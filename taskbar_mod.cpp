#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "core/Application.h"
#include "core/Diagnostics.h"
#include "core/Localization.h"
#include "res/app_version.h"
#include "ui/WidgetIdentity.h"

// ============================================================================
// ТОЧКА ВХОДА DLL
// ============================================================================

namespace {
// Одна копия виджета на сессию.
constexpr wchar_t kInstanceMutexName[] = L"Local\\BatteryInTaskBar.Widget.Instance";
// Событие «запустить виджет снова». Имя содержит метку сборки: инжектор с
// другой версией на диске не сможет разбудить старый код.
constexpr wchar_t kRestartEventPrefix[] = L"Local\\BatteryInTaskBar.Restart.";

/// Время сборки модуля (PE TimeDateStamp).
DWORD ModuleTimeStamp(HMODULE hModule) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hModule);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const BYTE*>(hModule) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

std::wstring RestartEventName(HMODULE hModule) {
    wchar_t suffix[16]{};
    swprintf_s(suffix, L"%08lX", static_cast<unsigned long>(ModuleTimeStamp(hModule)));
    return std::wstring(kRestartEventPrefix) + suffix;
}

/// Закрыть виджет, оставшийся от другой загрузки DLL. Мьютекс ловит только
/// текущую версию, а старая могла остаться без него — тогда будет две плитки.
void CloseStaleWidget() {
    HWND stale = FindWindowW(nullptr, kWidgetWindowTitle);
    if (!stale) stale = FindWindowW(kLegacyWidgetClassName, nullptr);
    if (!stale) return;

    PostMessageW(stale, WM_CLOSE, 0, 0);
    // Ждём фактического закрытия, а не фиксированные 250 мс: иначе на
    // медленном Explorer'е остаются две плитки.
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (!IsWindow(stale)) break;
        Sleep(50);
    }
    if (IsWindow(stale)) Diag::Log(L"migration: stale widget did not close");
}

DWORD WINAPI ModMainThread(LPVOID lpParam) {
    auto hModule = static_cast<HMODULE>(lpParam);
    if (!hModule) return 0;

    // DLL остаётся загруженной до перезапуска Explorer: за время работы на неё
    // могла остаться ссылка (WinRT-объекты, слоистые окна меню), а
    // FreeLibraryAndExitThread в таком состоянии оставляет «полуживой» модуль,
    // который больше нельзя запустить. Вместо этого ждём запрос от injector.exe.
    const std::wstring restartName = RestartEventName(hModule);
    HANDLE hRestart = CreateEventW(nullptr, FALSE, FALSE, restartName.c_str());
    if (!hRestart) return 0;

    for (;;) {
        HANDLE hInstanceMutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
        const bool alreadyRunning = hInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS;
        if (alreadyRunning) {
            // Виджет уже работает (например, повторная инъекция): ждём сигнала.
            CloseHandle(hInstanceMutex);
            WaitForSingleObject(hRestart, INFINITE);
            continue;
        }

        CloseStaleWidget();

        L10n::Init();
        Diag::Init();
        Diag::Log(L"widget loading, version=%ls, language=%ls", APP_VERSION_WSTRING,
                  L10n::LanguageCode());
        {
            Application app(hModule);
            const bool clean = app.Run();
            Diag::Log(L"widget stopped");
            if (!clean) Diag::Log(L"widget: a worker did not stop in time");
        }

        if (hInstanceMutex) CloseHandle(hInstanceMutex);
        WaitForSingleObject(hRestart, INFINITE);
    }
}
}

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        if (HANDLE hThread = CreateThread(nullptr, 0, ModMainThread, hModule, 0, nullptr)) {
            CloseHandle(hThread);
        } else {
            return FALSE;  // не смогли стартовать — пусть LoadLibrary сообщит об ошибке
        }
    }
    return TRUE;
}
