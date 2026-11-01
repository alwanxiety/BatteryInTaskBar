#include "hid/BekenStatusProtocol.h"

namespace BekenStatusProtocol {
namespace {

constexpr std::uint8_t kReportId = 0x03;
constexpr std::uint8_t kCmdBattery = 0x40;
constexpr std::uint8_t kShapeX6 = 0x10;    // X6/R1: уровень шагами по 10 %
constexpr std::uint8_t kShapeX11 = 0x55;   // X11/Inphic: подкоманда + уровень

constexpr std::uint8_t kSubLevel = 0x01;
constexpr std::uint8_t kSubFull = 0x02;
constexpr std::uint8_t kSubCharging = 0x03;
constexpr int kX6Step = 10;

bool IsInphicModel(std::uint8_t code) noexcept {
    return code == 0x95 || code == 0x90 || code == 0x93 || code == 0x99;
}

bool IsKnownModel(std::uint16_t pid, std::uint8_t code) noexcept {
    if (pid == 0xFA60) return code == kShapeX6 || code == kShapeX11;
    if (pid == 0xFA65) return IsInphicModel(code);
    return false;
}

}  // namespace

bool Parse(std::uint16_t pid, std::span<const std::uint8_t> frame,
           Reading& out) noexcept {
    // report id + модель + команда + подкоманда/стадия + уровень.
    if (frame.size() < 5) return false;
    if (frame[0] != kReportId || frame[2] != kCmdBattery) return false;

    const std::uint8_t model = frame[1];
    if (!IsKnownModel(pid, model)) return false;

    if (model == kShapeX6) {
        const int level = static_cast<int>(frame[4]) * kX6Step;
        // 0 % в этом протоколе неотличим от «нет данных», а ложный ноль хуже
        // отсутствия плитки. Живая мышь присылает 10..100.
        if (level < kX6Step || level > 100) return false;
        out.level = level;
        out.charging = false;
        return true;
    }

    const std::uint8_t sub = frame[3];
    if (sub == kSubCharging) {
        out.level = -1;  // уровень остаётся от предыдущего полного кадра
        out.charging = true;
        return true;
    }
    if (sub == kSubFull) {
        out.level = 100;
        out.charging = true;
        return true;
    }
    if (sub == kSubLevel) {
        const int level = frame[4];
        if (level < 1 || level > 100) return false;
        out.level = level;
        out.charging = false;
        return true;
    }
    return false;
}

}  // namespace BekenStatusProtocol
