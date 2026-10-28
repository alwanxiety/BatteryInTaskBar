#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>
#include "core/Localization.h"
#include "tools/HidDump.h"
#include "ui/WidgetIdentity.h"

namespace {
constexpr wchar_t kInstanceMutexName[] = L"Local\\BatteryInTaskBar.Widget.Instance";
constexpr wchar_t kUserExitEventName[] = L"Local\\BatteryInTaskBar.Exit";
constexpr wchar_t kWatchdogMutexName[] = L"Local\\BatteryInTaskBar.Injector";

constexpr int kOneShotAttempts = 5;
constexpr DWORD kOneShotDelayMs = 6000;
constexpr DWORD kWatchdogPollMs = 3000;
constexpr DWORD kPostInjectCooldownMs = 5000;

DWORD GetExplorerPID() {
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, L"explorer.exe") != 0) continue;
            DWORD session = 0;
            // В мультисессионной системе нужен Explorer именно нашей сессии.
            if (!ProcessIdToSessionId(pe.th32ProcessID, &session) || session != mySession) continue;
            pid = pe.th32ProcessID;
            break;
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return pid;
}

std::wstring GetModuleDirectory() {
    std::vector<WCHAR> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }

    std::wstring path(buffer.data(), buffer.size());
    const auto lastSlash = path.find_last_of(L'\\');
    if (lastSlash == std::wstring::npos) return {};
    path.resize(lastSlash + 1);
    return path;
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsWidgetRunning() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutexName);
    if (!mutex) return false;
    CloseHandle(mutex);
    return true;
}

bool InjectDll(HANDLE hProc, const std::wstring& dllPath) {
    const SIZE_T memSize = (dllPath.size() + 1) * sizeof(WCHAR);
    LPVOID pRemoteBuf = VirtualAllocEx(hProc, nullptr, memSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteBuf) return false;

    if (!WriteProcessMemory(hProc, pRemoteBuf, dllPath.c_str(), memSize, nullptr)) {
        VirtualFreeEx(hProc, pRemoteBuf, 0, MEM_RELEASE);
        return false;
    }

    LPTHREAD_START_ROUTINE pfnLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!pfnLoadLib) {
        VirtualFreeEx(hProc, pRemoteBuf, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, pfnLoadLib, pRemoteBuf, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, pRemoteBuf, 0, MEM_RELEASE);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    const bool finished = waitResult == WAIT_OBJECT_0 && GetExitCodeThread(hThread, &exitCode) != FALSE;
    CloseHandle(hThread);
    VirtualFreeEx(hProc, pRemoteBuf, 0, MEM_RELEASE);

    return finished && exitCode != 0;
}

std::wstring ResolveDllPath() {
    const std::wstring directory = GetModuleDirectory();
    if (directory.empty()) return {};

    std::wstring path = directory + L"taskbar_mod.dll";
    if (FileExists(path)) return path;

    path = directory + L"libtaskbar_mod.dll";
    return FileExists(path) ? path : std::wstring{};
}

// --- Повторный запуск уже загруженной DLL ---
// Если виджет закрыли, а ссылка на DLL в Explorer осталась, обычный LoadLibrary
// не вызовет DllMain, и виджет молча не появится. Запускать код по вычисленному
// адресу экспорта нельзя: такой вызов может зависнуть на loader lock и
// заморозить оболочку. Поэтому честно сообщаем об ошибке и просим перезапустить
// Explorer.

std::uintptr_t FindRemoteModule(DWORD pid, const wchar_t* moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{ sizeof(MODULEENTRY32W) };
    std::uintptr_t base = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return base;
}

/// Время сборки файла (PE TimeDateStamp) — должно совпасть с загруженной DLL,
/// иначе мы разбудим старую версию вместо новой.
bool LocalImageStamp(const std::wstring& path, DWORD& timeStamp) {
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) return false;
    bool ok = false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) {
            timeStamp = nt->FileHeader.TimeDateStamp;
            ok = true;
        }
    }
    FreeLibrary(module);
    return ok;
}

/// Разбудить уже загруженную DLL: она ждёт на событии с меткой своей сборки и
/// запускает виджет заново. Если на диске другая версия, события не существует —
/// тогда нужен перезапуск Explorer.
bool SignalRestart(const std::wstring& dllPath) {
    DWORD stamp = 0;
    if (!LocalImageStamp(dllPath, stamp)) return false;

    wchar_t name[64]{};
    swprintf_s(name, L"Local\\BatteryInTaskBar.Restart.%08lX", static_cast<unsigned long>(stamp));
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!event) return false;
    SetEvent(event);
    CloseHandle(event);

    // Ждём, пока DLL создаст мьютекс экземпляра (виджет стартует).
    for (int attempt = 0; attempt < 50; ++attempt) {
        Sleep(100);
        if (IsWidgetRunning()) return true;
    }
    return false;
}

