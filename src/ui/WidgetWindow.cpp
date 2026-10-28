#define NOMINMAX
#include "ui/WidgetWindow.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <dbt.h>
#include <hidsdi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <uiautomation.h>
#include <wrl/client.h>
#include "system/WindowScanner.h"
#include "core/Diagnostics.h"
#include "core/Localization.h"
#include "core/Report.h"
#include "core/TelemetryCache.h"
#include "ui/ContextMenu.h"
#include "ui/MenuIds.h"
#include "ui/WidgetIdentity.h"

using Microsoft::WRL::ComPtr;

constexpr UINT_PTR TIMER_ANIM  = 101;
constexpr UINT_PTR TIMER_CHECK = 102;
constexpr UINT_PTR TIMER_HOVER = 103;
constexpr UINT_PTR TIMER_SNAP  = 104;
constexpr UINT WM_APP_TELEMETRY = WM_APP + 0x51;

constexpr wchar_t kAutoRunValueName[] = L"BatteryInTaskBar";
constexpr wchar_t kUserExitEventName[] = L"Local\\BatteryInTaskBar.Exit";
constexpr wchar_t kSettingsKey[] = L"Software\\BatteryInTaskBar";
constexpr wchar_t kThemeValueName[] = L"Theme";
constexpr wchar_t kPosValueName[] = L"PosOffset";

// Геометрия виджета и ячеек в DIP.
constexpr float kWidgetWidthDip = 168.0f;
constexpr float kWidgetHeightDip = 38.0f;
constexpr float kWidgetMarginDip = 4.0f;
constexpr float kSlotGapDip = 8.0f;
constexpr int kDragThresholdPx = 3;
constexpr int kSnapDurationMs = 140;

enum ZBID : DWORD {
    ZBID_IMMERSIVE_NOTIFICATION = 4
};

using pfnSetWindowBand = BOOL(WINAPI*)(HWND hWnd, HWND hwndInsertAfter, DWORD dwBand);

WidgetWindow::WidgetWindow(HMODULE hModule) : m_hInst(hModule) {}

bool WidgetWindow::Create() {
    m_isDarkTheme = QuerySystemIsDark();
    m_theme = LoadTheme();
    m_posOffsetDip = LoadPosOffset();
    // Последние показания — до первого Relayout, чтобы не мелькало «поиск».
    TelemetryCache::Restore();
    // Вторая плитка не имеет отдельной настройки: она живёт, пока наушники
    // подключены. Скрыть её можно только из её собственного меню.
    AppState::Instance().SetAirPodsEnabled(true);
    AppState::Instance().RequestAirPodsScan();
    if (!m_renderer.Initialize()) return false;
    if (!m_slotOverlay.Create(m_hInst)) return false;
    // Плитка наушников не критична: если её окно не создалось, мышь всё равно работает.
    m_headset.Create(m_hInst);

    m_hTaskbarWnd = WindowScanner::FindTaskbarWindow();
    if (!m_hTaskbarWnd) return false;

    if (!RegisterWindowClass()) return false;

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        m_className.c_str(), kWidgetWindowTitle,
        WS_POPUP | WS_VISIBLE,
        0, 0, 168, 38,
        nullptr, nullptr, m_hInst, this
    );

    if (!m_hwnd) {
        UnregisterWindowClass();
        return false;
    }
    s_instance = this;

    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    if (HMODULE hUser32 = GetModuleHandleW(L"user32.dll")) {
        if (auto pSetWindowBand = reinterpret_cast<pfnSetWindowBand>(GetProcAddress(hUser32, "SetWindowBand"))) {
            pSetWindowBand(m_hwnd, nullptr, ZBID_IMMERSIVE_NOTIFICATION);
        }
    }

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    DEV_BROADCAST_DEVICEINTERFACE_W devFilter{};
    devFilter.dbcc_size = sizeof(devFilter);
    devFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    devFilter.dbcc_classguid = hidGuid;
    m_hDevNotify = RegisterDeviceNotificationW(m_hwnd, &devFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

    RegisterShellHookWindow(m_hwnd);
    m_wmShellHook = RegisterWindowMessageW(L"SHELLHOOK");

    // Два узких хука вместо одного диапазона «все события»: первый ловит фуллскрин,
    // второй — только перемещение окон Explorer (таскбар и его дочерние элементы).
    DWORD explorerPid = 0;
    GetWindowThreadProcessId(m_hTaskbarWnd, &explorerPid);
    m_hEventHookForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                             nullptr, StaticWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (explorerPid != 0) {
        m_hEventHookLocation = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                               nullptr, StaticWinEventProc, explorerPid, 0, WINEVENT_OUTOFCONTEXT);
    }

    SetTimer(m_hwnd, TIMER_CHECK, 500, nullptr);
    Relayout();
    return true;
}

void WidgetWindow::Destroy() {
    CancelDrag();
    m_headset.Destroy();
    m_slotOverlay.Destroy();
    if (m_hEventHookForeground) { UnhookWinEvent(m_hEventHookForeground); m_hEventHookForeground = nullptr; }
    if (m_hEventHookLocation) { UnhookWinEvent(m_hEventHookLocation); m_hEventHookLocation = nullptr; }
    if (m_hDevNotify) { UnregisterDeviceNotification(m_hDevNotify); m_hDevNotify = nullptr; }
    if (m_hwnd) {
        KillTimer(m_hwnd, TIMER_ANIM);
        KillTimer(m_hwnd, TIMER_CHECK);
        KillTimer(m_hwnd, TIMER_HOVER);
        DeregisterShellHookWindow(m_hwnd);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    s_instance = nullptr;
    AppState::Instance().SetFullscreenAppActive(false);
    m_renderer.DiscardDeviceResources();
    UnregisterWindowClass();
}

bool WidgetWindow::RegisterWindowClass() {
    // Имя класса уникально для каждой загрузки DLL. Иначе, если предыдущая копия
    // выгрузилась, не сняв регистрацию, в таблице классов Explorer остаётся WndProc,
    // указывающий в освобождённую память, и первое же сообщение окну роняет Explorer
    // (c0000005, модуль unknown). Уникальное имя гарантирует свежий WndProc.
    const std::wstring base = std::wstring(kWidgetClassNamePrefix) + L"." +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(m_hInst)) + L"." +
        std::to_wstring(GetTickCount64());

    for (int attempt = 0; attempt < 8; ++attempt) {
        m_className = attempt == 0 ? base : base + L"." + std::to_wstring(attempt);

        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, StaticWndProc, 0, 0,
                           m_hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr,
                           m_className.c_str(), nullptr };
        if (RegisterClassExW(&wc)) return true;
        // Совпадение имени (редкий случай) — просто пробуем следующее.
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
    }

    m_className.clear();
    return false;
}

