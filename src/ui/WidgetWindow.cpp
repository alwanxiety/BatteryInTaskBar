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
constexpr UINT_PTR TIMER_ZORDER = 105;
constexpr UINT WM_APP_TELEMETRY = WM_APP + 0x51;
constexpr UINT WM_APP_REPORT_DONE = WM_APP + 0x52;
constexpr UINT WM_APP_APPS_EDGE = WM_APP + 0x53;

constexpr wchar_t kAutoRunValueName[] = L"BatteryInTaskBar";
constexpr wchar_t kUserExitEventName[] = L"Local\\BatteryInTaskBar.Exit";
constexpr wchar_t kSettingsKey[] = L"Software\\BatteryInTaskBar";
constexpr wchar_t kThemeValueName[] = L"Theme";
constexpr wchar_t kPosValueName[] = L"PosOffset";
constexpr wchar_t kHeadsetHiddenValueName[] = L"HeadsetHidden";

// Геометрия виджета и ячеек в DIP.
constexpr float kWidgetWidthDip = 168.0f;
constexpr float kWidgetHeightDip = 38.0f;
constexpr float kWidgetMarginDip = 4.0f;
constexpr float kSlotGapDip = 8.0f;
constexpr int kDragThresholdPx = 3;
constexpr int kSnapDurationMs = 140;
// Сколько времени работает пульсация после смены состояния. Дальше плитка
// остаётся статичной: иначе заряжающаяся мышка рисует 30 кадров в секунду
// часами и будит GPU Explorer'а.
constexpr ULONGLONG kPulseWindowMs = 60 * 1000;

enum ZBID : DWORD {
    ZBID_IMMERSIVE_NOTIFICATION = 4,
    // На Windows 11 XAML-острова оболочки (Task View, превью задач, календарь)
    // живут в полосе 16. В полосе 4 акриловая поверхность оболочки может
    // накрывать плитку при показе превью, поэтому используем ту же полосу.
    ZBID_SHELL_XAML = 16
};

using pfnSetWindowBand = BOOL(WINAPI*)(HWND hWnd, HWND hwndInsertAfter, DWORD dwBand);

WidgetWindow::WidgetWindow(HMODULE hModule) : m_hInst(hModule) {}

