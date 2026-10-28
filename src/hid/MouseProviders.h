#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <array>
#include <span>
#include <vector>
#include "core/AppState.h"

// Провайдеры HID устройств
struct _HIDP_CAPS;
typedef _HIDP_CAPS HIDP_CAPS;

/// Прерывание текущего цикла опроса. Провайдеры проверяют флаг внутри своих
/// переборов (индексы Logitech, повторные команды), чтобы не тратить таймауты,
/// когда приложение уже ушло в полноэкранный режим. Флаг общий: опрос идёт
/// одним потоком.
namespace PollAbort {
void Set(bool requested) noexcept;
[[nodiscard]] bool Requested() noexcept;
}

class IMouseProvider {
public:
    virtual ~IMouseProvider() = default;
    [[nodiscard]] virtual bool Match(uint16_t vid, uint16_t pid) const noexcept = 0;
    /// Дополнительная проверка по HID-описанию интерфейса: нужна провайдерам,
    /// которые цепляются не по VID/PID, а по наличию поля в отчёте (Battery Strength).
    [[nodiscard]] virtual bool MatchDevice(HANDLE, const HIDP_CAPS&) noexcept { return true; }
    virtual bool Query(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) = 0;
    [[nodiscard]] virtual const wchar_t* Name() const noexcept = 0;
};

class LogitechProvider : public IMouseProvider {
public:
    [[nodiscard]] bool Match(uint16_t vid, uint16_t) const noexcept override;
    bool Query(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) override;
    [[nodiscard]] const wchar_t* Name() const noexcept override { return L"Logitech"; }
private:
    struct ReportSizes { DWORD inputLen; DWORD outputLen; };
    struct BatteryCache {
        uint16_t pid = 0;
        uint8_t deviceIndex = 0;
        uint8_t unifiedIndex = 0;
        uint8_t legacyIndex = 0;
        uint8_t voltageIndex = 0;
        uint8_t nameIndex = 0;
        enum class BatteryType : uint8_t { None, Unified, Legacy, Voltage } type = BatteryType::None;
        std::wstring name;
        // Сглаженный уровень заряда храним как double: целочисленное округление
        // создавало «неподвижную точку» и заряд мог навсегда застрять на одном значении.
        double smoothedLevel = -1.0;
        // Спящая мышь молчит: повторное перечисление feature-индексов стоит
        // несколько таймаутов, поэтому после неудачи выдерживаем паузу.
        ULONGLONG retryDiscoveryTick = 0;
    };
    std::array<BatteryCache, 16> m_cache{};
    BatteryCache* FindCache(uint16_t pid, uint8_t devIdx) noexcept;
    static int VoltageToPercent(int voltageMv) noexcept;
    bool QueryCachedOrDiscover(HANDLE hDev, const ReportSizes& sizes, uint16_t pid, uint8_t devIdx, DeviceTelemetry& outData);
    static bool SendPacket(HANDLE hDev, const ReportSizes& sizes, uint8_t devIdx, uint8_t featIdx, uint8_t fnId, std::span<const uint8_t> params, std::span<uint8_t, 20> outResponse) noexcept;
    static uint8_t GetFeatureIndex(HANDLE hDev, const ReportSizes& sizes, uint8_t devIdx, uint16_t featureId) noexcept;
    static std::wstring QueryDeviceNameWithIndex(HANDLE hDev, const ReportSizes& sizes, uint8_t devIdx, uint8_t fNameIdx) noexcept;
};

class RazerProvider : public IMouseProvider {
public:
    [[nodiscard]] bool Match(uint16_t vid, uint16_t) const noexcept override;
    bool Query(HANDLE hDev, const HIDP_CAPS&, DeviceTelemetry& outData) override;
    [[nodiscard]] const wchar_t* Name() const noexcept override { return L"Razer"; }
};

class CompxNordicProvider : public IMouseProvider {
public:
    [[nodiscard]] bool Match(uint16_t vid, uint16_t) const noexcept override;
    [[nodiscard]] bool MatchDevice(HANDLE hDev, const HIDP_CAPS& caps) noexcept override;
    bool Query(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) override;
    [[nodiscard]] const wchar_t* Name() const noexcept override { return L"CompxNordic"; }
};

// VXE / Red Square (VXE R1 и родственные), а также ATK и VGN на Nordic:
// 17-байтовый vendor-отчёт 0x08 (cmd 0x04 или 0x17), плюс fallback через
// feature report 0x06 у части 1K-ресиверов.
class RedSquareProvider : public IMouseProvider {
public:
    [[nodiscard]] bool Match(uint16_t vid, uint16_t pid) const noexcept override;
    bool Query(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) override;
    [[nodiscard]] const wchar_t* Name() const noexcept override { return L"RedSquare"; }
private:
    enum class BatteryCommand : uint8_t { Unknown, Full, Legacy };
    static bool Exchange17(HANDLE hDev, uint8_t command, std::array<uint8_t, 17>& response) noexcept;
    static bool ParseBattery17(uint8_t command, const std::array<uint8_t, 17>& response, DeviceTelemetry& outData) noexcept;
    static bool QueryFeatureBattery(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) noexcept;
    static const wchar_t* ModelName(uint16_t vid, uint16_t pid) noexcept;
    BatteryCommand m_command = BatteryCommand::Unknown;
};

class BluetoothProvider {
public:
    static bool Query(DeviceTelemetry& outData);
};

// Заряд, который Windows сам знает про устройство (PnP-свойство батареи).
// Ловит мыши, для которых нет vendor-протокола: HID-класс, USB-ресиверы и
// Bluetooth. Идёт последним шансом после всех HID-провайдеров.
class PnpBatteryProvider {
public:
    static bool Query(DeviceTelemetry& outData);
};

// Стандартная страница HID Generic Device Controls, usage Battery Strength (0x06/0x20).
// Многие мыши, клавиатуры и ресиверы отдают заряд именно так — без vendor-протокола.
// Цепляется по описанию интерфейса, поэтому в списке провайдеров идёт последним.
class GenericBatteryProvider : public IMouseProvider {
public:
    [[nodiscard]] bool Match(uint16_t, uint16_t) const noexcept override { return true; }
    [[nodiscard]] bool MatchDevice(HANDLE hDev, const HIDP_CAPS& caps) noexcept override;
    bool Query(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) override;
    [[nodiscard]] const wchar_t* Name() const noexcept override { return L"GenericBattery"; }
};