void WidgetWindow::UnregisterWindowClass() {
    if (m_className.empty()) return;
    // Снимаем регистрацию своей же загрузки: иначе класс с «мёртвым» WndProc
    // переживёт выгрузку DLL.
    UnregisterClassW(m_className.c_str(), m_hInst);
    m_className.clear();
}

void WidgetWindow::Relayout() {
    if (!m_hwnd || !m_hTaskbarWnd || !IsWindow(m_hTaskbarWnd)) return;

    const bool fullscreen = IsFullscreenAppActive();
    // Тот же признак, что скрывает виджет, останавливает и опрос HID (см. Application).
    AppState::Instance().SetFullscreenAppActive(fullscreen);
    if (fullscreen || !IsTaskbarShowing()) {
        CancelDrag();
        m_headset.CancelDrag();
        if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
        m_headset.Relayout({}, 0, 0, false);
        RefreshAnimationState();
        return;
    }

    // Пока идёт перетаскивание или магнитная доводка, позицией управляют они.
    if (m_dragging || m_snapping) return;

    // Пока тянут плитку наушников, её живой предпросмотр нельзя сбрасывать.
    if (m_headset.IsDragging()) return;

    HWND hTrayNotify = WindowScanner::FindTrayNotifyWindow();
    if (!hTrayNotify) return;

    RECT rcNotify;
    GetWindowRect(hTrayNotify, &rcNotify);

    UINT dpi = GetDpiForWindow(m_hwnd);
    if (dpi == 0) dpi = 96;
    float scale = static_cast<float>(dpi) / 96.0f;

    int widgetWidth = static_cast<int>(168.0f * scale);
    int widgetHeight = static_cast<int>(38.0f * scale);
    int margin = static_cast<int>(4.0f * scale);

    RECT rcTb;
    GetWindowRect(m_hTaskbarWnd, &rcTb);
    const bool vertical = (rcTb.bottom - rcTb.top) > (rcTb.right - rcTb.left);

    int xPos = 0;
    int yPos = 0;

    std::vector<RECT> slots;
    int stepDip = 0;
    bool headsetShouldShow = false;

    if (!vertical && ComputeSlots(slots, stepDip)) {
        m_currentMouseSlot = StoredSlotIndex(static_cast<int>(slots.size()), stepDip);
        m_currentStepDip = stepDip > 0 ? stepDip : 176;
        const RECT& cell = slots[m_currentMouseSlot];
        xPos = cell.left;
        yPos = cell.top;
        widgetWidth = cell.right - cell.left;
        widgetHeight = cell.bottom - cell.top;
        headsetShouldShow = ShouldShowHeadsetTile();
    } else {
        m_currentMouseSlot = 0;
        m_currentStepDip = stepDip > 0 ? stepDip : 176;
        if (vertical) {
            // Вертикальный таскбар: ставим виджет над треем, по центру полосы.
            xPos = rcTb.left + ((rcTb.right - rcTb.left) - widgetWidth) / 2;
            yPos = rcNotify.top - widgetHeight - margin;
        } else {
            xPos = rcNotify.left - widgetWidth - margin;
            yPos = rcTb.top + ((rcTb.bottom - rcTb.top) - widgetHeight) / 2;
        }
    }

    m_lastSlots = slots;
    // При предпросмотре обмена мышь уже стоит в чужой ячейке — не дёргаем её.
    if (!m_mousePreviewActive) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, xPos, yPos, widgetWidth, widgetHeight,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    }

    // Вторая плитка — отдельное окно: двигается независимо и не перекрывает
    // панель задач между плитками.
    m_headset.Relayout(slots, m_currentStepDip, m_currentMouseSlot, headsetShouldShow);
    RefreshAnimationState();
}

void WidgetWindow::ForceRestore() const {
    if (m_hwnd && IsWindow(m_hwnd) && !IsFullscreenAppActive() && IsTaskbarShowing()) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    }
}

void WidgetWindow::UpdateAnimationTimer(bool needAnim) {
    if (needAnim != m_animActive) {
        m_animActive = needAnim;
        if (needAnim) SetTimer(m_hwnd, TIMER_ANIM, 33, nullptr);
        else KillTimer(m_hwnd, TIMER_ANIM);
    }
}

void WidgetWindow::SetHoverTarget(float target) {
    if (!m_hwnd) return;
    m_hoverTarget = std::clamp(target, 0.0f, 1.0f);
    if (!m_hoverTimerActive) {
        m_hoverLastTick = GetTickCount64();
        SetTimer(m_hwnd, TIMER_HOVER, 16, nullptr);
        m_hoverTimerActive = true;
    }
}

