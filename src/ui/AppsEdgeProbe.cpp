// min/max из windows.h ломают std::max — отключаем до первого включения.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui/AppsEdgeProbe.h"
#include "core/AppState.h"
#include "core/Diagnostics.h"
#include "system/WindowScanner.h"

#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

// Обычно снимок обновляется по событию оболочки (открылось/закрылось окно),
// а этот период — страховка. UIA по XAML-дереву панели задач стоит сотни
// миллисекунд CPU, поэтому «каждые 5 секунд» для фонового процесса — плохо.
constexpr DWORD kRefreshPeriodMs = 60000;
// Пока виджет скрыт полноэкранным приложением, UIA не трогаем вовсе.
constexpr DWORD kHiddenRefreshPeriodMs = 120000;
// Частые события оболочки склеиваем в один запрос.
constexpr ULONGLONG kMinQueryIntervalMs = 750;

/// Правый край кнопок приложений (без учёта трея). Возвращает false, если
/// панель недоступна или UIA не дал ни одной кнопки.
bool QueryAppsEdge(IUIAutomation& automation, HWND taskbar, const RECT& tb, const RECT& tray,
                   int& right) {
    right = 0;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation.ElementFromHandle(taskbar, &root)) || !root) return false;

    VARIANT type{};
    type.vt = VT_I4;
    type.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation.CreatePropertyCondition(UIA_ControlTypePropertyId, type,
                                                  &condition)) ||
        !condition) {
        return false;
    }

    // Прямоугольники забираем пакетно: иначе каждый элемент — отдельный вызов
    // провайдера, и обход десятков кнопок заметно грузит CPU Explorer'а.
    ComPtr<IUIAutomationCacheRequest> cacheRequest;
    const bool useCache =
        SUCCEEDED(automation.CreateCacheRequest(cacheRequest.GetAddressOf())) && cacheRequest;
    if (useCache) {
        cacheRequest->AddProperty(UIA_BoundingRectanglePropertyId);
        cacheRequest->put_TreeFilter(condition.Get());
        cacheRequest->put_TreeScope(TreeScope_Descendants);
    }

    auto rightmost = [&](IUIAutomationElementArray* elements, bool cached) -> int {
        if (!elements) return 0;
        int length = 0;
        if (FAILED(elements->get_Length(&length))) return 0;
        int best = 0;
        for (int i = 0; i < length; ++i) {
            ComPtr<IUIAutomationElement> element;
            if (FAILED(elements->GetElement(i, &element)) || !element) continue;

            RECT rc{};
            const HRESULT rectResult = cached ? element->get_CachedBoundingRectangle(&rc)
                                              : element->get_CurrentBoundingRectangle(&rc);
            if (FAILED(rectResult)) continue;
            if (rc.right <= rc.left || rc.bottom <= rc.top) continue;
            if (rc.right > tray.left) continue;                        // трей и правее
            if (rc.bottom <= tb.top || rc.top >= tb.bottom) continue;  // не в панели
            best = std::max(best, static_cast<int>(rc.right));
        }
        return best;
    };

    ComPtr<IUIAutomationElementArray> elements;
    int best = 0;
    if (useCache) {
        const HRESULT hr = root->FindAllBuildCache(TreeScope_Descendants, condition.Get(),
                                                   cacheRequest.Get(), elements.GetAddressOf());
        if (SUCCEEDED(hr)) best = rightmost(elements.Get(), true);
    }
    if (best <= 0) {
        // Провайдер XAML не всегда отдаёт прямоугольники из кэша — читаем напрямую.
        elements.Reset();
        const HRESULT hr = root->FindAll(TreeScope_Descendants, condition.Get(),
                                         elements.GetAddressOf());
        if (FAILED(hr)) return false;
        best = rightmost(elements.Get(), false);
    }

    if (best <= 0) return false;
    right = best;
    return true;
}

}  // namespace

AppsEdgeProbe::~AppsEdgeProbe() {
    Stop();
}

void AppsEdgeProbe::Start(HWND taskbar, HWND notifyWindow, UINT notifyMessage) {
    if (m_thread) return;
    m_taskbar = taskbar;
    m_notify = notifyWindow;
    m_notifyMessage = notifyMessage;

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_stopEvent || !m_wakeEvent) {
        Stop();
        return;
    }
    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) Stop();
}

