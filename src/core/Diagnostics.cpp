#include "core/Diagnostics.h"
#include "core/AppPaths.h"
#include <windows.h>
#include <cstdarg>
#include <cwchar>

namespace {

constexpr wchar_t kLogFileName[] = L"widget.log";
constexpr ULONGLONG kMaxLogBytes = 512ull * 1024ull;

CRITICAL_SECTION g_lock;
bool g_lockReady = false;
std::wstring g_path;

std::wstring Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[48]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds);
    return buffer;
}

void WriteText(HANDLE file, const wchar_t* text, DWORD lengthBytes) {
    DWORD written = 0;
    WriteFile(file, text, lengthBytes, &written, nullptr);
}

void RotateIfNeeded() {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &info)) return;
    const ULONGLONG size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size <= kMaxLogBytes) return;
    const std::wstring oldPath = g_path + L".old";
    MoveFileExW(g_path.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace

void Diag::Init() {
    if (g_lockReady) return;
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;

    g_path = AppPaths::DataFile(kLogFileName);
    if (g_path.empty()) return;
    RotateIfNeeded();
    Log(L"--- BatteryInTaskBar start ---");
}

void Diag::Log(const wchar_t* format, ...) {
    if (!g_lockReady || g_path.empty() || format == nullptr) return;

    wchar_t message[2048]{};
    va_list args;
    va_start(args, format);
    vswprintf_s(message, format, args);
    va_end(args);

    std::wstring line = Timestamp();
    line += L"  ";
    line += message;
    line += L"\r\n";

    EnterCriticalSection(&g_lock);
    HANDLE file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
            // BOM, иначе Блокнот покажет кириллицу как мусор.
            const wchar_t bom = 0xFEFF;
            WriteText(file, &bom, sizeof(bom));
        }
        WriteText(file, line.c_str(), static_cast<DWORD>(line.size() * sizeof(wchar_t)));
        CloseHandle(file);
    }
    LeaveCriticalSection(&g_lock);
}

std::wstring Diag::LogPath() {
    return g_path;
}