void WidgetWindow::TickHover() {
    if (!m_hwnd) return;
    const ULONGLONG now = GetTickCount64();
    float dt = static_cast<float>(now - m_hoverLastTick) / 1000.0f;
    m_hoverLastTick = now;
    dt = std::clamp(dt, 0.0f, 0.1f);

    // Экспоненциальное сглаживание: быстрый отклик и мягкое «доезжание»,
    // без рывка, как в анимациях Windows 11.
    const float smoothing = 1.0f - std::exp(-dt / 0.07f);
    m_hoverProgress += (m_hoverTarget - m_hoverProgress) * smoothing;
    if (std::abs(m_hoverTarget - m_hoverProgress) < 0.004f) {
        m_hoverProgress = m_hoverTarget;
        KillTimer(m_hwnd, TIMER_HOVER);
        m_hoverTimerActive = false;
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

namespace {

std::uint64_t NowUnixMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

struct AppsEdgeCache {
    ULONGLONG tick{ 0 };
    int right{ 0 };
    bool valid{ false };
};

AppsEdgeCache g_appsEdgeCache;

// Правый край кнопок приложений по данным UI Automation. Legacy-окна Win11
// (ReBarWindow32 / MSTaskSwWClass) не отражают реальную XAML-раскладку: при
// большом числе окон кнопки уходят дальше, и ячейки наезжали на них.
int QueryAppsEdge(HWND taskbar, const RECT& tb, const RECT& tray, bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force && g_appsEdgeCache.valid && now - g_appsEdgeCache.tick < 3000) {
        return g_appsEdgeCache.right;
    }

    static const bool comReady = [] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE || hr == S_FALSE;
    }();
    if (!comReady) return g_appsEdgeCache.valid ? g_appsEdgeCache.right : 0;

    using Microsoft::WRL::ComPtr;
    static ComPtr<IUIAutomation> automation;
    if (!automation) {
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&automation)))) {
            return g_appsEdgeCache.valid ? g_appsEdgeCache.right : 0;
        }
        // Ограничиваем ожидание: зависший провайдер не должен подвесить виджет.
        ComPtr<IUIAutomation2> automation2;
        if (SUCCEEDED(automation.As(&automation2))) {
            automation2->put_ConnectionTimeout(400);
            automation2->put_TransactionTimeout(800);
        }
    }

    int best = 0;
    ComPtr<IUIAutomationElement> root;
    if (SUCCEEDED(automation->ElementFromHandle(taskbar, &root)) && root) {
        VARIANT type{};
        type.vt = VT_I4;
        type.lVal = UIA_ButtonControlTypeId;
        ComPtr<IUIAutomationCondition> condition;
        if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type,
                                                          &condition)) &&
            condition) {
            ComPtr<IUIAutomationElementArray> elements;
            if (SUCCEEDED(root->FindAll(TreeScope_Descendants, condition.Get(), &elements)) &&
                elements) {
                int length = 0;
                elements->get_Length(&length);
                for (int i = 0; i < length; ++i) {
                    ComPtr<IUIAutomationElement> element;
                    if (FAILED(elements->GetElement(i, &element)) || !element) continue;

                    RECT rc{};
                    if (FAILED(element->get_CurrentBoundingRectangle(&rc))) continue;
                    if (rc.right <= rc.left || rc.bottom <= rc.top) continue;
                    if (rc.right > tray.left) continue;                        // трей и правее
                    if (rc.bottom <= tb.top || rc.top >= tb.bottom) continue;  // не в панели
                    best = std::max(best, static_cast<int>(rc.right));
                }
            }
        }
    }

    if (best > 0) g_appsEdgeCache = { now, best, true };
    return best > 0 ? best : (g_appsEdgeCache.valid ? g_appsEdgeCache.right : 0);
}

}  // namespace

bool WidgetWindow::ComputeSlots(std::vector<RECT>& slots, int& stepDip, bool forceAppsRefresh) const {
    slots.clear();
    stepDip = 0;
    if (!m_hwnd || !m_hTaskbarWnd || !IsWindow(m_hTaskbarWnd)) return false;

    RECT rcTb{};
    if (!GetWindowRect(m_hTaskbarWnd, &rcTb)) return false;
    // Вертикальная панель: свободного места по горизонтали нет, оставляем как было.
    if ((rcTb.bottom - rcTb.top) >= (rcTb.right - rcTb.left)) return false;

    HWND hTray = WindowScanner::FindTrayNotifyWindow();
    RECT rcTray{};
    if (!hTray || !GetWindowRect(hTray, &rcTray)) return false;

    UINT dpi = GetDpiForWindow(m_hwnd);
    if (dpi == 0) dpi = 96;
    const float scale = static_cast<float>(dpi) / 96.0f;

    const int widgetW = static_cast<int>(std::lround(kWidgetWidthDip * scale));
    const int widgetH = static_cast<int>(std::lround(kWidgetHeightDip * scale));
    const int margin = static_cast<int>(std::lround(kWidgetMarginDip * scale));
    const int gap = static_cast<int>(std::lround(kSlotGapDip * scale));

    const int rightEdge = static_cast<int>(rcTray.left) - margin;
    if (rightEdge - widgetW <= rcTb.left) return false;

    // По умолчанию — одна ячейка у трея; расширяем влево до кнопок приложений.
    int leftBound = rightEdge - widgetW;
    const int appsRight = QueryAppsEdge(m_hTaskbarWnd, rcTb, rcTray, forceAppsRefresh);
    if (appsRight > 0 && appsRight < rcTray.left) {
        // Ячейка, которая хотя бы на пиксель заходит на кнопки, отбрасывается:
        // leftBound — самый левый допустимый край.
        leftBound = std::min(leftBound, appsRight + gap);
        leftBound = std::max(leftBound, static_cast<int>(rcTb.left) + margin);
    }

    const int step = widgetW + gap;
    int count = (rightEdge - leftBound - widgetW) / step + 1;
    count = std::clamp(count, 1, 12);

    const int y = rcTb.top + (rcTb.bottom - rcTb.top - widgetH) / 2;
    slots.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        RECT r{};
        r.right = rightEdge - i * step;
        r.left = r.right - widgetW;
        r.top = y;
        r.bottom = y + widgetH;
        slots.push_back(r);
    }
    stepDip = static_cast<int>(std::lround(static_cast<float>(step) / scale));
    return true;
}

int WidgetWindow::StoredSlotIndex(int slotCount, int stepDip) const {
    if (slotCount <= 1 || stepDip <= 0) return 0;
    const long long index = std::llround(static_cast<double>(m_posOffsetDip) / stepDip);
    return std::clamp(static_cast<int>(index), 0, slotCount - 1);
}