void AppsEdgeProbe::Stop() {
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_thread) {
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    if (m_wakeEvent) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
    std::lock_guard lock(m_mutex);
    m_snapshot = {};
}

void AppsEdgeProbe::RequestRefresh() {
    if (m_wakeEvent) SetEvent(m_wakeEvent);
}

bool AppsEdgeProbe::Get(const RECT& taskbar, const RECT& tray, ULONGLONG maxAgeMs, int& right) {
    bool needRefresh = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_snapshot.valid && EqualRect(&m_snapshot.taskbar, &taskbar) &&
            EqualRect(&m_snapshot.tray, &tray)) {
            const ULONGLONG now = GetTickCount64();
            right = m_snapshot.right;
            needRefresh = now < m_snapshot.tick || (now - m_snapshot.tick) > maxAgeMs;
            if (!needRefresh) return true;
        } else {
            needRefresh = true;
        }
    }
    // Данных для этой геометрии нет: просим фон, но UI не ждём.
    if (needRefresh) RequestRefresh();
    return false;
}

DWORD WINAPI AppsEdgeProbe::ThreadProc(LPVOID param) {
    static_cast<AppsEdgeProbe*>(param)->Run();
    return 0;
}

void AppsEdgeProbe::Run() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comReady = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE || hr == S_FALSE;

    ComPtr<IUIAutomation> automation;
    auto ensureAutomation = [&automation]() -> bool {
        if (automation) return true;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&automation)))) {
            return false;
        }
        // Ограничиваем ожидание: зависший провайдер не должен занимать поток
        // навсегда.
        ComPtr<IUIAutomation2> automation2;
        if (SUCCEEDED(automation.As(&automation2))) {
            automation2->put_ConnectionTimeout(400);
            automation2->put_TransactionTimeout(800);
        }
        return true;
    };

    for (;;) {
        if (comReady && !automation) ensureAutomation();

        const bool hidden = AppState::Instance().IsFullscreenAppActive();
        HANDLE events[2] = { m_stopEvent, m_wakeEvent };
        const DWORD period = hidden ? kHiddenRefreshPeriodMs : kRefreshPeriodMs;
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, period);
        if (wait == WAIT_OBJECT_0) break;

        if (hidden || !m_taskbar || !IsWindow(m_taskbar)) continue;

        // Склеиваем частые события оболочки: активация окон приходит пачками,
        // а обход UIA стоит дорого.
        if (m_lastQueryTick != 0) {
            const ULONGLONG now = GetTickCount64();
            if (now >= m_lastQueryTick && now - m_lastQueryTick < kMinQueryIntervalMs) {
                const DWORD delay =
                    static_cast<DWORD>(kMinQueryIntervalMs - (now - m_lastQueryTick));
                HANDLE waitEvents[2] = { m_stopEvent, m_wakeEvent };
                if (WaitForMultipleObjects(2, waitEvents, FALSE, delay) == WAIT_OBJECT_0) break;
            }
        }

        RECT taskbar{};
        if (!GetWindowRect(m_taskbar, &taskbar)) continue;
        HWND trayWindow = WindowScanner::FindTrayNotifyWindow();
        RECT tray{};
        if (!trayWindow || !GetWindowRect(trayWindow, &tray)) continue;

        int right = 0;
        if (!automation.Get() ||
            !QueryAppsEdge(*automation.Get(), m_taskbar, taskbar, tray, right)) {
            if (!m_failureLogged) {
                m_failureLogged = true;
                Diag::Log(L"layout: apps edge query failed, using the tray cell only");
            }
            continue;
        }
        m_lastQueryTick = GetTickCount64();
        if (m_failureLogged) {
            m_failureLogged = false;
            Diag::Log(L"layout: apps edge query recovered");
        }

        bool changed = false;
        {
            std::lock_guard lock(m_mutex);
            changed = !m_snapshot.valid || m_snapshot.right != right ||
                      !EqualRect(&m_snapshot.taskbar, &taskbar) ||
                      !EqualRect(&m_snapshot.tray, &tray);
            m_snapshot = { right, taskbar, tray, GetTickCount64(), true };
        }
        if (changed) {
            Diag::Log(L"layout: apps edge=%d", right);
            if (m_notify) PostMessageW(m_notify, m_notifyMessage, 0, 0);
        }
    }

    automation.Reset();
    if (comReady) CoUninitialize();
}
