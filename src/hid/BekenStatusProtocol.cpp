#include "hid/BekenStatusProtocol.h"

namespace BekenStatusProtocol {
namespace {

constexpr std::uint8_t kReportId = 0x03;
constexpr std::uint8_t kCmdBattery = 0x40;
constexpr std::uint8_t kCmdBatteryAlt = 0x41;

// Поколение X6/R1 у HaloBattery: 03 10 40 <стадия DPI> <заряд / 10>.
// У этого кадра нет состояния зарядки, а байт 3 — стадия, не подкоманда.
constexpr std::uint8_t kShapeX6Step = 0x10;

// Остальные модели семейства (ATKsoft, проверено на X6/X11): байт 1 — id
// модели, байт 2 — команда 0x40/0x41, байт 3 — состояние, байт 4 — процент.
constexpr std::uint8_t kModelX11 = 0x55;
constexpr std::uint8_t kModelX6 = 0x85;
constexpr std::uint8_t kModelX3 = 0x4D;
constexpr std::uint8_t kModelX3Max = 0x4E;
constexpr std::uint8_t kModelM3 = 0xBC;

constexpr std::uint8_t kStateNormal = 0x01;
constexpr std::uint8_t kStateFull = 0x02;
constexpr std::uint8_t kStateCharging = 0x03;

constexpr int kX6Step = 10;
constexpr int kLevelMin = 1;
constexpr int kLevelMax = 100;

bool IsInphicModel(std::uint8_t code) noexcept {
    return code == 0x95 || code == 0x90 || code == 0x93 || code == 0x99;
}

bool IsBekenModel(std::uint8_t code) noexcept {
    return code == kShapeX6Step || code == kModelX11 || code == kModelX6 ||
           code == kModelX3 || code == kModelX3Max || code == kModelM3;
}

bool IsKnownModel(std::uint16_t pid, std::uint8_t code) noexcept {
    if (pid == 0xFA60) return IsBekenModel(code);
    if (pid == 0xFA65) return IsInphicModel(code);
    return false;
}

}  // namespace

bool Parse(std::uint16_t pid, std::span<const std::uint8_t> frame,
           Reading& out) noexcept {
    // report id + модель + команда + состояние/стадия + уровень.
    if (frame.size() < 5) return false;
    if (frame[0] != kReportId) return false;
    if (frame[2] != kCmdBattery && frame[2] != kCmdBatteryAlt) return false;

    const std::uint8_t model = frame[1];
    if (!IsKnownModel(pid, model)) return false;

    if (model == kShapeX6Step) {
        const int level = static_cast<int>(frame[4]) * kX6Step;
        // 0 % в этом протоколе неотличим от «нет данных», а ложный ноль хуже
        // отсутствия плитки. Живая мышь присылает 10..100.
        if (level < kX6Step || level > kLevelMax) return false;
        out.level = level;
        out.charging = false;
        return true;
    }

    // ATKsoft: байт 3 — состояние (1 — от батареи, 2 — заряжено, 3 — на
    // кабеле), байт 4 — процент. У X11/Inphic тот же смысл, поэтому одна
    // ветка на всё семейство.
    const std::uint8_t state = frame[3];
    const int value = frame[4];
    if (state == kStateNormal) {
        if (value < kLevelMin || value > kLevelMax) return false;
        out.level = value;
        out.charging = false;
        return true;
    }
    if (state == kStateFull) {
        out.level = (value >= kLevelMin && value <= kLevelMax) ? value : kLevelMax;
        out.charging = true;
        return true;
    }
    if (state == kStateCharging) {
        // У части прошивок в кадре «на кабеле» процент нулевой: тогда
        // оставляем последний известный уровень (level = -1).
        out.level = (value >= kLevelMin && value <= kLevelMax) ? value : -1;
        out.charging = true;
        return true;
    }
    return false;
}

}  // namespace BekenStatusProtocol