int WidgetWindow::NearestSlot(int centerX) const {
    int best = 0;
    int bestDistance = std::numeric_limits<int>::max();
    for (std::size_t i = 0; i < m_dragSlots.size(); ++i) {
        const int center = (m_dragSlots[i].left + m_dragSlots[i].right) / 2;
        const int distance = std::abs(center - centerX);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void WidgetWindow::StartDrag(HWND hwnd) {
    if (m_dragging || m_snapping || m_menuOpen) return;
    if (IsFullscreenAppActive() || !IsTaskbarShowing()) return;

    std::vector<RECT> slots;
    int stepDip = 0;
    if (!ComputeSlots(slots, stepDip, true) || slots.size() < 2) return;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    if (!GetWindowRect(hwnd, &m_dragStartRect)) return;

    m_dragSlots = std::move(slots);
    m_dragStepDip = stepDip > 0 ? stepDip : 176;
    m_headset.UpdateSlots(m_dragSlots);
    m_dragStartCursor = cursor;
    m_dragMoved = false;
    m_dragging = true;
    m_snapOldMouseSlot = m_currentMouseSlot;
    m_dragHeadsetSlot = m_headset.Slot();
    m_previewHeadsetActive = false;
    m_dragActive = NearestSlot((m_dragStartRect.left + m_dragStartRect.right) / 2);

    SetCapture(hwnd);
    SetHoverTarget(0.0f);
    m_slotOverlay.Show(m_dragSlots, m_dragActive, IsMenuDark(), GetDpiForWindow(hwnd));
}

void WidgetWindow::UpdateDrag(HWND hwnd) {
    if (!m_dragging) return;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;

    const int dx = cursor.x - m_dragStartCursor.x;
    // Порог, чтобы обычный клик не сдвигал виджет на пиксель.
    if (!m_dragMoved && std::abs(dx) < kDragThresholdPx) return;
    m_dragMoved = true;

    const int width = m_dragStartRect.right - m_dragStartRect.left;
    const int startLeft = static_cast<int>(m_dragStartRect.left);
    const int minLeft = m_dragSlots.back().left;
    const int maxLeft = m_dragSlots.front().left;
    const int left = std::clamp(startLeft + dx, minLeft, maxLeft);

    SetWindowPos(hwnd, HWND_TOPMOST, left, m_dragStartRect.top, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    const int active = NearestSlot(left + width / 2);
    if (active != m_dragActive) {
        m_dragActive = active;
        m_slotOverlay.SetActive(active, IsMenuDark());
    }

    // Живой предпросмотр обмена: как только мышь накрыла плитку наушников,
    // наушники сразу переезжают в освободившуюся ячейку мыши. Отпустим —
    // обмен зафиксируется; уведём курсор — вернутся на место.
    if (m_dragHeadsetSlot >= 0) {
        if (active == m_dragHeadsetSlot && !m_previewHeadsetActive) {
            m_previewHeadsetActive = true;
            m_headset.PreviewToSlot(m_snapOldMouseSlot);
        } else if (active != m_dragHeadsetSlot && m_previewHeadsetActive) {
            m_previewHeadsetActive = false;
            m_headset.PreviewToSlot(m_dragHeadsetSlot);
        }
    }
}

void WidgetWindow::FinishDrag(HWND hwnd, bool allowSnap) {
    if (!m_dragging) return;
    m_dragging = false;

    if (GetCapture() == hwnd) ReleaseCapture();
    m_slotOverlay.Hide();

    // Если отпустили не над наушниками, живой предпросмотр отменяем.
    if (m_previewHeadsetActive && m_dragActive != m_dragHeadsetSlot) {
        m_previewHeadsetActive = false;
        m_headset.PreviewToSlot(m_dragHeadsetSlot);
    }

    if (allowSnap && m_dragMoved) {
        RECT current{};
        if (GetWindowRect(hwnd, &current)) {
            StartSnap(hwnd, NearestSlot((current.left + current.right) / 2));
        }
    } else if (m_dragMoved) {
        // Отмена — вернуть на исходное место без анимации.
        SetWindowPos(hwnd, HWND_TOPMOST, m_dragStartRect.left, m_dragStartRect.top, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    m_dragMoved = false;
}

void WidgetWindow::CancelDrag() {
    if (!m_dragging && !m_snapping) return;
    m_dragging = false;
    m_snapping = false;
    m_dragMoved = false;
    if (m_previewHeadsetActive) {
        m_previewHeadsetActive = false;
        m_headset.PreviewToSlot(m_dragHeadsetSlot);
    }
    if (m_hwnd) {
        if (GetCapture() == m_hwnd) ReleaseCapture();
        KillTimer(m_hwnd, TIMER_SNAP);
    }
    m_slotOverlay.Hide();
}

void WidgetWindow::StartSnap(HWND hwnd, int slotIndex) {
    if (slotIndex < 0 || slotIndex >= static_cast<int>(m_dragSlots.size())) return;
    if (!GetWindowRect(hwnd, &m_snapFrom)) return;

    m_snapTo = m_dragSlots[slotIndex];
    m_snapIndex = slotIndex;
    m_snapStepDip = m_dragStepDip;
    m_snapStartTick = GetTickCount64();
    m_snapping = true;
    SetTimer(hwnd, TIMER_SNAP, 15, nullptr);
}

void WidgetWindow::TickSnap(HWND hwnd) {
    if (!m_snapping) {
        KillTimer(hwnd, TIMER_SNAP);
        return;
    }

    const ULONGLONG elapsed = GetTickCount64() - m_snapStartTick;
    float t = static_cast<float>(elapsed) / static_cast<float>(kSnapDurationMs);
    t = std::clamp(t, 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

    const int x = m_snapFrom.left +
                  static_cast<int>(std::lround((m_snapTo.left - m_snapFrom.left) * eased));
    const int y = m_snapFrom.top +
                  static_cast<int>(std::lround((m_snapTo.top - m_snapFrom.top) * eased));
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    if (t >= 1.0f) {
        KillTimer(hwnd, TIMER_SNAP);
        m_snapping = false;

        // Если мышь поставили на ячейку наушников — меняемся местами.
        if (m_headset.Slot() >= 0 && m_headset.Slot() == m_snapIndex) {
            m_headset.SetOffsetDip(m_snapOldMouseSlot * m_snapStepDip, true);
        }
        m_previewHeadsetActive = false;
        m_posOffsetDip = m_snapIndex * m_snapStepDip;
        SavePosOffset(m_posOffsetDip);
        Relayout();
    }
}

int WidgetWindow::LoadPosOffset() {
    HKEY hKey;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return 0;
    const LONG result = RegQueryValueExW(hKey, kPosValueName, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    if (result != ERROR_SUCCESS || type != REG_DWORD) return 0;
    if (value > 5000) return 0;  // мусор от другой версии
    return static_cast<int>(value);
}

void WidgetWindow::SavePosOffset(int dip) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr,
                        &hKey, nullptr) != ERROR_SUCCESS) return;
    DWORD value = static_cast<DWORD>(std::max(0, dip));
    RegSetValueExW(hKey, kPosValueName, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
}

bool WidgetWindow::ShouldShowHeadsetTile() const {
    if (m_headsetUserHidden) return false;
    if (!m_headset.Hwnd()) return false;  // окно не создалось — нечего показывать
    const AirPodsTelemetry pods = AppState::Instance().GetAirPods();
    // Только живые данные: отключились — плитка исчезает, а не показывает
    // заряд часовой давности как текущий.
    if (!pods.connected || !pods.hasData || !pods.HasAnyLevel()) return false;
    // После «Скрыть виджет» показываем только заряд, полученный заново:
    // старые показания не должны выглядеть как подключение.
    if (m_headsetHiddenSinceUnixMs != 0 && pods.lastSuccessUnixMs <= m_headsetHiddenSinceUnixMs) {
        return false;
    }
    return true;
}

void WidgetWindow::UpdateHeadsetState() {
    if (!m_hwnd) return;
    const AirPodsTelemetry pods = AppState::Instance().GetAirPods();
    const bool wanted = ShouldShowHeadsetTile();

    // Сравниваем именно «данные есть», а не «плитка стоит»: если места на
    // панели не хватает, Relayout каждый тик вызывать нельзя.
    if (wanted != m_headsetWanted) {
        m_headsetWanted = wanted;
        m_lastAirPods = pods;
        Relayout();
        return;
    }
    if (wanted && !pods.SameDisplayed(m_lastAirPods)) {
        m_lastAirPods = pods;
        m_headset.Invalidate();
        RefreshAnimationState();
    } else {
        m_lastAirPods = pods;
    }

    if (m_headsetProbeActive) {
        if (pods.hasData && pods.connected) {
            m_headsetProbeActive = false;
        } else if (GetTickCount64() > m_headsetProbeDeadline) {
            // Устройство подключилось, но это были не наушники: снова замолкаем.
            m_headsetProbeActive = false;
            m_headsetUserHidden = true;
            m_headsetWanted = false;
            AppState::Instance().SetAirPodsEnabled(false);
            Relayout();
        }
    }
}

void WidgetWindow::ResumeHeadsetProbe() {
    if (!m_headsetUserHidden || m_headsetProbeActive) return;
    // Скрытую плитку не опрашиваем. Любое изменение дерева устройств — повод
    // один раз проверить, не вернулись ли наушники; если нет, через 90 с
    // снова прекращаем сканирование.
    m_headsetUserHidden = false;
    m_headsetProbeActive = true;
    m_headsetProbeDeadline = GetTickCount64() + 90 * 1000;
    AppState::Instance().SetAirPodsEnabled(true);
    AppState::Instance().RequestAirPodsScan();
}

void WidgetWindow::HideHeadsetTile() {
    if (m_headsetUserHidden) return;
    m_headsetUserHidden = true;
    m_headsetWanted = false;
    m_headsetProbeActive = false;
    m_headsetHiddenSinceUnixMs = NowUnixMs();
    AppState::Instance().SetAirPodsEnabled(false);
    // Старые показания не должны выглядеть свежими после возобновления.
    AppState::Instance().UpdateAirPods(AirPodsTelemetry{});
    m_lastAirPods = {};
    m_headset.CancelDrag();
    Relayout();
}

bool WidgetWindow::CanStartDrag() const noexcept {
    return !m_dragging && !m_snapping && !m_menuOpen &&
           !IsFullscreenAppActive() && IsTaskbarShowing();
}

bool WidgetWindow::ComputeSlotsForDrag(std::vector<RECT>& slots, int& stepDip) const {
    return ComputeSlots(slots, stepDip, true);
}

void WidgetWindow::ShowSlotOverlay(const std::vector<RECT>& slots, int active) {
    if (!m_hwnd) return;
    m_slotOverlay.Show(slots, active, IsMenuDark(), GetDpiForWindow(m_hwnd));
}

void WidgetWindow::UpdateSlotOverlay(int active) {
    m_slotOverlay.SetActive(active, IsMenuDark());
}

void WidgetWindow::HideSlotOverlay() {
    m_slotOverlay.Hide();
}

void WidgetWindow::PreviewMouseToSlot(int slot) {
    if (!m_hwnd || m_dragging || m_snapping) return;
    if (slot < 0 || slot >= static_cast<int>(m_lastSlots.size())) return;
    const RECT& cell = m_lastSlots[slot];
    SetWindowPos(m_hwnd, HWND_TOPMOST, cell.left, cell.top,
                 cell.right - cell.left, cell.bottom - cell.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    m_mousePreviewActive = true;
}

void WidgetWindow::EndMousePreview() {
    if (!m_mousePreviewActive) return;
    m_mousePreviewActive = false;
    if (!m_hwnd) return;
    if (m_currentMouseSlot >= 0 && m_currentMouseSlot < static_cast<int>(m_lastSlots.size())) {
        const RECT& cell = m_lastSlots[m_currentMouseSlot];
        SetWindowPos(m_hwnd, HWND_TOPMOST, cell.left, cell.top,
                     cell.right - cell.left, cell.bottom - cell.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
}

void WidgetWindow::SwapTiles(int mouseSlot, int headsetSlot) {
    m_mousePreviewActive = false;
    const int step = m_currentStepDip > 0 ? m_currentStepDip : 176;
    m_posOffsetDip = std::max(0, mouseSlot) * step;
    SavePosOffset(m_posOffsetDip);
    m_headset.SetOffsetDip(std::max(0, headsetSlot) * step, true);
    Relayout();
}

void WidgetWindow::RefreshAnimationState() {
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        UpdateAnimationTimer(false);
        return;
    }
    const bool visible = IsWindowVisible(m_hwnd) != FALSE;
    const DeviceTelemetry telemetry = AppState::Instance().GetTelemetry();
    const AirPodsTelemetry pods = AppState::Instance().GetAirPods();
    // После восстановления из кэша данные могут быть старыми: не включаем
    // пульсацию «низкого заряда» по показаниям недельной давности.
    const std::int64_t mouseAge = AppState::Instance().SecondsSinceLastSuccess();
    const bool mouseFresh = mouseAge >= 0 && mouseAge <= 600;
    const std::uint64_t nowMs = NowUnixMs();
    const bool podsFresh = pods.lastSuccessUnixMs != 0 && nowMs >= pods.lastSuccessUnixMs &&
                           (nowMs - pods.lastSuccessUnixMs) <= 600000;
    const bool podsLow = m_headset.IsVisible() && podsFresh &&
                         ((pods.leftLevel >= 0 && pods.leftLevel <= 15) ||
                          (pods.rightLevel >= 0 && pods.rightLevel <= 15) ||
                          (pods.caseLevel >= 0 && pods.caseLevel <= 15));
    const bool needAnim = visible &&
        ((mouseFresh && (telemetry.isCharging ||
                         (telemetry.hasData && telemetry.batteryLevel >= 0 && telemetry.batteryLevel <= 15))) ||
         podsLow);
    UpdateAnimationTimer(needAnim);
}

void WidgetWindow::NotifyTelemetryChanged() const noexcept {
    if (m_hwnd && IsWindow(m_hwnd)) PostMessageW(m_hwnd, WM_APP_TELEMETRY, 0, 0);
}

LRESULT CALLBACK WidgetWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WidgetWindow* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = static_cast<WidgetWindow*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<WidgetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (pThis) {
        __try {
            return pThis->HandleMessage(hwnd, msg, wParam, lParam);
        }
        __except (LogStructuredException(GetExceptionCode())) {
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void CALLBACK WidgetWindow::StaticWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    WidgetWindow* self = s_instance;
    if (!self || idObject != OBJID_WINDOW) return;

    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        // Таскбар или его дочернее окно (TrayNotifyWnd) сдвинулись — пересчитываем позицию.
        if (self->m_hTaskbarWnd && (hwnd == self->m_hTaskbarWnd || IsChild(self->m_hTaskbarWnd, hwnd))) {
            self->Relayout();
        }
        return;
    }

    if (event == EVENT_SYSTEM_FOREGROUND) {
        // Переключение фокуса — самый быстрый момент, чтобы остановить или вернуть опрос.
        const bool fullscreen = self->IsFullscreenAppActive();
        AppState::Instance().SetFullscreenAppActive(fullscreen);
        if (fullscreen) {
            ShowWindow(self->m_hwnd, SW_HIDE);
            self->RefreshAnimationState();
        } else {
            self->Relayout();
            self->ForceRestore();
        }
    }
}

int WidgetWindow::LogStructuredException(DWORD code) noexcept {
    WCHAR buf[64];
    wsprintfW(buf, L"[BatteryWidget] SEH Intercepted: 0x%08X\n", code);
    OutputDebugStringW(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

LRESULT WidgetWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == m_wmShellHook && m_wmShellHook != 0) {
        // Оболочка изменилась — возвращаем окна наверх, но не пересчитываем
        // сетку: shell-хук приходит часто, а UI Automation стоит миллисекунды.
        ForceRestore();
        m_headset.ForceRestore();
        return 0;
    }

    switch (msg) {
    case WM_APP_TELEMETRY: {
        RefreshAnimationState();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND: return 1;

    case WM_TIMER:
        if (wParam == TIMER_ANIM) {
            if (!IsWindowVisible(hwnd)) {
                UpdateAnimationTimer(false);
                return 0;
            }
            m_animPhase += 0.12f;
            if (m_animPhase > 6.283185f) m_animPhase -= 6.283185f;
            InvalidateRect(hwnd, nullptr, FALSE);
            m_headset.TickAnimation(m_animPhase);
        } else if (wParam == TIMER_CHECK) {
            // Страховочный опрос раз в 500 мс: событие переднего плана могло не дойти.
            const bool fullscreen = IsFullscreenAppActive();
            AppState::Instance().SetFullscreenAppActive(fullscreen);
            if (fullscreen || !IsTaskbarShowing()) {
                if (IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_HIDE);
                    m_hoverTracking = false;
                    m_hoverProgress = 0.0f;
                    m_hoverTarget = 0.0f;
                }
                m_headset.Relayout({}, 0, 0, false);
            } else if (!IsWindowVisible(hwnd)) {
                Relayout();
            }
            UpdateHeadsetState();
            RefreshAnimationState();
        } else if (wParam == TIMER_HOVER) {
            TickHover();
        } else if (wParam == TIMER_SNAP) {
            TickSnap(hwnd);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (m_dragging) {
            UpdateDrag(hwnd);
            return 0;
        }
        if (!m_hoverTracking) {
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            if (TrackMouseEvent(&tme)) m_hoverTracking = true;
        }
        if (!m_menuOpen) SetHoverTarget(1.0f);
        return 0;

    case WM_LBUTTONDOWN:
        StartDrag(hwnd);
        return 0;

    case WM_LBUTTONUP:
        FinishDrag(hwnd, true);
        return 0;

    case WM_CAPTURECHANGED:
        // Захват потеряли (Alt+Tab, смена фокуса) — аккуратно доводим до ячейки.
        if (m_dragging) FinishDrag(hwnd, true);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && m_dragging) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;

    case WM_MOUSELEAVE:
        m_hoverTracking = false;
        SetHoverTarget(0.0f);
        return 0;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE ||
            wParam == DBT_DEVNODES_CHANGED) {
            if (wParam != DBT_DEVNODES_CHANGED) {
                AppState::Instance().RequestRescan();
            }
            // Наушники подключили/убрали в кейс — проверяем Bluetooth сразу.
            // Если плитка была скрыта вручную, это единственный сигнал,
            // по которому опрос вообще возобновляется.
            if (m_headsetUserHidden) {
                ResumeHeadsetProbe();
            } else {
                // Подключение/отключение Bluetooth-устройства — не ждём
                // очередного окна сканирования.
                AppState::Instance().RequestAirPodsScan();
            }
        }
        return TRUE;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            AppState::Instance().RequestRescan();
            return TRUE;
        }
        break;

    case WM_DPICHANGED:
        Relayout();
        m_renderer.DiscardDeviceResources();
        m_headset.DiscardDeviceResources();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SETTINGCHANGE: {
        const bool newDark = QuerySystemIsDark();
        if (newDark != m_isDarkTheme) {
            m_isDarkTheme = newDark;
            // Смена системной темы меняет цвета только у Glass; Renderer сам
            // пересоздаст ресурсы, если для текущей темы это важно.
            InvalidateRect(hwnd, nullptr, FALSE);
            m_headset.Invalidate();
        }
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) return 0;
        break;

    case WM_SHOWWINDOW:
        if (wParam == FALSE && !IsFullscreenAppActive() && IsTaskbarShowing()) return 0;
        if (wParam != FALSE) {
            RefreshAnimationState();
        } else {
            // Скрыли виджет — подсветка не должна «залипнуть» на будущий показ.
            m_hoverTracking = false;
            m_hoverProgress = 0.0f;
            m_hoverTarget = 0.0f;
        }
        break;

    case WM_WINDOWPOSCHANGING: {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
        if (!IsFullscreenAppActive() && IsTaskbarShowing()) {
            pos->flags &= ~SWP_HIDEWINDOW;
            pos->flags |= SWP_SHOWWINDOW;
            pos->hwndInsertAfter = HWND_TOPMOST;
        }
        return 0;
    }

    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowContextMenu(hwnd, m_hInst);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_REFRESH: AppState::Instance().RequestRescan(); break;
        case IDM_AUTORUN: ToggleAutoRun(m_hInst); break;
        case IDM_THEME_GLASS: ApplyTheme(WidgetTheme::Glass, true); break;
        case IDM_THEME_DARK: ApplyTheme(WidgetTheme::Dark, true); break;
        case IDM_THEME_LIGHT: ApplyTheme(WidgetTheme::Light, true); break;
        case IDM_REPORT: ShowDiagnosticsReport(hwnd); break;
        case IDM_EXIT:
            SignalUserExit();
            PostQuitMessage(0);
            break;
        default: break;
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        m_renderer.Render(hwnd, AppState::Instance().GetTelemetry(), m_theme, m_isDarkTheme,
                          m_hoverProgress, m_animPhase);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

std::wstring WidgetWindow::BuildAutoRunCommand(HMODULE hInst) {
    std::array<WCHAR, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(hInst, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};

    std::wstring command(path.data(), length);
    const auto lastSlash = command.find_last_of(L'\\');
    if (lastSlash == std::wstring::npos) return {};
    command.resize(lastSlash + 1);
    command += L"injector.exe";
    // Путь может содержать пробелы — в Run-ключе его нужно заключать в кавычки.
    // --watch переинжектит виджет после рестарта Explorer.
    return L"\"" + command + L"\" --watch";
}

void WidgetWindow::ShowContextMenu(HWND hwnd, HMODULE hInst) {
    if (m_menuOpen || !hwnd || !IsWindow(hwnd)) return;

    SyncAutoRunCommand(hInst);

    RECT rc; GetWindowRect(hwnd, &rc);
    const DeviceTelemetry telemetry = AppState::Instance().GetTelemetry();

    std::vector<ContextMenuItem> items;

    ContextMenuItem title;
    title.kind = ContextMenuItem::Kind::Title;
    title.text = telemetry.deviceName.empty() ? L10n::Get(Str::DeviceFallback) : telemetry.deviceName;
    // Обычный клик по заголовку ничего не делает; меню раскрывает этот id
    // только после пяти кликов подряд (скрытая команда «собрать отчёт»).
    title.id = IDM_REPORT;
    items.push_back(std::move(title));

    ContextMenuItem status;
    status.kind = ContextMenuItem::Kind::Info;
    status.text = L10n::Get(Str::Battery);
    if (!telemetry.hasData) status.text += L10n::Get(Str::SearchDevice);
    else if (telemetry.batteryLevel >= 0) status.text += std::to_wstring(telemetry.batteryLevel) + L"%";
    else status.text += L10n::Get(Str::NoData);
    if (telemetry.hasData && telemetry.isCharging) status.text += L10n::Get(Str::ChargingSuffix);
    if (telemetry.hasData) {
        // Даже «свежие» данные быстро устаревают, если мышь уснула: показываем,
        // когда заряд был реально прочитан.
        const std::int64_t age = AppState::Instance().SecondsSinceLastSuccess();
        if (age >= 0) status.text += L" · " + L10n::Ago(age);
    }
    items.push_back(std::move(status));

    if (telemetry.hasData && telemetry.voltageMv > 0) {
        ContextMenuItem voltage;
        voltage.kind = ContextMenuItem::Kind::Info;
        voltage.text = L10n::Get(Str::Voltage) + std::to_wstring(telemetry.voltageMv) + L" mV";
        items.push_back(std::move(voltage));
    }
    if (telemetry.hasData && telemetry.estimatedMinutes > 0) {
        ContextMenuItem time;
        time.kind = ContextMenuItem::Kind::Info;
        // При зарядке это время до 100%, при разряде — до нуля: подписи разные.
        time.text = L10n::Get(telemetry.isCharging ? Str::EstimatedToFull : Str::EstimatedTime) +
                    L10n::Remaining(telemetry.estimatedMinutes);
        items.push_back(std::move(time));
    }

    // AirPods: отдельные значения рядом с мышью, чтобы не смешивать устройства.
    // (в меню мыши не показываем — у наушников своё меню)

    ContextMenuItem themeItem;
    themeItem.text = L10n::Get(Str::Theme);
    themeItem.id = 0;
    auto addTheme = [this, &themeItem](const wchar_t* text, UINT id, WidgetTheme theme) {
        ContextMenuItem entry;
        entry.text = text;
        entry.id = id;
        entry.checked = (m_theme == theme);
        themeItem.submenu.push_back(std::move(entry));
    };
    addTheme(L10n::Get(Str::ThemeGlass), IDM_THEME_GLASS, WidgetTheme::Glass);
    addTheme(L10n::Get(Str::ThemeDark), IDM_THEME_DARK, WidgetTheme::Dark);
    addTheme(L10n::Get(Str::ThemeLight), IDM_THEME_LIGHT, WidgetTheme::Light);
    items.push_back(std::move(themeItem));

    ContextMenuItem refresh;
    refresh.text = L10n::Get(Str::RefreshNow);
    refresh.id = IDM_REFRESH;
    items.push_back(std::move(refresh));

    ContextMenuItem autoRun;
    autoRun.text = L10n::Get(Str::StartWithWindows);
    autoRun.id = IDM_AUTORUN;
    autoRun.checked = IsAutoRunEnabled();
    items.push_back(std::move(autoRun));

    ContextMenuItem exitItem;
    exitItem.text = L10n::Get(Str::CloseWidget);
    exitItem.id = IDM_EXIT;
    items.push_back(std::move(exitItem));

    // Пока меню открыто, оно владеет мышью: подсветку плитки гасим,
    // а после закрытия восстанавливаем по фактическому положению курсора.
    m_menuOpen = true;
    if (m_hoverTracking) {
        TRACKMOUSEEVENT cancel{ sizeof(TRACKMOUSEEVENT), TME_CANCEL, hwnd, 0 };
        TrackMouseEvent(&cancel);
        m_hoverTracking = false;
    }
    SetHoverTarget(0.0f);

    const UINT chosen = ShowWin11Menu(hInst, hwnd, IsMenuDark(), rc, items);

    m_menuOpen = false;
    POINT cursor{};
    if (GetCursorPos(&cursor) && GetWindowRect(hwnd, &rc) && PtInRect(&rc, cursor)) {
        TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
        if (TrackMouseEvent(&tme)) m_hoverTracking = true;
        SetHoverTarget(1.0f);
    }

    // Выбор пункта прогоняем через обычный WM_COMMAND — обработчики те же, что были у меню.
    if (chosen != 0) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(chosen, 0), 0);
}

void WidgetWindow::ShowDiagnosticsReport(HWND hwnd) {
    const AppState& state = AppState::Instance();

    Report::Snapshot snapshot;
    snapshot.telemetry = state.GetTelemetry();
    snapshot.airPods = state.GetAirPods();
    snapshot.airPodsEnabled = state.IsAirPodsEnabled();
    snapshot.theme = static_cast<int>(m_theme);
    snapshot.systemDark = m_isDarkTheme;
    snapshot.fullscreenApp = state.IsFullscreenAppActive();
    snapshot.secondsSincePoll = state.SecondsSinceLastSuccess();
    snapshot.lastPollUnixMs = state.LastSuccessUnixMs();
    snapshot.pollTrace = state.GetPollTrace();

    const std::wstring path = Report::Write(hwnd, snapshot);
    const std::wstring title = L10n::Get(Str::ReportTitle);

    if (path.empty()) {
        MessageBoxW(nullptr, L10n::Get(Str::ReportFailed), title.c_str(), MB_ICONERROR | MB_OK);
        return;
    }
    Diag::Log(L"diagnostics report saved: %ls", path.c_str());

    wchar_t message[1024]{};
    swprintf_s(message, 1024, L10n::Get(Str::ReportSaved), path.c_str());
    MessageBoxW(nullptr, message, title.c_str(), MB_ICONINFORMATION | MB_OK);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool WidgetWindow::IsFullscreenAppActive() const noexcept {
    HWND hFore = GetForegroundWindow();
    if (!hFore || hFore == m_hTaskbarWnd || hFore == m_hwnd) return false;
    std::array<WCHAR, 64> cls{};
    GetClassNameW(hFore, cls.data(), static_cast<int>(cls.size()));
    if (wcscmp(cls.data(), L"Progman") == 0 || wcscmp(cls.data(), L"WorkerW") == 0) return false;
    RECT rcApp; GetWindowRect(hFore, &rcApp);
    HMONITOR hMon = MonitorFromWindow(hFore, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMon, &mi)) {
        return (rcApp.left <= mi.rcMonitor.left && rcApp.top <= mi.rcMonitor.top &&
                rcApp.right >= mi.rcMonitor.right && rcApp.bottom >= mi.rcMonitor.bottom);
    }
    return false;
}

bool WidgetWindow::IsTaskbarShowing() const noexcept {
    if (!m_hTaskbarWnd || !IsWindow(m_hTaskbarWnd)) return false;
    APPBARDATA abd = { sizeof(APPBARDATA) };
    abd.hWnd = m_hTaskbarWnd;
    auto state = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    if ((state & ABS_AUTOHIDE) != 0) {
        // Автоскрытие работает на любой стороне экрана, поэтому смотрим долю
        // площади таскбара, реально попавшую на монитор, а не только нижнюю кромку.
        RECT rcTb; GetWindowRect(m_hTaskbarWnd, &rcTb);
        HMONITOR hMon = MonitorFromWindow(m_hTaskbarWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfoW(hMon, &mi)) {
            RECT visible{};
            const int tbWidth = rcTb.right - rcTb.left;
            const int tbHeight = rcTb.bottom - rcTb.top;
            if (tbWidth > 0 && tbHeight > 0) {
                if (!IntersectRect(&visible, &rcTb, &mi.rcMonitor)) return false;
                const double visibleRatio =
                    static_cast<double>((visible.right - visible.left) * (visible.bottom - visible.top)) /
                    (static_cast<double>(tbWidth) * tbHeight);
                if (visibleRatio < 0.5) return false;
            }
        }
    }
    return IsWindowVisible(m_hTaskbarWnd) != FALSE;
}

bool WidgetWindow::QuerySystemIsDark() noexcept {
    HKEY hKey; DWORD val = 0; DWORD sz = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS) {
            RegCloseKey(hKey); return val == 0;
        }
        RegCloseKey(hKey);
    }
    return true;
}

WidgetTheme WidgetWindow::LoadTheme() {
    HKEY hKey;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return WidgetTheme::Glass;
    }
    const LONG result = RegQueryValueExW(hKey, kThemeValueName, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    if (result != ERROR_SUCCESS || type != REG_DWORD) return WidgetTheme::Glass;
    // Незнакомое значение (например, из более новой версии) — безопасный дефолт.
    if (value > static_cast<DWORD>(WidgetTheme::Light)) return WidgetTheme::Glass;
    return static_cast<WidgetTheme>(value);
}

void WidgetWindow::SaveTheme(WidgetTheme theme) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr,
                        &hKey, nullptr) != ERROR_SUCCESS) return;
    const DWORD value = static_cast<DWORD>(theme);
    RegSetValueExW(hKey, kThemeValueName, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
}

void WidgetWindow::ApplyTheme(WidgetTheme theme, bool persist) {
    if (persist) SaveTheme(theme);
    if (theme == m_theme) return;
    m_theme = theme;
    Diag::Log(L"theme changed: %d", static_cast<int>(theme));
    // Цвета кистей зависят от темы — пересоздаём ресурсы и перерисовываем плитки.
    m_renderer.DiscardDeviceResources();
    m_headset.DiscardDeviceResources();
    if (m_hwnd && IsWindow(m_hwnd)) InvalidateRect(m_hwnd, nullptr, FALSE);
    m_headset.Invalidate();
}

bool WidgetWindow::IsMenuDark() const noexcept {
    if (m_theme == WidgetTheme::Dark) return true;
    if (m_theme == WidgetTheme::Light) return false;
    return m_isDarkTheme;  // матовая тема следует системной, как и панель задач
}

bool WidgetWindow::IsAutoRunEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    const LONG result = RegQueryValueExW(hKey, kAutoRunValueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
}

void WidgetWindow::ToggleAutoRun(HMODULE hInst) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS) return;

    if (IsAutoRunEnabled()) {
        RegDeleteValueW(hKey, kAutoRunValueName);
    } else {
        const std::wstring command = BuildAutoRunCommand(hInst);
        if (!command.empty()) {
            RegSetValueExW(hKey, kAutoRunValueName, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(command.c_str()),
                           static_cast<DWORD>((command.size() + 1) * sizeof(WCHAR)));
        }
    }
    RegCloseKey(hKey);
}

void WidgetWindow::SyncAutoRunCommand(HMODULE hInst) {
    if (!IsAutoRunEnabled()) return;

    const std::wstring expected = BuildAutoRunCommand(hInst);
    if (expected.empty()) return;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS) return;

    std::array<WCHAR, 512> current{};
    DWORD size = static_cast<DWORD>(sizeof(current));
    DWORD type = 0;
    const LONG result = RegQueryValueExW(hKey, kAutoRunValueName, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(current.data()), &size);
    const bool upToDate = result == ERROR_SUCCESS && type == REG_SZ && expected == current.data();
    if (!upToDate) {
        // Автозапуск включён, но путь/аргументы устарели (например, папка переехала) — чиним.
        RegSetValueExW(hKey, kAutoRunValueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(expected.c_str()),
                       static_cast<DWORD>((expected.size() + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(hKey);
}

void WidgetWindow::SignalUserExit() noexcept {
    if (HANDLE hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kUserExitEventName)) {
        SetEvent(hEvent);
        CloseHandle(hEvent);
    }
}
