#pragma once
#include <windows.h>
#include "ui/WidgetWindow.h"

class Application {
public:
    explicit Application(HMODULE hModule);
    void Run();

private:
    void Stop();
    static DWORD WINAPI StaticPollingThread(LPVOID param);
    DWORD PollingWorker();

    WidgetWindow m_window;
    HANDLE m_stopEvent{ nullptr };
    HANDLE m_pollingThread{ nullptr };
};