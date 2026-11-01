#include "hid/BekenStatusProtocol.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::vector<std::uint8_t> Frame(std::initializer_list<std::uint8_t> bytes) {
    return std::vector<std::uint8_t>(bytes);
}

void ExpectRejected(std::uint16_t pid, const std::vector<std::uint8_t>& frame,
                    const char* what) {
    BekenStatusProtocol::Reading reading;
    Check(!BekenStatusProtocol::Parse(pid, frame, reading), what);
}

}  // namespace

int main() {
    using BekenStatusProtocol::Parse;
    using BekenStatusProtocol::Reading;

    // Поколение X6/R1 у HaloBattery: 03 10 40 <стадия> <заряд/10>.
    {
        const auto frame = Frame({ 0x03, 0x10, 0x40, 0x01, 0x0A });  // 10 * 10 = 100
        Reading reading;
        Check(Parse(0xFA60, frame, reading), "X6 step shape: 100% accepted");
        Check(reading.level == 100, "X6 step shape: level = byte4 * 10");
        Check(!reading.charging, "X6 step shape: no charging state");
    }
    {
        const auto frame = Frame({ 0x03, 0x10, 0x40, 0x01, 0x09 });  // 90
        Reading reading;
        Check(Parse(0xFA60, frame, reading) && reading.level == 90, "X6 step shape: 90%");
    }
    {
        const auto frame = Frame({ 0x03, 0x10, 0x40, 0x01, 0x01 });  // 10
        Reading reading;
        Check(Parse(0xFA60, frame, reading) && reading.level == 10, "X6 step shape: 10%");
    }
    // 0 % неотличим от «нет данных» — лучше промолчать, чем показать ложный ноль.
    ExpectRejected(0xFA60, Frame({ 0x03, 0x10, 0x40, 0x01, 0x00 }),
                   "X6 step shape: zero rejected");
    ExpectRejected(0xFA60, Frame({ 0x03, 0x10, 0x40, 0x01, 0x0B }),
                   "X6 step shape: level above 100 rejected");
    // ACK конфигурации и DPI-кнопка делят форму, но не команду 0x40/0x41.
    ExpectRejected(0xFA60, Frame({ 0x03, 0x10, 0x50, 0x01, 0x0A }),
                   "X6 step shape: config ACK rejected");
    ExpectRejected(0xFA60, Frame({ 0x03, 0x10, 0x10, 0x01, 0x0A }),
                   "X6 step shape: DPI button report rejected");

    // X6/X3/X11 по ATKsoft: 03 <модель> 40 <состояние> <процент>.
    // 0x85 — X6; процент лежит прямо в байте 4.
    {
        const auto frame = Frame({ 0x03, 0x85, 0x40, 0x01, 0x1F });  // 31 %
        Reading reading;
        Check(Parse(0xFA60, frame, reading), "X6 model: level accepted");
        Check(reading.level == 31, "X6 model: level parsed directly");
        Check(!reading.charging, "X6 model: on battery");
    }
    {
        const auto frame = Frame({ 0x03, 0x85, 0x40, 0x03, 0x1E });  // 30 %, на кабеле
        Reading reading;
        Check(Parse(0xFA60, frame, reading), "X6 model: charging frame accepted");
        Check(reading.level == 30, "X6 model: charging level parsed");
        Check(reading.charging, "X6 model: charging flag");
    }
    {
        const auto frame = Frame({ 0x03, 0x85, 0x40, 0x02, 0x64 });  // 100 %, заряжено
        Reading reading;
        Check(Parse(0xFA60, frame, reading) && reading.level == 100 && reading.charging,
              "X6 model: full frame");
    }
    {
        // 0x41 — альтернативная команда батареи; раскладка та же.
        const auto frame = Frame({ 0x03, 0x85, 0x41, 0x01, 0x50 });  // 80 %
        Reading reading;
        Check(Parse(0xFA60, frame, reading) && reading.level == 80,
              "X6 model: battery alt command");
    }
    {
        // На кабеле процент может быть нулевым — уровень оставляем прежним.
        const auto frame = Frame({ 0x03, 0x85, 0x40, 0x03, 0x00 });
        Reading reading;
        Check(Parse(0xFA60, frame, reading) && reading.level == -1 && reading.charging,
              "X6 model: charging without level");
    }
    {
        const auto frame = Frame({ 0x03, 0x4D, 0x40, 0x01, 0x2A });  // X3, 42 %
        Reading reading;
        Check(Parse(0xFA60, frame, reading) && reading.level == 42, "X3 model: level");
    }
    {
        const auto frame = Frame({ 0x03, 0xBC, 0x40, 0x01, 0x14 });  // M3, 20 %
        Reading reading;
        Check(Parse(0xFA60, frame, reading) && reading.level == 20, "M3 model: level");
    }

    // FA61 — тот же X6, но по USB-кабелю. Кадры, если приходят, того же вида.
    {
        const auto frame = Frame({ 0x03, 0x85, 0x40, 0x01, 0x1F });  // 31 %
        Reading reading;
        Check(Parse(0xFA61, frame, reading) && reading.level == 31,
              "FA61 cable: X6 model level");
    }
    {
        const auto frame = Frame({ 0x03, 0x10, 0x40, 0x01, 0x0A });  // 100 %
        Reading reading;
        Check(Parse(0xFA61, frame, reading) && reading.level == 100,
              "FA61 cable: step shape");
    }

    // X11/Inphic: 03 55 40 01 <level>.
    {
        const auto frame = Frame({ 0x03, 0x55, 0x40, 0x01, 0x1F });  // 31%
        Reading reading;
        Check(Parse(0xFA60, frame, reading), "X11: level accepted");
        Check(reading.level == 31, "X11: level parsed");
        Check(!reading.charging, "X11: on battery");
    }
    {
        const auto frame = Frame({ 0x03, 0x55, 0x40, 0x03, 0x1E });
        Reading reading;
        Check(Parse(0xFA60, frame, reading), "X11: charging frame accepted");
        Check(reading.level == 30, "X11: charging level parsed");
        Check(reading.charging, "X11: charging flag");
    }
    {
        const auto frame = Frame({ 0x03, 0x55, 0x40, 0x02, 0x00 });
        Reading reading;
        Check(Parse(0xFA60, frame, reading), "X11: full frame accepted");
        Check(reading.level == 100 && reading.charging, "X11: full = 100% charging");
    }
    ExpectRejected(0xFA60, Frame({ 0x03, 0x55, 0x40, 0x01, 0x00 }),
                   "X11: zero level rejected");
    ExpectRejected(0xFA60, Frame({ 0x03, 0x55, 0x40, 0x01, 0x65 }),
                   "X11: level above 100 rejected");
    ExpectRejected(0xFA60, Frame({ 0x03, 0x55, 0x40, 0x00, 0x1F }),
                   "X11: unknown state rejected");

    // Inphic на своём ресивере: 03 95 40 01 4b = 75 %.
    {
        const auto frame = Frame({ 0x03, 0x95, 0x40, 0x01, 0x4B });
        Reading reading;
        Check(Parse(0xFA65, frame, reading) && reading.level == 75,
              "Inphic: level parsed");
    }
    // Коды моделей не смешиваются между ресиверами.
    ExpectRejected(0xFA60, Frame({ 0x03, 0x95, 0x40, 0x01, 0x4B }),
                   "FA60 does not accept Inphic model code");
    ExpectRejected(0xFA65, Frame({ 0x03, 0x10, 0x40, 0x01, 0x0A }),
                   "FA65 does not accept X6 model code");
    ExpectRejected(0xFA65, Frame({ 0x03, 0x85, 0x40, 0x01, 0x1F }),
                   "FA65 does not accept X6 model code 0x85");

    // Общие проверки формы.
    ExpectRejected(0xFA60, Frame({ 0x04, 0x85, 0x40, 0x01, 0x1F }),
                   "wrong report id rejected");
    ExpectRejected(0xFA60, Frame({ 0x03, 0x85, 0x50, 0x01, 0x1F }),
                   "wrong command rejected");
    ExpectRejected(0xFA60, Frame({ 0x03, 0x85, 0x40, 0x01 }),
                   "short frame rejected");
    ExpectRejected(0xFA60, Frame({ 0x03, 0x85, 0x40 }),
                   "very short frame rejected");
    ExpectRejected(0xFA60, Frame({ 0x03, 0x42, 0x40, 0x01, 0x1F }),
                   "unknown model rejected");

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all Beken status protocol checks passed\n");
    return 0;
}
