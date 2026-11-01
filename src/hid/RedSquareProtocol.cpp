#include "hid/RedSquareProtocol.h"

namespace RedSquareProtocol {

bool Parse(std::uint8_t command, const std::array<std::uint8_t, 17>& response,
           Reading& out) noexcept {
    // Эхо ответа: 0x08 — report id, второй байт — команда.
    if (response[0] != 0x08 || response[1] != command) return false;

    if (command == 0x04) {
        const int level = response[6];
        if (level > 100) return false;
        // Байт 7 — 0/1 (питание/кабель). Любое другое значение — это не ответ
        // о заряде, а случайный отчёт (движение мыши и т. п.).
        if (response[7] > 1) return false;

        out.level = level;
        out.charging = response[7] != 0;
        const int millivolts = (static_cast<int>(response[8]) << 8) | response[9];
        out.voltageMv = (millivolts >= 2500 && millivolts <= 5000) ? millivolts : -1;
        return true;
    }

    if (command == 0x17) {
        // Старый вариант: заряд отдаётся половинными шагами.
        const int level = response[5] * 2;
        if (level <= 0 || level > 100) return false;

        out.level = level;
        out.charging = false;
        out.voltageMv = -1;
        return true;
    }

    return false;
}

}  // namespace RedSquareProtocol
