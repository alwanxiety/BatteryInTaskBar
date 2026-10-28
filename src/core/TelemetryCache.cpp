#include "core/TelemetryCache.h"
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <string>
#include "core/AppState.h"
#include "hid/MouseProviders.h"

namespace {

constexpr wchar_t kCacheKey[] = L"Software\\BatteryInTaskBar\\Cache";
// Данные старше месяца уже не «последнее показание», а мусор.
constexpr std::uint64_t kMaxAgeMs = 30ULL * 24 * 60 * 60 * 1000;

std::uint64_t NowUnixMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

bool ReadDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    DWORD type = 0;
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) ==
               ERROR_SUCCESS &&
           type == REG_DWORD;
}

bool ReadQword(HKEY key, const wchar_t* name, std::uint64_t& value) {
    DWORD size = sizeof(value);
    DWORD type = 0;
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) ==
               ERROR_SUCCESS &&
           type == REG_QWORD;
}

std::wstring ReadString(HKEY key, const wchar_t* name) {
    DWORD size = 0;
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_SZ || size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &size) !=
        ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void WriteQword(HKEY key, const wchar_t* name, std::uint64_t value) {
    RegSetValueExW(key, name, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

void TelemetryCache::Restore() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kCacheKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;

    const std::uint64_t now = NowUnixMs();

    std::uint64_t mouseSuccess = 0;
    DWORD level = 0;
    DWORD charging = 0;
    DWORD estimate = 0;
    DWORD voltage = 0;
    DWORD hasData = 0;
    if (ReadQword(key, L"MouseLastSuccess", mouseSuccess) &&
        ReadDword(key, L"MouseLevel", level) && level <= 100 &&
        ReadDword(key, L"MouseHasData", hasData) && hasData != 0 &&
        mouseSuccess != 0 && now >= mouseSuccess && (now - mouseSuccess) <= kMaxAgeMs) {
        DeviceTelemetry telemetry;
        telemetry.deviceName = ReadString(key, L"MouseName");
        // В старых версиях в кэш могло попасть имя ресивера — на плитке это
        // выглядело как «USB Receiver» вместо мыши.
        if (LooksLikeReceiverName(telemetry.deviceName)) telemetry.deviceName.clear();
        telemetry.batteryLevel = static_cast<int>(level);
        ReadDword(key, L"MouseCharging", charging);
        telemetry.isCharging = charging != 0;
        ReadDword(key, L"MouseEstimate", estimate);
        telemetry.estimatedMinutes = static_cast<int>(estimate);
        ReadDword(key, L"MouseVoltage", voltage);
        telemetry.voltageMv = static_cast<int>(voltage);
        telemetry.hasData = true;
        AppState::Instance().RestoreTelemetry(telemetry, mouseSuccess);
    }

    std::uint64_t headsetSuccess = 0;
    std::uint64_t headsetSeen = 0;
    DWORD left = 0;
    DWORD right = 0;
    DWORD caseLevel = 0;
    DWORD leftCharging = 0;
    DWORD rightCharging = 0;
    DWORD caseCharging = 0;
    DWORD headsetHasData = 0;
    if (ReadQword(key, L"HeadsetLastSuccess", headsetSuccess) &&
        ReadDword(key, L"HeadsetHasData", headsetHasData) && headsetHasData != 0 &&
        headsetSuccess != 0 && now >= headsetSuccess && (now - headsetSuccess) <= kMaxAgeMs) {
        AirPodsTelemetry telemetry;
        telemetry.deviceName = ReadString(key, L"HeadsetName");
        ReadDword(key, L"HeadsetLeft", left);
        ReadDword(key, L"HeadsetRight", right);
        ReadDword(key, L"HeadsetCase", caseLevel);
        ReadDword(key, L"HeadsetLeftCharging", leftCharging);
        ReadDword(key, L"HeadsetRightCharging", rightCharging);
        ReadDword(key, L"HeadsetCaseCharging", caseCharging);
        telemetry.leftLevel = left <= 100 ? static_cast<int>(left) : -1;
        telemetry.rightLevel = right <= 100 ? static_cast<int>(right) : -1;
        telemetry.caseLevel = caseLevel <= 100 ? static_cast<int>(caseLevel) : -1;
        telemetry.leftCharging = leftCharging != 0;
        telemetry.rightCharging = rightCharging != 0;
        telemetry.caseCharging = caseCharging != 0;
        ReadQword(key, L"HeadsetLastSeen", headsetSeen);
        telemetry.lastSeenUnixMs = headsetSeen;
        telemetry.lastSuccessUnixMs = headsetSuccess;
        telemetry.hasData = true;
        // Связь после перезапуска не подтверждена: плитка появится только когда
        // Bluetooth-вотчер увидит подключение и придёт свежая реклама.
        telemetry.connected = false;
        AppState::Instance().UpdateAirPods(telemetry);
    }

    RegCloseKey(key);
}

void TelemetryCache::SaveFromState() {
    const AppState& state = AppState::Instance();
    const DeviceTelemetry mouse = state.GetTelemetry();
    const AirPodsTelemetry headset = state.GetAirPods();

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kCacheKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }

    WriteDword(key, L"Version", 1);
    WriteDword(key, L"MouseHasData", mouse.hasData ? 1 : 0);
    WriteDword(key, L"MouseLevel", static_cast<DWORD>(mouse.batteryLevel));
    WriteDword(key, L"MouseCharging", mouse.isCharging ? 1 : 0);
    WriteDword(key, L"MouseEstimate", static_cast<DWORD>(mouse.estimatedMinutes));
    WriteDword(key, L"MouseVoltage", static_cast<DWORD>(mouse.voltageMv));
    WriteString(key, L"MouseName", mouse.deviceName);
    WriteQword(key, L"MouseLastSuccess", state.LastSuccessUnixMs());

    WriteDword(key, L"HeadsetHasData", headset.hasData ? 1 : 0);
    WriteDword(key, L"HeadsetLeft", static_cast<DWORD>(headset.leftLevel));
    WriteDword(key, L"HeadsetRight", static_cast<DWORD>(headset.rightLevel));
    WriteDword(key, L"HeadsetCase", static_cast<DWORD>(headset.caseLevel));
    WriteDword(key, L"HeadsetLeftCharging", headset.leftCharging ? 1 : 0);
    WriteDword(key, L"HeadsetRightCharging", headset.rightCharging ? 1 : 0);
    WriteDword(key, L"HeadsetCaseCharging", headset.caseCharging ? 1 : 0);
    WriteString(key, L"HeadsetName", headset.deviceName);
    WriteQword(key, L"HeadsetLastSeen", headset.lastSeenUnixMs);
    WriteQword(key, L"HeadsetLastSuccess", headset.lastSuccessUnixMs);
    WriteQword(key, L"SavedAt", NowUnixMs());

    RegCloseKey(key);
}
