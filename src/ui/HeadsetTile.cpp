#define NOMINMAX
#include "ui/HeadsetTile.h"
#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include "core/Localization.h"
#include "core/Diagnostics.h"
#include "ui/ContextMenu.h"
#include "ui/MenuIds.h"
#include "ui/WidgetWindow.h"

namespace {
constexpr wchar_t kHeadsetTitle[] = L"BatteryInTaskBar.Headset";
constexpr UINT_PTR TIMER_HOVER = 501;
constexpr UINT_PTR TIMER_SNAP = 502;
constexpr UINT_PTR TIMER_TRANSITION = 503;
constexpr int kDragThresholdPx = 3;
constexpr int kSnapDurationMs = 140;
// Появление/скрытие: плитка проявляется и выезжает из-под соседней на 12 px.
constexpr int kTransitionMs = 220;
constexpr int kSlideOffset = 14;

enum ZBID : DWORD {
    ZBID_IMMERSIVE_NOTIFICATION = 4
};

using pfnSetWindowBand = BOOL(WINAPI*)(HWND hWnd, HWND hwndInsertAfter, DWORD dwBand);

std::wstring LevelText(int level) {
    return level < 0 ? std::wstring(L"—") : std::to_wstring(level) + L"%";
}
}  // namespace

HeadsetTile::HeadsetTile(WidgetWindow& owner) : m_owner(owner) {}

HeadsetTile::~HeadsetTile() { Destroy(); }

bool HeadsetTile::Create(HINSTANCE hInst) {
    if (m_hwnd) return true;
    m_hInst = hInst;
    m_offsetDip = LoadOffset();

    if (!m_renderer.Initialize()) return false;
    if (!RegisterWindowClass()) return false;

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        m_className.c_str(), kHeadsetTitle, WS_POPUP,
        0, 0, 168, 38, nullptr, nullptr, m_hInst, this);
    if (!m_hwnd) {
        UnregisterWindowClass();
        return false;
    }
    // Прозрачность для анимации появления/скрытия. DWM композитит окно целиком.
    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);

    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    if (HMODULE hUser32 = GetModuleHandleW(L"user32.dll")) {
        if (auto pSetWindowBand = reinterpret_cast<pfnSetWindowBand>(GetProcAddress(hUser32, "SetWindowBand"))) {
            pSetWindowBand(m_hwnd, nullptr, ZBID_IMMERSIVE_NOTIFICATION);
        }
    }
    return true;
}

void HeadsetTile::Destroy() {
    CancelDrag();
    CancelTransition();
    if (m_hwnd) {
        KillTimer(m_hwnd, TIMER_HOVER);
        KillTimer(m_hwnd, TIMER_SNAP);
        KillTimer(m_hwnd, TIMER_TRANSITION);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_renderer.DiscardDeviceResources();
    UnregisterWindowClass();
}

bool HeadsetTile::RegisterWindowClass() {
    // Своё уникальное имя класса на каждую загрузку DLL — как у основной плитки.
    const std::wstring base = L"BatteryInTaskBar.Headset." +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(m_hInst)) + L"." +
        std::to_wstring(GetTickCount64());

    for (int attempt = 0; attempt < 8; ++attempt) {
        m_className = attempt == 0 ? base : base + L"." + std::to_wstring(attempt);
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, StaticWndProc, 0, 0,
                           m_hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr,
                           m_className.c_str(), nullptr };
        if (RegisterClassExW(&wc)) return true;
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
    }
    m_className.clear();
    return false;
}

void HeadsetTile::UnregisterWindowClass() {
    if (m_className.empty()) return;
    if (!UnregisterClassW(m_className.c_str(), m_hInst)) {
        Diag::Log(L"headset: UnregisterClass failed, error=%lu", GetLastError());
    }
    m_className.clear();
}

int HeadsetTile::SlotFromOffset(int slotCount) const {
    if (slotCount <= 0) return -1;
    if (m_offsetDip < 0) {
        // Позиция ещё не задана — встаём рядом с мышью, но не на неё.
        if (m_mouseSlot + 1 < slotCount) return m_mouseSlot + 1;
        if (m_mouseSlot - 1 >= 0) return m_mouseSlot - 1;
        return -1;
    }
    const int step = m_stepDip > 0 ? m_stepDip : 176;
    const long long index = std::llround(static_cast<double>(m_offsetDip) / step);
    return std::clamp(static_cast<int>(index), 0, slotCount - 1);
}