bool WidgetWindow::Create() {
    m_isDarkTheme = QuerySystemIsDark();
    m_theme = LoadTheme();
    m_posOffsetDip = LoadPosOffset();
    // Последние показания — до первого Relayout, чтобы не мелькало «поиск».
    TelemetryCache::Restore();
    // «Скрыть виджет» переживает перезапуск Explorer. Если наушники уже
    // отключены, флаг снимется при первом же известном состоянии подключения.
    m_headsetUserHidden = LoadHeadsetHidden();
    m_headsetStillConnected = m_headsetUserHidden;
    AppState::Instance().SetAirPodsEnabled(!m_headsetUserHidden);
    AppState::Instance().RequestAirPodsScan();
    if (!m_renderer.Initialize()) {
        Diag::Log(L"widget: Direct2D/DirectWrite init failed");
        return false;
    }
    if (!m_slotOverlay.Create(m_hInst)) {
        Diag::Log(L"widget: slot overlay class registration failed");
        return false;
    }
    // Плитка наушников не критична: если её окно не создалось, мышь всё равно работает.
    m_headset.Create(m_hInst);

    m_hTaskbarWnd = WindowScanner::FindTaskbarWindow();
    if (!m_hTaskbarWnd) {
        Diag::Log(L"widget: Shell_TrayWnd not found");
        return false;
    }

    if (!RegisterWindowClass()) {
        Diag::Log(L"widget: window class registration failed");
        return false;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        m_className.c_str(), kWidgetWindowTitle,
        WS_POPUP | WS_VISIBLE,
        0, 0, 168, 38,
        nullptr, nullptr, m_hInst, this
    );

    if (!m_hwnd) {
        Diag::Log(L"widget: CreateWindowExW failed, error=%lu", GetLastError());
        UnregisterWindowClass();
        return false;
    }
    s_instance = this;

    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    if (HMODULE hUser32 = GetModuleHandleW(L"user32.dll")) {
        if (auto pSetWindowBand = reinterpret_cast<pfnSetWindowBand>(GetProcAddress(hUser32, "SetWindowBand"))) {
            pSetWindowBand(m_hwnd, nullptr, ZBID_SHELL_XAML);
        }
        using pfnGetWindowBand = BOOL(WINAPI*)(HWND, DWORD*);
        if (auto pGetWindowBand = reinterpret_cast<pfnGetWindowBand>(GetProcAddress(hUser32, "GetWindowBand"))) {
            DWORD band = 0;
            if (pGetWindowBand(m_hwnd, &band)) Diag::Log(L"widget: window band=%lu", band);
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
    m_appsEdge.Start(m_hTaskbarWnd, m_hwnd, WM_APP_APPS_EDGE);
    Relayout();
    return true;
}

void WidgetWindow::Destroy() {
    CancelDrag();
    // Отчёт и UIA-опрос идут в своих потоках: сначала останавливаем их, потом
    // разрушаем окна, чтобы никто не отправил сообщение в мёртвый hwnd.
    if (m_reportThread.joinable()) m_reportThread.join();
    m_reportBusy = false;
    m_appsEdge.Stop();
    m_headset.Destroy();
    m_slotOverlay.Destroy();
    if (m_hEventHookForeground) { UnhookWinEvent(m_hEventHookForeground); m_hEventHookForeground = nullptr; }
    if (m_hEventHookLocation) { UnhookWinEvent(m_hEventHookLocation); m_hEventHookLocation = nullptr; }
    if (m_hDevNotify) { UnregisterDeviceNotification(m_hDevNotify); m_hDevNotify = nullptr; }
    if (m_hwnd) {
        KillTimer(m_hwnd, TIMER_ANIM);
        KillTimer(m_hwnd, TIMER_CHECK);
        KillTimer(m_hwnd, TIMER_HOVER);
        KillTimer(m_hwnd, TIMER_ZORDER);
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
    if (!UnregisterClassW(m_className.c_str(), m_hInst)) {
        Diag::Log(L"widget: UnregisterClass failed, error=%lu", GetLastError());
    }
    m_className.clear();
}

void WidgetWindow::ScheduleRelayout() {
    const ULONGLONG now = GetTickCount64();
    if (m_lastLayoutTick != 0 && now >= m_lastLayoutTick && now - m_lastLayoutTick < 100) {
        m_layoutDirty = true;
        return;
    }
    m_layoutDirty = false;
    m_lastLayoutTick = now;
    Relayout();
}

void WidgetWindow::SetFullscreenState(bool active) {
    if (active != m_fullscreenState) {
        m_fullscreenState = active;
        if (active) LogForegroundWindow();
    }
    AppState::Instance().SetFullscreenAppActive(active);
}

void WidgetWindow::LogForegroundWindow() const {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return;
    std::array<WCHAR, 96> cls{};
    GetClassNameW(foreground, cls.data(), static_cast<int>(cls.size()));
    RECT rc{};
    GetWindowRect(foreground, &rc);
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    Diag::Log(L"fullscreen: fg=%ls rect=%ld,%ld,%ld,%ld pid=%lu", cls.data(), rc.left, rc.top,
              rc.right, rc.bottom, pid);
}

void WidgetWindow::Relayout() {
    if (!m_hwnd || !m_hTaskbarWnd || !IsWindow(m_hTaskbarWnd)) return;

    const bool fullscreen = IsFullscreenAppActive();
    // Тот же признак, что скрывает виджет, останавливает и опрос HID (см. Application).
    SetFullscreenState(fullscreen);
    const bool taskbarShowing = IsTaskbarShowing();
    if (fullscreen || !taskbarShowing) {
        CancelDrag();
        m_headset.CancelDrag();
        if (IsWindowVisible(m_hwnd)) {
            if (!fullscreen) Diag::Log(L"hide: taskbar is not showing");
            ShowWindow(m_hwnd, SW_HIDE);
        }
        m_lastAppliedValid = false;
        m_headset.Relayout({}, 0, 0, false);
        RefreshAnimationState();
        return;
    }

    // Пока открыто меню, окна не двигаем: иначе меню остаётся на месте,
    // а плитка «уезжает» из-под него. После закрытия меню Relayout вызовут явно.
    if (m_menuOpen) return;

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
        const RECT target{ xPos, yPos, xPos + widgetWidth, yPos + widgetHeight };
        const bool sameRect = m_lastAppliedValid && EqualRect(&target, &m_lastAppliedRect);
        if (!sameRect || !IsWindowVisible(m_hwnd)) {
            SetWindowPos(m_hwnd, HWND_TOPMOST, xPos, yPos, widgetWidth, widgetHeight,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        }
        m_lastAppliedRect = target;
        m_lastAppliedValid = true;
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

/// Процесс оболочки Windows: Explorer и её отдельные хосты (меню «Пуск»,
/// поиск, быстрые настройки). Их всплывающие окна бывают размером на весь
/// монитор, но полноэкранным приложением они не являются.
bool IsShellHostProcess(DWORD processId) noexcept {
    if (processId == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;

    wchar_t path[MAX_PATH]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    bool shell = false;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        static constexpr const wchar_t* kShellHosts[] = {
            L"explorer.exe",
            L"StartMenuExperienceHost.exe",
            L"ShellExperienceHost.exe",
            L"SearchHost.exe",
            L"TextInputHost.exe",
        };
        for (const wchar_t* name : kShellHosts) {
            if (_wcsicmp(base, name) == 0) {
                shell = true;
                break;
            }
        }
    }
    CloseHandle(process);
    return shell;
}

}  // namespace

bool WidgetWindow::ComputeSlots(std::vector<RECT>& slots, int& stepDip, bool forceAppsRefresh) {
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
    int appsRight = 0;
    if (forceAppsRefresh) m_appsEdge.RequestRefresh();
    // Снимок снимается в фоновом потоке: UIA на UI-потоке Explorer подвешивал
    // оболочку на сотни миллисекунд при каждом пересчёте.
    if (m_appsEdge.Get(rcTb, rcTray, 120000, appsRight)) {
        if (appsRight > 0 && appsRight < rcTray.left) {
            // Ячейка, которая хотя бы на пиксель заходит на кнопки, отбрасывается:
            // leftBound — самый левый допустимый край.
            leftBound = std::min(leftBound, appsRight + gap);
            leftBound = std::max(leftBound, static_cast<int>(rcTb.left) + margin);
        }
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
    m_lastAppliedValid = false;
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
    m_lastAppliedValid = false;

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

bool WidgetWindow::LoadHeadsetHidden() {
    HKEY hKey;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegQueryValueExW(hKey, kHeadsetHiddenValueName, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    return result == ERROR_SUCCESS && type == REG_DWORD && value != 0;
}

void WidgetWindow::SaveHeadsetHidden(bool hidden) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr,
                        &hKey, nullptr) != ERROR_SUCCESS) return;
    const DWORD value = hidden ? 1 : 0;
    RegSetValueExW(hKey, kHeadsetHiddenValueName, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
}

bool WidgetWindow::ShouldShowHeadsetTile() const {
    if (m_headsetUserHidden) return false;
    if (!m_headset.Hwnd()) return false;  // окно не создалось — нечего показывать
    const AirPodsTelemetry pods = AppState::Instance().GetAirPods();
    // Плитка живёт, пока Windows считает наушники подключёнными. Данные о
    // заряде могут прийти на секунду позже — тогда на плите будет «…»,
    // но сама плитка не появляется с задержкой.
    if (!pods.connected) return false;
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

    // «Скрыть виджет» держится до следующего реального подключения. Пока
    // наушники подключены, флаг не снимаем; как только вотчер сообщил об
    // отключении — скрытие больше не нужно, следующее подключение покажет
    // плитку снова. Никакие WM_DEVICECHANGE его не «воскрешают».
    if (m_headsetUserHidden) {
        if (pods.connectionKnown) {
            if (pods.connected) {
                m_headsetStillConnected = true;
            } else if (m_headsetStillConnected) {
                m_headsetStillConnected = false;
                m_headsetUserHidden = false;
                SaveHeadsetHidden(false);
                Diag::Log(L"headset: disconnected while hidden, taking the tile back");
            }
        }
        m_lastAirPods = pods;
        return;
    }

    // Пока плитка скрыта вручную, скан выключен. Как только вотчер увидит
    // подключение, включаем его снова, чтобы получить свежий заряд.
    if (pods.connected && !AppState::Instance().IsAirPodsEnabled()) {
        AppState::Instance().SetAirPodsEnabled(true);
        AppState::Instance().RequestAirPodsScan();
    }

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
}

void WidgetWindow::HideHeadsetTile() {
    if (m_headsetUserHidden) return;
    m_headsetUserHidden = true;
    m_headsetWanted = false;
    const AirPodsTelemetry pods = AppState::Instance().GetAirPods();
    // Если состояние неизвестно, считаем, что наушники подключены: скрытие
    // не должно сниматься первым же «пустым» опросом.
    m_headsetStillConnected = pods.connectionKnown ? pods.connected : true;
    m_headsetHiddenSinceUnixMs = NowUnixMs();
    SaveHeadsetHidden(true);
    AppState::Instance().SetAirPodsEnabled(false);
    m_headset.CancelDrag();
    Relayout();
}

bool WidgetWindow::CanStartDrag() const noexcept {
    return !m_dragging && !m_snapping && !m_menuOpen &&
           !IsFullscreenAppActive() && IsTaskbarShowing();
}

bool WidgetWindow::ComputeSlotsForDrag(std::vector<RECT>& slots, int& stepDip) {
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
    m_lastAppliedValid = false;
}

void WidgetWindow::EndMousePreview() {
    if (!m_mousePreviewActive) return;
    m_mousePreviewActive = false;
    m_lastAppliedValid = false;
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
        m_headsetAnimNeeded = false;
        m_headset.SetAnimation(false);
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
    const bool mouseLowOrCharging =
        mouseFresh && (telemetry.isCharging ||
                       (telemetry.hasData && telemetry.batteryLevel >= 0 &&
                        telemetry.batteryLevel <= 15));
    const bool needAnim = visible && (mouseLowOrCharging || podsLow);

    // Пульсация нужна как визуальный сигнал о смене состояния, а не как вечный
    // источник 30 кадров в секунду. Даём ей минуту после включения.
    const ULONGLONG now = GetTickCount64();
    if (needAnim && !m_lastNeedAnim) m_animDeadline = now + kPulseWindowMs;
    m_lastNeedAnim = needAnim;
    const bool pulsing = needAnim && now < m_animDeadline;

    UpdateAnimationTimer(pulsing);

    // Вторую плитку перерисовываем только когда пульсирует именно она:
    // иначе окно наушников мигало кадрами из-за заряжающейся мыши.
    const bool headsetPulsing = pulsing && podsLow;
    if (headsetPulsing != m_headsetAnimNeeded) {
        m_headsetAnimNeeded = headsetPulsing;
        m_headset.SetAnimation(headsetPulsing);
    }
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
        try {
            return pThis->HandleMessageSafe(hwnd, msg, wParam, lParam);
        } catch (...) {
            // C++-исключение (например, bad_alloc) не должно убивать Explorer.
            try {
                Diag::Log(L"ui: unhandled C++ exception in message 0x%04X", msg);
            } catch (...) {
            }
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT WidgetWindow::HandleMessageSafe(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    __try {
        return HandleMessage(hwnd, msg, wParam, lParam);
    }
    __except (LogStructuredException(GetExceptionCode())) {
        return 0;
    }
}

void CALLBACK WidgetWindow::StaticWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    WidgetWindow* self = s_instance;
    if (!self || idObject != OBJID_WINDOW) return;

    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        // Таскбар или его дочернее окно (TrayNotifyWnd) сдвинулись — пересчитываем позицию.
        if (self->m_hTaskbarWnd && (hwnd == self->m_hTaskbarWnd || IsChild(self->m_hTaskbarWnd, hwnd))) {
            self->ScheduleRelayout();
        }
        return;
    }

    if (event == EVENT_SYSTEM_FOREGROUND) {
        // Переключение фокуса — самый быстрый момент, чтобы остановить или вернуть опрос.
        const bool fullscreen = self->IsFullscreenAppActive();
        self->SetFullscreenState(fullscreen);
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
        // Снимок ячеек обновим в фоне, из отдельного потока.
        ForceRestore();
        m_headset.ForceRestore();
        m_appsEdge.RequestRefresh();
        return 0;
    }

    switch (msg) {
    case WM_APP_TELEMETRY: {
        RefreshAnimationState();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_APP_REPORT_DONE: {
        m_reportBusy = false;
        FinishDiagnosticsReport(hwnd);
        return 0;
    }

    case WM_APP_APPS_EDGE: {
        // Фоновый поток нашёл новый правый край кнопок приложений.
        if (!m_dragging && !m_snapping && !m_menuOpen && !m_headset.IsDragging()) {
            Relayout();
        }
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
            SetFullscreenState(fullscreen);
            const bool taskbarShowing = IsTaskbarShowing();
            if (fullscreen || !taskbarShowing) {
                if (IsWindowVisible(hwnd)) {
                    if (!fullscreen) Diag::Log(L"hide: taskbar is not showing");
                    ShowWindow(hwnd, SW_HIDE);
                    m_lastAppliedValid = false;
                    m_hoverTracking = false;
                    m_hoverProgress = 0.0f;
                    m_hoverTarget = 0.0f;
                }
                m_headset.Relayout({}, 0, 0, false);
            } else if (!IsWindowVisible(hwnd)) {
                Relayout();
            } else {
                // Оболочка может переставить свои окна выше виджета (например,
                // когда показывает превью задачи). Панель задач непрозрачная, и
                // плитка «пропадает», хотя IsWindowVisible остаётся true.
                // Возвращаем topmost-порядок; окно при этом не активируется.
                ForceRestore();
                m_headset.ForceRestore();

                // Пока курсор над панелью задач (там и появляются превью),
                // оболочка может переставлять окна чаще — держим порядок 10 раз
                // в секунду, иначе плитка успевает «утонуть» под панелью.
                POINT cursor{};
                RECT taskbarRect{};
                const bool overTaskbar =
                    GetCursorPos(&cursor) && GetWindowRect(m_hTaskbarWnd, &taskbarRect) &&
                    PtInRect(&taskbarRect, cursor);
                if (overTaskbar) {
                    SetTimer(hwnd, TIMER_ZORDER, 100, nullptr);
                } else {
                    KillTimer(hwnd, TIMER_ZORDER);
                }

                if (m_layoutDirty) {
                    // Склеенные события оболочки: догоняем раскладку, когда поток
                    // событий утих, а не на каждом кадре анимации панели.
                    const ULONGLONG now = GetTickCount64();
                    if (now - m_lastLayoutTick >= 100) {
                        m_layoutDirty = false;
                        m_lastLayoutTick = now;
                        Relayout();
                    }
                }
            }
            UpdateHeadsetState();
            RefreshAnimationState();
        } else if (wParam == TIMER_HOVER) {
            TickHover();
        } else if (wParam == TIMER_ZORDER) {
            // Курсор над панелью задач: оболочка любит переставлять свои окна.
            if (IsWindowVisible(hwnd)) {
                ForceRestore();
                m_headset.ForceRestore();
            }
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
            // Если плитку скрыли вручную, скан не включаем: скрытие снимается
            // только реальным отключением устройства (см. UpdateHeadsetState).
            if (!m_headsetUserHidden) {
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

    case WM_CLOSE:
        if (m_menuOpen) {
            // Нельзя уничтожать окно изнутри вложенного цикла меню: сначала
            // закрываем меню, а WM_CLOSE повторится в NotifyMenuClosed().
            CloseActiveMenu();
            m_exitRequested = true;
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

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
    SetMenuOpen(true);
    if (m_hoverTracking) {
        TRACKMOUSEEVENT cancel{ sizeof(TRACKMOUSEEVENT), TME_CANCEL, hwnd, 0 };
        TrackMouseEvent(&cancel);
        m_hoverTracking = false;
    }
    SetHoverTarget(0.0f);

    const UINT chosen = ShowWin11Menu(hInst, hwnd, IsMenuDark(), rc, items);

    NotifyMenuClosed();
    POINT cursor{};
    if (GetCursorPos(&cursor) && GetWindowRect(hwnd, &rc) && PtInRect(&rc, cursor)) {
        TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
        if (TrackMouseEvent(&tme)) m_hoverTracking = true;
        SetHoverTarget(1.0f);
    }

    // Выбор пункта прогоняем через обычный WM_COMMAND — обработчики те же, что были у меню.
    if (chosen != 0) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(chosen, 0), 0);
}

void WidgetWindow::NotifyMenuClosed() {
    SetMenuOpen(false);
    // Пока меню было открыто, раскладка могла измениться (закрылось окно,
    // пришли данные наушников) — догоняем её сейчас.
    if (m_hwnd && IsWindow(m_hwnd)) Relayout();
    if (m_exitRequested && m_hwnd && IsWindow(m_hwnd)) {
        m_exitRequested = false;
        // WM_CLOSE пришёл, пока меню было открыто; теперь окно можно закрывать.
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }
}

void WidgetWindow::ShowDiagnosticsReport(HWND hwnd) {
    if (!hwnd) return;
    if (m_reportBusy.exchange(true)) {
        // Отчёт уже собирается — второй параллельный только нагрузит HID.
        Diag::Log(L"diagnostics report: already running");
        return;
    }

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

    // HID-дамп и PnP-опрос занимают секунды. В UI-потоке Explorer это означало
    // бы зависшую панель задач, поэтому отчёт пишется в фоне.
    if (m_reportThread.joinable()) m_reportThread.join();
    m_reportDone = false;
    m_reportPath.clear();
    try {
        m_reportThread = std::thread([this, snapshot, hwnd] {
            const std::wstring path = Report::Write(hwnd, snapshot);
            {
                m_reportPath = path;
                m_reportDone.store(true, std::memory_order_release);
            }
            PostMessageW(hwnd, WM_APP_REPORT_DONE, 0, 0);
        });
    } catch (...) {
        m_reportBusy = false;
        MessageBoxW(nullptr, L10n::Get(Str::ReportFailed), L10n::Get(Str::ReportTitle),
                    MB_ICONERROR | MB_OK);
    }
}

void WidgetWindow::FinishDiagnosticsReport(HWND hwnd) {
    if (!m_reportDone.load(std::memory_order_acquire)) return;
    const std::wstring path = m_reportPath;
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

    // Всплывающие окна оболочки Windows 11 (превью задач, Task View, календарь,
    // быстрые настройки, переполнение трея) — это окна на весь монитор с
    // прозрачностью. Считать их полноэкранным приложением нельзя: иначе виджет
    // исчезает при наведении на кнопку панели задач.
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(hFore, &foregroundPid);
    if (foregroundPid != 0 && foregroundPid == GetCurrentProcessId()) return false;

    // Все всплывашки оболочки — tool-window: у них нет кнопки на панели задач.
    // Полноэкранные игры и видео обычными окнами и остаются.
    if ((GetWindowLongPtrW(hFore, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return false;

    if (_wcsnicmp(cls.data(), L"Xaml", 4) == 0) return false;
    static constexpr const wchar_t* kShellWindows[] = {
        L"TopLevelWindowForOverflowXamlIsland",
        L"MultitaskingViewFrame",
        L"ForegroundStaging",
        L"TaskListThumbnailWnd",
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
    };
    for (const wchar_t* name : kShellWindows) {
        if (wcscmp(cls.data(), name) == 0) return false;
    }

    RECT rcApp; GetWindowRect(hFore, &rcApp);
    // Сверяем с монитором, на котором стоит наша панель задач. Полноэкранная
    // игра на втором мониторе не должна прятать виджет на первом: Windows сам
    // прячет панель только того монитора, где открылось приложение.
    HWND anchor = (m_hTaskbarWnd && IsWindow(m_hTaskbarWnd)) ? m_hTaskbarWnd : hFore;
    HMONITOR hMon = MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMon, &mi)) {
        const bool coversMonitor =
            rcApp.left <= mi.rcMonitor.left && rcApp.top <= mi.rcMonitor.top &&
            rcApp.right >= mi.rcMonitor.right && rcApp.bottom >= mi.rcMonitor.bottom;
        if (!coversMonitor) return false;
        // Окно на весь монитор, но принадлежит оболочке (меню «Пуск», поиск) —
        // это не игра: виджет и опрос оставляем включёнными.
        return !IsShellHostProcess(foregroundPid);
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
