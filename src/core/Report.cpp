#include "core/Report.h"
#include "airpods/AirPodsMonitor.h"
#include "core/AppPaths.h"
#include "core/Diagnostics.h"
#include "core/Localization.h"
#include "res/app_version.h"
#include "tools/HidDump.h"
#include <cstdarg>
#include <chrono>
#include <cwchar>
#include <vector>

namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\BatteryInTaskBar";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"BatteryInTaskBar";

void Append(std::wstring& out, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int length = _vscwprintf(format, args);
    va_end(args);
    if (length < 0) return;

    // Размер считаем заранее: фиксированного буфера здесь быть не должно. Хвост
    // журнала легко перерастает 4 КБ, а vswprintf_s при нехватке места уходит в
    // fail-fast — это уронило бы весь Explorer вместе с отчётом.
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    va_start(args, format);
    vswprintf_s(buffer.data(), buffer.size(), format, args);
    va_end(args);

    buffer.resize(static_cast<std::size_t>(length));
    out += buffer;
}

std::wstring RegString(HKEY root, const wchar_t* subKey, const wchar_t* name) {
    wchar_t value[512]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(root, subKey, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
    return value;
}

bool RegDword(HKEY root, const wchar_t* subKey, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(root, subKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

const wchar_t* ThemeName(int theme) {
    switch (theme) {
    case 1: return L"Dark";
    case 2: return L"Light";
    default: return L"Glass";
    }
}

/// Версия ядра и редакция Windows. GetVersionEx врёт без манифеста,
/// поэтому спрашиваем ntdll напрямую.
std::wstring OsVersionText() {
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) fn(&info);
    }

    std::wstring text;
    wchar_t buffer[160]{};
    swprintf_s(buffer, L"Windows %lu.%lu build %lu", info.dwMajorVersion, info.dwMinorVersion,
               info.dwBuildNumber);
    text = buffer;

    const std::wstring release = RegString(HKEY_LOCAL_MACHINE,
                                           L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                           L"DisplayVersion");
    if (!release.empty()) text += L" (" + release + L")";
    text += (sizeof(void*) == 8) ? L" x64" : L" x86";
    return text;
}

/// Хвост журнала в UTF-16LE: целиком или последние ~256 КБ, чтобы отчёт
/// оставался разумного размера даже после ротации.
std::wstring ReadLogTail() {
    const std::wstring path = Diag::LogPath();
    if (path.empty()) return L"(journal is not initialized)\r\n";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return L"(cannot open journal, error " + std::to_wstring(GetLastError()) + L")\r\n";
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart == 0) {
        CloseHandle(file);
        return L"(journal is empty)\r\n";
    }

    constexpr LONGLONG kMaxBytes = 256 * 1024;
    LONGLONG offset = (size.QuadPart > kMaxBytes) ? (size.QuadPart - kMaxBytes) : 0;
    if (offset % 2 != 0) ++offset;  // UTF-16 читаем по границе символа

    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return L"(cannot seek journal)\r\n";
    }

    const DWORD toRead = static_cast<DWORD>(size.QuadPart - offset);
    std::vector<wchar_t> buffer(static_cast<std::size_t>(toRead / sizeof(wchar_t)) + 1, L'\0');
    DWORD read = 0;
    const bool ok = ReadFile(file, buffer.data(), toRead, &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!ok || read < sizeof(wchar_t)) return L"(cannot read journal)\r\n";

    std::wstring text(buffer.data(), static_cast<std::size_t>(read / sizeof(wchar_t)));
    if (!text.empty() && text.front() == L'\uFEFF') text.erase(0, 1);
    if (offset > 0) text = L"(... beginning skipped ...)\r\n" + text;
    return text;
}

