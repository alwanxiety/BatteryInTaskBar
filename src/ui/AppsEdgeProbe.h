#pragma once
#include <windows.h>
#include <mutex>

/// Фоновый опрос UI Automation: правый край кнопок приложений на панели задач.
/// UIA может отвечать сотни миллисекунд и делает это в процессе Explorer, то
/// есть на том же UI-потоке, где живёт виджет. Поэтому запрос уходит в
/// отдельный поток, а раскладка читает последний снимок.
class AppsEdgeProbe {
public:
    AppsEdgeProbe() = default;
    ~AppsEdgeProbe();

    AppsEdgeProbe(const AppsEdgeProbe&) = delete;
    AppsEdgeProbe& operator=(const AppsEdgeProbe&) = delete;

    /// @param notifyWindow окно, которому придёт notifyMessage после того, как
    ///        снимок изменился (повод пересчитать ячейки).
    void Start(HWND taskbar, HWND notifyWindow, UINT notifyMessage);
    void Stop();

    /// Снимок для этой же геометрии панели. Если данных ещё нет, просит поток
    /// обновиться и возвращает false — UI-поток ждать не должен.
    bool Get(const RECT& taskbar, const RECT& tray, ULONGLONG maxAgeMs, int& right);
    void RequestRefresh();

private:
    struct Snapshot {
        int right = 0;
        RECT taskbar{};
        RECT tray{};
        ULONGLONG tick = 0;
        bool valid = false;
    };

    static DWORD WINAPI ThreadProc(LPVOID param);
    void Run();

    HWND m_taskbar{ nullptr };
    HWND m_notify{ nullptr };
    UINT m_notifyMessage{ 0 };

    HANDLE m_thread{ nullptr };
    HANDLE m_stopEvent{ nullptr };
    HANDLE m_wakeEvent{ nullptr };
    ULONGLONG m_lastQueryTick{ 0 };
    bool m_failureLogged{ false };

    mutable std::mutex m_mutex;
    Snapshot m_snapshot;
};
