#pragma once
#include <cstdint>
#include <span>

// Кадры заряда семейства Beken/Inphic на ресиверах 1d57:fa60 / 1d57:fa65.
// Мышь сама присылает их в interrupt-IN на коллекции usage 0x000A:0x0000:
//
//   03 10 40 <стадия DPI> <заряд / 10>   — поколение X6/R1
//   03 55 40 01 <заряд>                  — X11, разряжается
//   03 55 40 02 ..                       — X11, заряжено
//   03 55 40 03 ..                       — X11, на кабеле
//
// В кадрах X6/R1 состояния зарядки нет: байт 3 — это стадия DPI, а не
// подкоманда. Разбор вынесен отдельно от Windows-кода, чтобы проверять его
// на синтетических кадрах без реальной мыши.
namespace BekenStatusProtocol {

struct Reading {
    int level = -1;         // -1 — кадр несёт только состояние зарядки
    bool charging = false;
};

/// @param pid 0xFA60 (Attack Shark X6/X11/R1) или 0xFA65 (Inphic In9 Pro).
/// @param frame входной отчёт вместе с report id (Windows отдаёт его первым).
/// @return false, если это не кадр заряда (ACK конфигурации, DPI-кнопка и т. п.).
[[nodiscard]] bool Parse(std::uint16_t pid, std::span<const std::uint8_t> frame,
                         Reading& out) noexcept;

}  // namespace BekenStatusProtocol