/// Unix-мс → локальное «ГГГГ-ММ-ДД ЧЧ:ММ:СС». Пустая строка, если время не переводится.
std::wstring LocalTimeText(std::uint64_t unixMs) {
    constexpr std::uint64_t kEpochOffset100ns = 116444736000000000ULL;  // 1601 → 1970
    const ULONGLONG ticks = unixMs * 10000ULL + kEpochOffset100ns;

    FILETIME utc{};
    utc.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
    utc.dwHighDateTime = static_cast<DWORD>(ticks >> 32);

    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return {};

    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

/// Возраст данных для шапки отчёта: отчёт всегда на английском, поэтому и здесь
/// английские формы (в меню возраст локализован отдельно).
std::wstring PollAgeText(std::int64_t seconds) {
    if (seconds < 0) return L"never polled";
    if (seconds < 60) return L"just now";
    if (seconds < 3600) return std::to_wstring(seconds / 60) + L" min ago";
    if (seconds < 86400) return std::to_wstring(seconds / 3600) + L" h ago";
    return std::to_wstring(seconds / 86400) + L" d ago";
}

std::uint64_t NowUnixMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

/// Возраст цикла опроса: здесь нужны секунды, «just now» скрывал бы 20–50 с.
std::wstring CycleAgeText(std::int64_t seconds) {
    if (seconds < 0) return L"unknown";
    if (seconds < 60) return std::to_wstring(seconds) + L" s ago";
    if (seconds < 3600) return std::to_wstring(seconds / 60) + L" min ago";
    if (seconds < 86400) return std::to_wstring(seconds / 3600) + L" h ago";
    return std::to_wstring(seconds / 86400) + L" d ago";
}

/// Что именно случилось с попыткой: «молчит» и «не открылась» — разные проблемы.
const wchar_t* FailureText(const PollAttempt& attempt) {
    if (attempt.ok) return L"ok";
    switch (attempt.failure) {
    case PollAttempt::Failure::Open: return L"open failed";
    case PollAttempt::Failure::NoCaps: return L"no caps";
    case PollAttempt::Failure::NoAnswer: return L"no answer";
    case PollAttempt::Failure::None: return L"ok";
    }
    return L"?";
}

/// Что пробовал последний цикл опроса: кто ответил, сколько заняли попытки.
/// Отвечает на вопрос «HID-дамп устройство видит, а виджет молчит — почему?».
std::wstring PollTraceText(const PollTrace& trace) {
    if (!trace.valid) return L"  no poll has run yet\r\n";

    const std::uint64_t now = NowUnixMs();
    const std::int64_t age = (now > trace.finishedUnixMs)
                                 ? static_cast<std::int64_t>((now - trace.finishedUnixMs) / 1000)
                                 : 0;

    std::wstring text;
    Append(text, L"  last cycle: %ls (%ls), %d ms, result: %ls\r\n",
           LocalTimeText(trace.finishedUnixMs).c_str(), CycleAgeText(age).c_str(), trace.totalMs,
           trace.aborted ? L"aborted" : (trace.succeeded ? L"answered" : L"no answer"));

    if (!trace.selectedPath.empty()) {
        Append(text, L"  selected: %ls, attempt %d, %d ms\r\n", trace.selectedProvider.c_str(),
               trace.selectedAttempt, trace.selectedMs);
        Append(text, L"    path: %ls\r\n", trace.selectedPath.c_str());
    } else if (trace.bluetoothAnswered) {
        Append(text, L"  selected: PnP battery\r\n");
    }

    Append(text, L"  attempts: %zu\r\n", trace.attempts.size());
    int number = 1;
    for (const PollAttempt& attempt : trace.attempts) {
        Append(text, L"    %d. %-14ls %-11ls %5d ms  %ls\r\n", number++, attempt.provider.c_str(),
               FailureText(attempt), attempt.elapsedMs, attempt.path.c_str());
    }

    if (trace.bluetoothTried) {
        Append(text, L"  pnp battery: %ls\r\n", trace.bluetoothAnswered ? L"answered" : L"no answer");
    }
    return text;
}

std::wstring TelemetryText(const DeviceTelemetry& telemetry) {
    const std::wstring level = (telemetry.batteryLevel >= 0)
                                   ? std::to_wstring(telemetry.batteryLevel) + L"%"
                                   : std::wstring(L"unknown");
    const std::wstring voltage = (telemetry.voltageMv > 0)
                                     ? std::to_wstring(telemetry.voltageMv) + L" mV"
                                     : std::wstring(L"unknown");
    const std::wstring estimate = (telemetry.estimatedMinutes > 0)
                                      ? std::to_wstring(telemetry.estimatedMinutes) + L" min"
                                      : std::wstring(L"unknown");

    std::wstring text;
    Append(text, L"  device: %ls\r\n",
           telemetry.deviceName.empty() ? L"(unknown)" : telemetry.deviceName.c_str());
    Append(text, L"  level: %ls  charging: %ls  voltage: %ls  estimate: %ls  hasData: %ls\r\n",
           level.c_str(), telemetry.isCharging ? L"yes" : L"no", voltage.c_str(),
           estimate.c_str(), telemetry.hasData ? L"yes" : L"no");
    return text;
}

}  // namespace