void HeadsetTile::ApplySlot(int slot) {
    if (!m_hwnd || slot < 0 || slot >= static_cast<int>(m_dragSlots.size())) return;
    const RECT& cell = m_dragSlots[slot];
    m_slot = slot;
    SetWindowPos(m_hwnd, HWND_TOPMOST, cell.left, cell.top,
                 cell.right - cell.left, cell.bottom - cell.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

void HeadsetTile::Relayout(const std::vector<RECT>& slots, int stepDip, int mouseSlot, bool shouldShow) {
    if (!m_hwnd) return;
    if (m_dragging || m_snapping) return;

    m_dragSlots = slots;
    m_stepDip = stepDip > 0 ? stepDip : 176;
    m_mouseSlot = mouseSlot;

    // Пустой список — полноэкранный режим или вертикальная панель: прячем сразу,
    // без анимации, чтобы не мелькать поверх игры.
    if (slots.empty()) {
        HideImmediately();
        return;
    }

    if (!shouldShow || slots.size() < 2) {
        if (m_transitionActive && m_transitionHide) return;  // уже прячется
        if (IsWindowVisible(m_hwnd)) StartHideAnimation();
        else m_slot = -1;
        return;
    }

    int target = SlotFromOffset(static_cast<int>(slots.size()));
    if (target == mouseSlot) {
        target = (mouseSlot + 1 < static_cast<int>(slots.size())) ? mouseSlot + 1 : mouseSlot - 1;
    }
    if (target < 0 || target >= static_cast<int>(slots.size())) {
        HideImmediately();
        return;
    }
    if (target != SlotFromOffset(static_cast<int>(slots.size())) || m_offsetDip < 0) {
        // Позицию сохраняем только при первом размещении. Временный сдвиг из-за
        // мыши в «любимой» ячейке не должен перезаписывать выбор пользователя.
        SetOffsetDip(target * m_stepDip, m_offsetDip < 0);
    }

    if (!IsWindowVisible(m_hwnd)) {
        StartShowAnimation(target);
    } else {
        // Уже на панели — просто ставим в ячейку (обмен, пересчёт сетки).
        CancelTransition();
        ApplyAlpha(255);
        ApplySlot(target);
    }
}

void HeadsetTile::HideImmediately() {
    CancelTransition();
    m_slot = -1;
    if (m_hwnd && IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
    if (m_hwnd) ApplyAlpha(0);
}

void HeadsetTile::ApplyAlpha(BYTE alpha) {
    if (!m_hwnd) return;
    m_alpha = alpha;
    SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
}

void HeadsetTile::CancelTransition() {
    if (!m_transitionActive) return;
    m_transitionActive = false;
    if (m_hwnd) KillTimer(m_hwnd, TIMER_TRANSITION);
}

void HeadsetTile::StartShowAnimation(int slot) {
    if (!m_hwnd || slot < 0 || slot >= static_cast<int>(m_dragSlots.size())) return;
    const RECT& cell = m_dragSlots[slot];
    m_slot = slot;
    m_transitionHide = false;
    m_transitionFromX = cell.left + kSlideOffset;
    m_transitionToX = cell.left;
    m_transitionTop = cell.top;
    m_transitionWidth = cell.right - cell.left;
    m_transitionHeight = cell.bottom - cell.top;
    m_transitionFromAlpha = 0;
    m_transitionToAlpha = 255;
    m_transitionStart = GetTickCount64();
    m_transitionActive = true;

    ApplyAlpha(0);
    SetWindowPos(m_hwnd, HWND_TOPMOST, m_transitionFromX, m_transitionTop,
                 m_transitionWidth, m_transitionHeight,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    SetTimer(m_hwnd, TIMER_TRANSITION, 15, nullptr);
}

void HeadsetTile::StartHideAnimation() {
    if (!m_hwnd) return;
    RECT rc{};
    if (!GetWindowRect(m_hwnd, &rc)) {
        HideImmediately();
        return;
    }
    m_transitionHide = true;
    m_transitionFromX = rc.left;
    m_transitionToX = rc.left + kSlideOffset;
    m_transitionTop = rc.top;
    m_transitionWidth = rc.right - rc.left;
    m_transitionHeight = rc.bottom - rc.top;
    m_transitionFromAlpha = m_alpha;
    m_transitionToAlpha = 0;
    m_transitionStart = GetTickCount64();
    m_transitionActive = true;
    // Ячейку освобождаем сразу: плитка уже уходит, и обмен с мышью не должен
    // считать её занятой.
    m_slot = -1;
    SetTimer(m_hwnd, TIMER_TRANSITION, 15, nullptr);
}

void HeadsetTile::TickTransition() {
    if (!m_transitionActive) {
        if (m_hwnd) KillTimer(m_hwnd, TIMER_TRANSITION);
        return;
    }

    const ULONGLONG elapsed = GetTickCount64() - m_transitionStart;
    float t = static_cast<float>(elapsed) / static_cast<float>(kTransitionMs);
    t = std::clamp(t, 0.0f, 1.0f);
    // Ease-out: быстро выезжает, мягко останавливается.
    const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

    const int x = m_transitionFromX +
                  static_cast<int>(std::lround((m_transitionToX - m_transitionFromX) * eased));
    // Прозрачность меняем линейно: при ease-out она «прыгает» к 90 % за первые
    // 100 мс, и плавность не читается.
    const int alpha = m_transitionFromAlpha +
                      static_cast<int>(std::lround((m_transitionToAlpha - m_transitionFromAlpha) * t));
    ApplyAlpha(static_cast<BYTE>(std::clamp(alpha, 0, 255)));
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, m_transitionTop, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    if (t >= 1.0f) {
        KillTimer(m_hwnd, TIMER_TRANSITION);
        m_transitionActive = false;
        if (m_transitionHide) {
            ShowWindow(m_hwnd, SW_HIDE);
            m_slot = -1;
            ApplyAlpha(0);
        } else {
            ApplyAlpha(255);
        }
    }
}

void HeadsetTile::SetOffsetDip(int dip, bool persist) {
    m_offsetDip = std::max(-1, dip);
    if (persist) SaveOffset(m_offsetDip);
}

int HeadsetTile::LoadOffset() {
    HKEY hKey;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\BatteryInTaskBar", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return -1;
    }
    const LONG result = RegQueryValueExW(hKey, L"HeadsetPosOffset", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    if (result != ERROR_SUCCESS || type != REG_DWORD) return -1;
    if (value > 5000) return -1;
    return static_cast<int>(value);
}

void HeadsetTile::SaveOffset(int dip) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BatteryInTaskBar", 0, nullptr, 0, KEY_WRITE,
                        nullptr, &hKey, nullptr) != ERROR_SUCCESS) return;
    const DWORD value = static_cast<DWORD>(std::max(0, dip));
    RegSetValueExW(hKey, L"HeadsetPosOffset", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
}

void HeadsetTile::Invalidate() {
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HeadsetTile::DiscardDeviceResources() { m_renderer.DiscardDeviceResources(); }

void HeadsetTile::ForceRestore() const {
    if (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void HeadsetTile::TickAnimation(float phase) {
    m_animPhase = phase;
    // Перерисовываем только когда пульсирует сама плитка наушников: при
    // заряжающейся мыши окно наушников иначе мигало 30 раз в секунду зря.
    if (m_animNeeded && IsVisible()) InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool HeadsetTile::IsVisible() const {
    return m_hwnd && IsWindowVisible(m_hwnd) != FALSE;
}

RECT HeadsetTile::Rect() const {
    RECT rc{};
    if (m_hwnd) GetWindowRect(m_hwnd, &rc);
    return rc;
}

int HeadsetTile::NearestSlot(int centerX) const {
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

void HeadsetTile::PreviewToSlot(int slot) {
    if (!m_hwnd) return;
    if (slot < 0 || slot >= static_cast<int>(m_dragSlots.size())) return;
    CancelTransition();
    ApplyAlpha(255);
    const RECT& cell = m_dragSlots[slot];
    SetWindowPos(m_hwnd, HWND_TOPMOST, cell.left, cell.top,
                 cell.right - cell.left, cell.bottom - cell.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

void HeadsetTile::StartDrag() {
    if (m_dragging || m_snapping || m_slot < 0) return;
    if (!m_owner.CanStartDrag()) return;

    CancelTransition();
    ApplyAlpha(255);

    std::vector<RECT> slots;
    int stepDip = 0;
    if (!m_owner.ComputeSlotsForDrag(slots, stepDip) || slots.size() < 2) return;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    if (!GetWindowRect(m_hwnd, &m_dragStartRect)) return;

    m_dragSlots = std::move(slots);
    m_stepDip = stepDip > 0 ? stepDip : 176;
    m_owner.UpdateSlotCache(m_dragSlots, m_stepDip);
    m_mouseSlot = m_owner.MouseSlot();
    m_dragStartSlot = m_slot;
    m_mouseSlotOriginal = m_mouseSlot;
    m_previewMouseActive = false;
    m_dragStartCursor = cursor;
    m_dragMoved = false;
    m_dragging = true;
    m_dragActive = m_slot;

    SetCapture(m_hwnd);
    SetHover(0.0f);
    m_owner.ShowSlotOverlay(m_dragSlots, m_dragActive);
}

void HeadsetTile::UpdateDrag() {
    if (!m_dragging) return;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;

    const int dx = cursor.x - m_dragStartCursor.x;
    if (!m_dragMoved && std::abs(dx) < kDragThresholdPx) return;
    m_dragMoved = true;

    const int width = m_dragStartRect.right - m_dragStartRect.left;
    const int startLeft = static_cast<int>(m_dragStartRect.left);
    const int minLeft = m_dragSlots.back().left;
    const int maxLeft = m_dragSlots.front().left;
    const int left = std::clamp(startLeft + dx, minLeft, maxLeft);

    SetWindowPos(m_hwnd, HWND_TOPMOST, left, m_dragStartRect.top, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    const int active = NearestSlot(left + width / 2);
    if (active != m_dragActive) {
        m_dragActive = active;
        m_owner.UpdateSlotOverlay(active);
    }

    // Живой предпросмотр: наушники накрыли мышь — мышь сразу уезжает в
    // освободившуюся ячейку наушников. Отпустим — обмен, уведём — вернём.
    if (m_mouseSlotOriginal >= 0) {
        if (active == m_mouseSlotOriginal && !m_previewMouseActive) {
            m_previewMouseActive = true;
            m_owner.PreviewMouseToSlot(m_dragStartSlot);
        } else if (active != m_mouseSlotOriginal && m_previewMouseActive) {
            m_previewMouseActive = false;
            m_owner.EndMousePreview();
        }
    }
}

void HeadsetTile::FinishDrag() {
    if (!m_dragging) return;
    m_dragging = false;

    if (GetCapture() == m_hwnd) ReleaseCapture();
    m_owner.HideSlotOverlay();

    // Отпустили не над мышью — предпросмотр отменяем, мышь возвращается.
    if (m_previewMouseActive && m_dragActive != m_mouseSlotOriginal) {
        m_previewMouseActive = false;
        m_owner.EndMousePreview();
    }

    if (m_dragMoved) {
        const int target = m_dragActive;
        StartSnap(target, target == m_owner.MouseSlot());
    }
}

void HeadsetTile::StartSnap(int targetSlot, bool swap) {
    if (targetSlot < 0 || targetSlot >= static_cast<int>(m_dragSlots.size())) return;
    if (!GetWindowRect(m_hwnd, &m_snapFrom)) return;

    m_snapTo = m_dragSlots[targetSlot];
    m_snapTarget = targetSlot;
    m_snapSwap = swap;
    m_snapStartTick = GetTickCount64();
    m_snapping = true;
    SetTimer(m_hwnd, TIMER_SNAP, 15, nullptr);
}

void HeadsetTile::TickSnap() {
    if (!m_snapping) {
        KillTimer(m_hwnd, TIMER_SNAP);
        return;
    }

    const ULONGLONG elapsed = GetTickCount64() - m_snapStartTick;
    float t = static_cast<float>(elapsed) / static_cast<float>(kSnapDurationMs);
    t = std::clamp(t, 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

    const int x = m_snapFrom.left + static_cast<int>(std::lround((m_snapTo.left - m_snapFrom.left) * eased));
    const int y = m_snapFrom.top + static_cast<int>(std::lround((m_snapTo.top - m_snapFrom.top) * eased));
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    if (t >= 1.0f) {
        KillTimer(m_hwnd, TIMER_SNAP);
        m_snapping = false;
        if (m_snapSwap) {
            // Наушники бросили поверх мыши: меняемся ячейками. Мышь уже
            // показана на месте наушников — фиксируем это.
            m_previewMouseActive = false;
            m_owner.SwapTiles(m_dragStartSlot, m_snapTarget);
        } else {
            m_previewMouseActive = false;
            SetOffsetDip(m_snapTarget * m_stepDip, true);
            m_slot = m_snapTarget;
            m_owner.Relayout();
        }
    }
}

void HeadsetTile::CancelDrag() {
    if (!m_dragging && !m_snapping) return;
    m_dragging = false;
    m_snapping = false;
    m_dragMoved = false;
    if (m_previewMouseActive) {
        m_previewMouseActive = false;
        m_owner.EndMousePreview();
    }
    if (m_hwnd) {
        if (GetCapture() == m_hwnd) ReleaseCapture();
        KillTimer(m_hwnd, TIMER_SNAP);
    }
    m_owner.HideSlotOverlay();
}

void HeadsetTile::SetHover(float target) {
    if (!m_hwnd) return;
    m_hoverTarget = std::clamp(target, 0.0f, 1.0f);
    if (!m_hoverTimerActive) {
        m_hoverLastTick = GetTickCount64();
        SetTimer(m_hwnd, TIMER_HOVER, 16, nullptr);
        m_hoverTimerActive = true;
    }
}

void HeadsetTile::TickHover() {
    if (!m_hwnd) return;
    const ULONGLONG now = GetTickCount64();
    float dt = static_cast<float>(now - m_hoverLastTick) / 1000.0f;
    m_hoverLastTick = now;
    dt = std::clamp(dt, 0.0f, 0.1f);

    const float smoothing = 1.0f - std::exp(-dt / 0.07f);
    m_hoverProgress += (m_hoverTarget - m_hoverProgress) * smoothing;
    if (std::abs(m_hoverTarget - m_hoverProgress) < 0.004f) {
        m_hoverProgress = m_hoverTarget;
        KillTimer(m_hwnd, TIMER_HOVER);
        m_hoverTimerActive = false;
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HeadsetTile::ShowContextMenu() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;

    const AirPodsTelemetry pods = AppState::Instance().GetAirPods();
    RECT rc{};
    GetWindowRect(m_hwnd, &rc);

    std::vector<ContextMenuItem> items;

    ContextMenuItem title;
    title.kind = ContextMenuItem::Kind::Title;
    title.text = pods.deviceName.empty() ? L10n::Get(Str::HeadsetName) : pods.deviceName;
    items.push_back(std::move(title));

    ContextMenuItem status;
    status.kind = ContextMenuItem::Kind::Info;
    status.text = std::wstring(L10n::Get(Str::Battery)) +
                  L10n::Get(Str::AirPodsLeftShort) + L" " + LevelText(pods.leftLevel) + L" · " +
                  L10n::Get(Str::AirPodsRightShort) + L" " + LevelText(pods.rightLevel) + L" · " +
                  L10n::Get(Str::AirPodsCaseShort) + L" " + LevelText(pods.caseLevel);
    if (!pods.connected) {
        status.text += L" · " + std::wstring(L10n::Get(Str::SubDisconnected));
    } else {
        const std::int64_t age = AppState::Instance().SecondsSinceAirPodsSuccess();
        if (age >= 0) status.text += L" · " + L10n::Ago(age);
    }
    items.push_back(std::move(status));

    ContextMenuItem themeItem;
    themeItem.text = L10n::Get(Str::Theme);
    auto addTheme = [this, &themeItem](const wchar_t* text, UINT id, WidgetTheme theme) {
        ContextMenuItem entry;
        entry.text = text;
        entry.id = id;
        entry.checked = (m_owner.CurrentTheme() == theme);
        themeItem.submenu.push_back(std::move(entry));
    };
    addTheme(L10n::Get(Str::ThemeGlass), IDM_THEME_GLASS, WidgetTheme::Glass);
    addTheme(L10n::Get(Str::ThemeDark), IDM_THEME_DARK, WidgetTheme::Dark);
    addTheme(L10n::Get(Str::ThemeLight), IDM_THEME_LIGHT, WidgetTheme::Light);
    items.push_back(std::move(themeItem));

    ContextMenuItem refresh;
    refresh.text = L10n::Get(Str::RefreshNow);
    refresh.id = IDM_HEADSET_REFRESH;
    items.push_back(std::move(refresh));

    ContextMenuItem hide;
    hide.text = L10n::Get(Str::HideWidget);
    hide.id = IDM_HEADSET_HIDE;
    items.push_back(std::move(hide));

    SetHover(0.0f);
    // Пока меню открыто, владелец не должен двигать плитки: меню привязано
    // к позиции окна.
    m_owner.SetMenuOpen(true);
    const UINT chosen = ShowWin11Menu(m_hInst, m_hwnd, m_owner.MenuDark(), rc, items);
    m_owner.NotifyMenuClosed();

    POINT cursor{};
    if (GetCursorPos(&cursor) && GetWindowRect(m_hwnd, &rc) && PtInRect(&rc, cursor)) {
        TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hwnd, 0 };
        if (TrackMouseEvent(&tme)) m_hoverTracking = true;
        SetHover(1.0f);
    }

    if (chosen != 0) PostMessageW(m_hwnd, WM_COMMAND, MAKEWPARAM(chosen, 0), 0);
}

LRESULT CALLBACK HeadsetTile::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HeadsetTile* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<HeadsetTile*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HeadsetTile*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        try {
            return self->HandleMessageSafe(hwnd, msg, wParam, lParam);
        } catch (...) {
            // C++-исключение не должно уходить в user32 и ронять Explorer.
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT HeadsetTile::HandleMessageSafe(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    __try {
        return HandleMessage(hwnd, msg, wParam, lParam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

LRESULT HeadsetTile::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        m_renderer.RenderHeadset(hwnd, AppState::Instance().GetAirPods(), m_owner.CurrentTheme(),
                                 m_owner.SystemDark(), m_hoverProgress, m_animPhase);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_HOVER) TickHover();
        else if (wParam == TIMER_SNAP) TickSnap();
        else if (wParam == TIMER_TRANSITION) TickTransition();
        return 0;

    case WM_MOUSEMOVE:
        if (m_dragging) {
            UpdateDrag();
            return 0;
        }
        if (!m_hoverTracking) {
            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            if (TrackMouseEvent(&tme)) m_hoverTracking = true;
        }
        SetHover(1.0f);
        return 0;

    case WM_MOUSELEAVE:
        m_hoverTracking = false;
        SetHover(0.0f);
        return 0;

    case WM_LBUTTONDOWN:
        StartDrag();
        return 0;

    case WM_LBUTTONUP:
        FinishDrag();
        return 0;

    case WM_CAPTURECHANGED:
        if (m_dragging) FinishDrag();
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && m_dragging) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;

    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowContextMenu();
        return 0;

    case WM_DPICHANGED:
        m_renderer.DiscardDeviceResources();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_WINDOWPOSCHANGING:
        // Вторая плитка тоже должна оставаться над панелью задач.
        reinterpret_cast<WINDOWPOS*>(lParam)->hwndInsertAfter = HWND_TOPMOST;
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_HEADSET_REFRESH:
            AppState::Instance().RequestAirPodsScan();
            break;
        case IDM_HEADSET_HIDE:
            m_owner.HideHeadsetTile();
            break;
        case IDM_THEME_GLASS:
            m_owner.ApplyThemeFromMenu(WidgetTheme::Glass);
            break;
        case IDM_THEME_DARK:
            m_owner.ApplyThemeFromMenu(WidgetTheme::Dark);
            break;
        case IDM_THEME_LIGHT:
            m_owner.ApplyThemeFromMenu(WidgetTheme::Light);
            break;
        default:
            break;
        }
        return 0;

    case WM_DESTROY:
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
