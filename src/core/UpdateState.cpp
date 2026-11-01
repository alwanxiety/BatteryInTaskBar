#include "core/UpdateState.h"

#include <windows.h>
#include <chrono>

namespace {

constexpr wchar_t kKeyPath[] = L"Software\\BatteryInTaskBar\\Update";

std::uint64_t NowUnixMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::wstring ReadString(HKEY key, const wchar_t* name) {
    DWORD size = 0;
    DWORD type = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, nullptr, &size) !=
            ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, value.data(),
                     &size) != ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

void WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

UpdateState::Info UpdateState::Read() {
    Info info;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return info;
    }

    DWORD available = 0;
    DWORD size = sizeof(available);
    DWORD type = 0;
    if (RegGetValueW(key, nullptr, L"Available", RRF_RT_REG_DWORD, &type,
                     &available, &size) == ERROR_SUCCESS) {
        info.available = available != 0;
    }
    info.latest = ReadString(key, L"Latest");
    info.error = ReadString(key, L"Error");

    ULONGLONG checked = 0;
    size = sizeof(checked);
    if (RegGetValueW(key, nullptr, L"CheckedAt", RRF_RT_REG_QWORD, &type,
                     &checked, &size) == ERROR_SUCCESS) {
        info.checkedAtUnixMs = checked;
    }

    RegCloseKey(key);
    if (info.latest.empty()) info.available = false;
    return info;
}

void UpdateState::Write(const Info& info) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    const DWORD available = info.available ? 1 : 0;
    RegSetValueExW(key, L"Available", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&available), sizeof(available));
    WriteString(key, L"Latest", info.latest);
    WriteString(key, L"Error", info.error);

    const ULONGLONG checked =
        info.checkedAtUnixMs != 0 ? info.checkedAtUnixMs : NowUnixMs();
    RegSetValueExW(key, L"CheckedAt", 0, REG_QWORD,
                   reinterpret_cast<const BYTE*>(&checked), sizeof(checked));

    RegCloseKey(key);
}
