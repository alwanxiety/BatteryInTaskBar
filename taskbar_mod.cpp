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

    HANDLE hInstanceMutex = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
    if (hInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hInstanceMutex);
        FreeLibraryAndExitThread(hModule, 0);
    }

    CloseStaleWidget();

    L10n::Init();
    Diag::Init();
    Diag::Log(L"widget loading, version=%ls, language=%ls", APP_VERSION_WSTRING,
              L10n::LanguageCode());

    bool clean = false;
    {
        Application app(hModule);
        clean = app.Run();
        Diag::Log(L"widget stopped");
    }

    if (hInstanceMutex) CloseHandle(hInstanceMutex);
    if (!clean) {
        // Поток WinRT не удалось остановить: код DLL всё ещё нужен ему,
        // поэтому выгружать библиотеку нельзя.
        Diag::Log(L"widget: a worker did not stop in time, DLL stays loaded");
        return 0;
    }
    Diag::Log(L"widget: unloading DLL");
    FreeLibraryAndExitThread(hModule, 0);
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
