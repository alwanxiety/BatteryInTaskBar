#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Разбор ответа ATK/VXE (17-байтовый vendor-отчёт 0x08). Вынесен отдельно от
// Windows-кода, чтобы проверять его на синтетических отчётах без реальной мыши.
namespace RedSquareProtocol {

struct Reading {
    int level = -1;       // -1 — ответ не похож на заряд
    bool charging = false;
    int voltageMv = -1;
};

/// @param command 0x04 (основной протокол) или 0x17 (старый, шаг 2 %).
/// @return false, если отчёт не является ответом о заряде (например, это
///         отчёт движения мыши, у которого совпал report id/байт команды).
[[nodiscard]] bool Parse(std::uint8_t command,
                         const std::array<std::uint8_t, 17>& response,
                         Reading& out) noexcept;

/// Feature report 0x06 (usage page 0xFF04): байт 3 — процент, байты 6..7 —
/// напряжение little-endian. Отчёт без правдоподобного напряжения не считается
/// батарейным: у VXE R1 Pro Max он иногда отдаёт «100 %» и мусор в напряжении.
[[nodiscard]] bool ParseFeature(const std::uint8_t* data, std::size_t size,
                                Reading& out) noexcept;

}  // namespace RedSquareProtocol
