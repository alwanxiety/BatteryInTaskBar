#pragma once
#include <windows.h>
#include "ui/WidgetWindow.h"

class Application {
public:
    explicit Application(HMODULE hModule);
    /// true — всё остановилось штатно и DLL можно выгружать.
    bool Run();

private:
    /// false — какой-то поток не удалось остановить; выгружать DLL нельзя.
    bool Stop();
    static DWORD WINAPI StaticPollingThread(LPVOID param);
    DWORD PollingWorker();

    WidgetWindow m_window;
    HANDLE m_stopEvent{ nullptr };
    HANDLE m_pollingThread{ nullptr };
};