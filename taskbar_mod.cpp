#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "core/Application.h"
#include "core/Diagnostics.h"
#include "core/Localization.h"
#include "res/app_version.h"
#include "ui/WidgetIdentity.h"

// ============================================================================
// ТОЧКА ВХОДА DLL
// ============================================================================

namespace {
// Одна копия виджета на сессию: вторая инъекция просто выгрузит DLL обратно.
constexpr wchar_t kInstanceMutexName[] = L"Local\\BatteryInTaskBar.Widget.Instance";

DWORD WINAPI ModMainThread(LPVOID lpParam) {
    auto hModule = static_cast<HMODULE>(lpParam);
    if (!hModule) return 0;

    HANDLE hInstanceMutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
    if (hInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hInstanceMutex);
        FreeLibraryAndExitThread(hModule, 0);
    }

    // Миграция: закрываем виджет, оставшийся от другой загрузки DLL. Мьютекс ловит
    // только текущую версию, а старая могла остаться без него — тогда будет две плитки.
    HWND hStaleWidget = FindWindowW(nullptr, kWidgetWindowTitle);
    if (!hStaleWidget) hStaleWidget = FindWindowW(kLegacyWidgetClassName, nullptr);
    if (hStaleWidget) {
        PostMessageW(hStaleWidget, WM_CLOSE, 0, 0);
        Sleep(250);
    }

    {
        // Язык интерфейса определяем один раз на загрузку DLL: он же попадает в журнал,
        // чтобы по присланному логу было видно, какая ветка строк работает.
        L10n::Init();
        Diag::Init();
        Diag::Log(L"widget loading, version=%ls, language=%ls", APP_VERSION_WSTRING,
                  L10n::LanguageCode());

        Application app(hModule);
        app.Run();
        Diag::Log(L"widget stopped");
    }

    if (hInstanceMutex) CloseHandle(hInstanceMutex);
    FreeLibraryAndExitThread(hModule, 0);
    return 0;
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