std::wstring Report::Write(HWND widget, const Snapshot& snapshot) {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    std::wstring out;
    Append(out, L"BatteryInTaskBar - diagnostics report\r\n");
    Append(out, L"Generated: %04u-%02u-%02u %02u:%02u:%02u\r\n",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Append(out, L"Version: %ls\r\n", APP_VERSION_WSTRING);
    Append(out, L"Language: %ls\r\n", L10n::LanguageCode());
    Append(out, L"OS: %ls\r\n", OsVersionText().c_str());
    Append(out, L"Explorer PID: %lu\r\n", GetCurrentProcessId());
    Append(out, L"Monitors: %d\r\n", GetSystemMetrics(SM_CMONITORS));

    if (widget && IsWindow(widget)) {
        RECT rc{};
        GetWindowRect(widget, &rc);
        const UINT dpi = GetDpiForWindow(widget);
        Append(out, L"Widget: %ldx%ld at (%ld,%ld), DPI %u\r\n",
               static_cast<long>(rc.right - rc.left), static_cast<long>(rc.bottom - rc.top),
               static_cast<long>(rc.left), static_cast<long>(rc.top), dpi);
    } else {
        Append(out, L"Widget: window not available\r\n");
    }

    if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        RECT rc{};
        GetWindowRect(taskbar, &rc);
        Append(out, L"Taskbar: %ldx%ld at (%ld,%ld)\r\n",
               static_cast<long>(rc.right - rc.left), static_cast<long>(rc.bottom - rc.top),
               static_cast<long>(rc.left), static_cast<long>(rc.top));
    } else {
        Append(out, L"Taskbar: Shell_TrayWnd not found\r\n");
    }

    DWORD storedTheme = 0;
    const bool haveStoredTheme = RegDword(HKEY_CURRENT_USER, kSettingsKey, L"Theme", storedTheme);
    const std::wstring storedThemeText = haveStoredTheme ? std::to_wstring(storedTheme)
                                                         : std::wstring(L"(not set)");
    const std::wstring storedLang = RegString(HKEY_CURRENT_USER, kSettingsKey, L"Lang");
    const std::wstring autoRun = RegString(HKEY_CURRENT_USER, kRunKey, kRunValueName);

    Append(out, L"Theme: %d (%ls), system dark: %ls, stored: %ls\r\n", snapshot.theme,
           ThemeName(snapshot.theme), snapshot.systemDark ? L"yes" : L"no",
           storedThemeText.c_str());
    Append(out, L"Language override: %ls\r\n", storedLang.empty() ? L"(none)" : storedLang.c_str());
    Append(out, L"Autorun: %ls\r\n", autoRun.empty() ? L"disabled" : L"enabled");
    Append(out, L"Data directory: %ls\r\n", AppPaths::DataDirectory().c_str());
    Append(out, L"Fullscreen app: %ls\r\n",
           snapshot.fullscreenApp ? L"yes (polling paused)" : L"no");

    if (snapshot.lastPollUnixMs == 0) {
        Append(out, L"Last successful poll: never\r\n");
    } else {
        Append(out, L"Last successful poll: %ls (%ls)\r\n",
               LocalTimeText(snapshot.lastPollUnixMs).c_str(),
               PollAgeText(snapshot.secondsSincePoll).c_str());
    }

    Append(out, L"\r\n=== Current telemetry ===\r\n%ls", TelemetryText(snapshot.telemetry).c_str());

    Append(out, L"\r\nAirPods setting: %ls\r\n", snapshot.airPodsEnabled ? L"enabled" : L"disabled");
    Append(out, L"AirPods detected: %ls, connected: %ls\r\n",
           snapshot.airPods.hasData ? L"yes" : L"no", snapshot.airPods.connected ? L"yes" : L"no");
    if (snapshot.airPods.hasData) {
        Append(out, L"AirPods name: %ls\r\n",
               snapshot.airPods.deviceName.empty() ? L"(unknown)" : snapshot.airPods.deviceName.c_str());
        Append(out, L"AirPods left: %d%%, right: %d%%, case: %d%%\r\n",
               snapshot.airPods.leftLevel, snapshot.airPods.rightLevel, snapshot.airPods.caseLevel);
    } else {
        Append(out, L"AirPods name: (none)\r\n");
        Append(out, L"AirPods left: unknown, right: unknown, case: unknown\r\n");
    }

    AirPodsMonitor::Instance().AppendDiagnostics(out);

    Append(out, L"\r\n=== Poller ===\r\n%ls", PollTraceText(snapshot.pollTrace).c_str());

    HidDump::AppendTo(out);

    Append(out, L"\r\n=== widget.log (tail) ===\r\n");
    out += ReadLogTail();
    Append(out, L"\r\n=== end of report ===\r\n");

    wchar_t fileName[64]{};
    swprintf_s(fileName, L"report-%04u%02u%02u-%02u%02u%02u.txt",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const std::wstring path = AppPaths::DataFile(fileName);
    if (path.empty() || !AppPaths::WriteUtf8File(path, out)) return {};
    return path;
}