bool TryInjectOnce(const std::wstring& dllPath) {
    if (dllPath.empty()) return false;

    const DWORD pid = GetExplorerPID();
    if (pid == 0) return false;

    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE,
        pid
    );
    if (!hProc) return false;

    // Модуль уже в процессе (виджет закрыли, но ссылка осталась): запускаем
    // виджет через экспорт, иначе LoadLibrary не вызовет DllMain.
    const std::uintptr_t loadedBase = FindRemoteModule(pid, L"taskbar_mod.dll");
    if (loadedBase != 0) {
        // DLL уже в процессе (виджет закрыли, но ссылка осталась): будим её
        // через событие. LoadLibrary в этом случае DllMain не вызовет.
        const bool started = SignalRestart(dllPath);
        CloseHandle(hProc);
        return started;
    }

    const bool injected = InjectDll(hProc, dllPath);
    CloseHandle(hProc);
    return injected;
}

void CloseWidgetWindow() {
    // Класс окна уникален для каждой загрузки DLL, поэтому ищем по заголовку.
    HWND hWidget = FindWindowW(nullptr, kWidgetWindowTitle);
    if (!hWidget) hWidget = FindWindowW(kLegacyWidgetClassName, nullptr);  // версии без заголовка
    if (hWidget) PostMessageW(hWidget, WM_CLOSE, 0, 0);
}

/// Режим `--dump-hid`: собирает отчёт о HID-устройствах, сохраняет его и открывает
/// в блокноте. Инъекция не выполняется — отчёт можно снять без виджета.
int RunHidDump() {
    L10n::Init();
    const std::wstring path = HidDump::WriteReport();
    const std::wstring title = L10n::Get(Str::DumpTitle);

    if (path.empty()) {
        MessageBoxW(nullptr, L10n::Get(Str::DumpFailed), title.c_str(), MB_ICONERROR | MB_OK);
        return 1;
    }

    wchar_t message[1024]{};
    swprintf_s(message, 1024, L10n::Get(Str::DumpSaved), path.c_str());
    MessageBoxW(nullptr, message, title.c_str(), MB_ICONINFORMATION | MB_OK);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

/// Показать причину, по которой виджет не запустился. Без этого окно закрывается
/// молча, и пользователь думает, что injector.exe «не работает».
void ShowInjectFailure() {
    MessageBoxW(nullptr, L10n::Get(Str::InjectFailedText), L10n::Get(Str::InjectFailedTitle),
                MB_ICONERROR | MB_OK);
}

/// Режим `--watch`: следим, чтобы виджет жил в актуальном Explorer.
/// Нужен для восстановления после перезапуска/падения Explorer.
int RunWatchdog() {
    HANDLE hWatchdogMutex = CreateMutexW(nullptr, FALSE, kWatchdogMutexName);
    if (!hWatchdogMutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hWatchdogMutex);  // второй сторож не нужен
        return 0;
    }

    HANDLE hUserExit = CreateEventW(nullptr, TRUE, FALSE, kUserExitEventName);
    if (!hUserExit) {
        CloseHandle(hWatchdogMutex);
        return 1;
    }
    ResetEvent(hUserExit);

    DWORD lastInjectTick = 0;

    for (;;) {
        // Пользователь закрыл виджет из меню — сторож завершает работу.
        if (WaitForSingleObject(hUserExit, kWatchdogPollMs) != WAIT_TIMEOUT) break;
        if (IsWidgetRunning()) continue;

        const DWORD now = GetTickCount();
        if (lastInjectTick != 0 && now - lastInjectTick < kPostInjectCooldownMs) continue;

        // Ещё раз проверяем флаг выхода непосредственно перед инъекцией:
        // иначе можно «воскресить» виджет в момент, когда пользователь его закрывает.
        if (WaitForSingleObject(hUserExit, 0) == WAIT_OBJECT_0) break;
        if (!TryInjectOnce(ResolveDllPath())) continue;

        lastInjectTick = now;
        if (WaitForSingleObject(hUserExit, 0) == WAIT_OBJECT_0) {
            CloseWidgetWindow();
            break;
        }
    }

    CloseHandle(hUserExit);
    CloseHandle(hWatchdogMutex);
    return 0;
}
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    L10n::Init();
    // Диагностика важнее остальных режимов: она ничего не инжектит и нужна
    // как раз тем, у кого виджет не работает.
    if (wcsstr(GetCommandLineW(), L"--dump-hid")) return RunHidDump();
    if (wcsstr(GetCommandLineW(), L"--watch")) return RunWatchdog();

    // Однократный запуск: инжектируем в текущий Explorer и выходим.
    if (IsWidgetRunning()) return 0;

    const std::wstring dllPath = ResolveDllPath();
    if (dllPath.empty()) {
        ShowInjectFailure();
        return 1;
    }

    for (int attempt = 0; attempt < kOneShotAttempts; ++attempt) {
        if (TryInjectOnce(dllPath)) return 0;  // Успех
        Sleep(kOneShotDelayMs);
    }

    // Без сообщения пользователь видит только «ничего не произошло».
    ShowInjectFailure();
    return 1;  // Не удалось после всех попыток
}
