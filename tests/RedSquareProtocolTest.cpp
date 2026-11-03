#include "hid/RedSquareProtocol.h"

#include <array>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::array<std::uint8_t, 17> Report(std::uint8_t command) {
    std::array<std::uint8_t, 17> report{};
    report[0] = 0x08;
    report[1] = command;
    return report;
}

void ExpectRejected(std::uint8_t command, const std::array<std::uint8_t, 17>& report,
                    const char* what) {
    RedSquareProtocol::Reading reading;
    Check(!RedSquareProtocol::Parse(command, report, reading), what);
}

}  // namespace

int main() {
    using RedSquareProtocol::Parse;
    using RedSquareProtocol::Reading;

    // Обычный ответ 0x04: 85 %, от батареи, 4200 мВ.
    {
        auto report = Report(0x04);
        report[6] = 85;
        report[7] = 0;
        report[8] = 0x10;
        report[9] = 0x68;
        Reading reading;
        Check(Parse(0x04, report, reading), "0x04: valid answer accepted");
        Check(reading.level == 85, "0x04: level parsed");
        Check(!reading.charging, "0x04: not charging");
        Check(reading.voltageMv == 4200, "0x04: voltage parsed");
    }

    // Ответ 0x04 во время зарядки.
    {
        auto report = Report(0x04);
        report[6] = 60;
        report[7] = 1;
        Reading reading;
        Check(Parse(0x04, report, reading) && reading.charging, "0x04: charging flag");
    }

    // Отчёт движения мыши, у которого совпал report id и «команда» (кнопка 0x04):
    // байт 7 — это дельта Y, а не флаг зарядки. Раньше такой отчёт принимался
    // за заряд и мог дать случайные 100 %.
    {
        auto report = Report(0x04);
        report[6] = 100;  // дельта X
        report[7] = 37;   // дельта Y
        ExpectRejected(0x04, report, "0x04: motion report is rejected");
    }

    // Заряд больше 100 — не ответ о заряде.
    {
        auto report = Report(0x04);
        report[6] = 101;
        report[7] = 0;
        ExpectRejected(0x04, report, "0x04: level above 100 rejected");
    }

    // 0 % — валидное значение (полностью разряжено).
    {
        auto report = Report(0x04);
        report[6] = 0;
        report[7] = 0;
        Reading reading;
        Check(Parse(0x04, report, reading) && reading.level == 0, "0x04: zero percent accepted");
    }

    // Чужое эхо: команда в отчёте не совпадает с запрошенной.
    {
        auto report = Report(0x17);
        report[6] = 85;
        ExpectRejected(0x04, report, "0x04: wrong echo rejected");
    }

    // Старый протокол 0x17: половинные шаги.
    {
        auto report = Report(0x17);
        report[5] = 43;  // 86 %
        Reading reading;
        Check(Parse(0x17, report, reading) && reading.level == 86, "0x17: half steps parsed");
        Check(!reading.charging, "0x17: never charging");
    }

    // 0 и >50 половинных шагов — не заряд.
    {
        auto report = Report(0x17);
        report[5] = 0;
        ExpectRejected(0x17, report, "0x17: zero half steps rejected");
        report[5] = 51;  // 102 %
        ExpectRejected(0x17, report, "0x17: level above 100 rejected");
    }

    // Неизвестная команда.
    {
        auto report = Report(0x04);
        ExpectRejected(0x05, report, "unknown command rejected");
    }

    // Напряжение вне диапазона не подставляется.
    {
        auto report = Report(0x04);
        report[6] = 50;
        report[7] = 0;
        report[8] = 0x00;
        report[9] = 0x10;  // 16 мВ
        Reading reading;
        Check(Parse(0x04, report, reading) && reading.voltageMv == -1,
              "0x04: implausible voltage ignored");
    }

    // Feature report 0x06: обычный отчёт с зарядом и напряжением.
    {
        std::array<std::uint8_t, 8> report{ 0x06, 0x00, 0x00, 50, 0, 0, 0x3C, 0x0F };  // 3900 мВ
        RedSquareProtocol::Reading reading;
        Check(RedSquareProtocol::ParseFeature(report.data(), report.size(), reading),
              "feature 0x06: valid answer accepted");
        Check(reading.level == 50, "feature 0x06: level parsed");
        Check(reading.voltageMv == 3900, "feature 0x06: voltage parsed");
        Check(!reading.charging, "feature 0x06: never charging");
    }

    // Полный заряд с правдоподобным напряжением — валиден.
    {
        std::array<std::uint8_t, 8> report{ 0x06, 0x00, 0x00, 100, 0, 0, 0x36, 0x10 };  // 4150 мВ
        RedSquareProtocol::Reading reading;
        Check(RedSquareProtocol::ParseFeature(report.data(), report.size(), reading) &&
                  reading.level == 100 && reading.voltageMv == 4150,
              "feature 0x06: 100% with plausible voltage accepted");
    }

    // VXE R1 Pro Max: 100 %, но байты 6..7 дают 51301 мВ. Это не батарейный
    // отчёт; раньше он принимался и залипал на 100 % до перезапуска виджета.
    {
        std::array<std::uint8_t, 8> report{ 0x06, 0x00, 0x00, 0x64, 0x64, 0x64, 0x65, 0xC8 };
        RedSquareProtocol::Reading reading;
        Check(!RedSquareProtocol::ParseFeature(report.data(), report.size(), reading),
              "feature 0x06: false 100% with bogus voltage rejected");
    }

    // Короткий отчёт, чужой id и заряд больше 100 — не заряд.
    {
        std::array<std::uint8_t, 8> report{ 0x06, 0x00, 0x00, 50, 0, 0, 0x3C, 0x0F };
        RedSquareProtocol::Reading reading;
        Check(!RedSquareProtocol::ParseFeature(report.data(), 7, reading),
              "feature 0x06: short report rejected");
        report[0] = 0x07;
        Check(!RedSquareProtocol::ParseFeature(report.data(), report.size(), reading),
              "feature 0x06: wrong report id rejected");
        report[0] = 0x06;
        report[3] = 101;
        Check(!RedSquareProtocol::ParseFeature(report.data(), report.size(), reading),
              "feature 0x06: level above 100 rejected");
    }

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all RedSquare protocol checks passed\n");
    return 0;
}
