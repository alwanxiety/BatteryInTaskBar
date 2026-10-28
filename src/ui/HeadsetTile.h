#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "core/AppState.h"
#include "ui/Renderer.h"

class WidgetWindow;

// Вторая плитка — наушники. Отдельное окно, а не половина общего:
// плитки двигаются независимо, и одна не должна перекрывать клики по
// панели задач на месте другой.
class HeadsetTile {
public:
    explicit HeadsetTile(WidgetWindow& owner);
    ~HeadsetTile();

    HeadsetTile(const HeadsetTile&) = delete;
    HeadsetTile& operator=(const HeadsetTile&) = delete;

    bool Create(HINSTANCE hInst);
    void Destroy();

    /// @param slots — свободные ячейки панели в экранных координатах (0 — у трея).
    /// @param mouseSlot — ячейка мыши, на неё нельзя встать без обмена местами.
    void Relayout(const std::vector<RECT>& slots, int stepDip, int mouseSlot, bool shouldShow);

    void Invalidate();
    void DiscardDeviceResources();
    void TickAnimation(float phase);
    void CancelDrag();
    void ShowContextMenu();
    /// Спрятать без анимации (полноэкранный режим, вертикальная панель).
    void HideImmediately();
    /// Вернуть окно наверх, не трогая позицию и не пересчитывая ячейки.
    void ForceRestore() const;
    /// Временно показать плитку в другой ячейке (живой предпросмотр обмена).
    void PreviewToSlot(int slot);
    /// Обновить сетку ячеек перед предпросмотром со стороны мыши.
    void UpdateSlots(const std::vector<RECT>& slots) { m_dragSlots = slots; }

    [[nodiscard]] bool IsVisible() const;
    [[nodiscard]] bool IsDragging() const noexcept { return m_dragging || m_snapping; }
    [[nodiscard]] HWND Hwnd() const noexcept { return m_hwnd; }
    [[nodiscard]] RECT Rect() const;
    [[nodiscard]] int Slot() const noexcept { return m_slot; }
    [[nodiscard]] int OffsetDip() const noexcept { return m_offsetDip; }

    void SetOffsetDip(int dip, bool persist);

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass();
    void UnregisterWindowClass();
    int SlotFromOffset(int slotCount) const;
    void ApplySlot(int slot);

    // Анимация появления/скрытия: плитка выезжает из-под соседней и проявляется.
    void StartShowAnimation(int slot);
    void StartHideAnimation();
    void TickTransition();
    void ApplyAlpha(BYTE alpha);
    void CancelTransition();

    void StartDrag();
    void UpdateDrag();
    void FinishDrag();
    void StartSnap(int targetSlot, bool swap);
    void TickSnap();

    void SetHover(float target);
    void TickHover();
    int NearestSlot(int centerX) const;

    static int LoadOffset();
    static void SaveOffset(int dip);

    WidgetWindow& m_owner;
    HINSTANCE m_hInst{ nullptr };
    std::wstring m_className;
    HWND m_hwnd{ nullptr };
    Renderer m_renderer;

    int m_slot{ -1 };
    int m_offsetDip{ -1 };  // -1 — позиция ещё не задана
    int m_stepDip{ 176 };
    int m_mouseSlot{ -1 };

    std::vector<RECT> m_dragSlots;
    bool m_dragging{ false };
    bool m_dragMoved{ false };
    POINT m_dragStartCursor{};
    RECT m_dragStartRect{};
    int m_dragStartSlot{ -1 };
    int m_dragActive{ -1 };
    int m_mouseSlotOriginal{ -1 };
    bool m_previewMouseActive{ false };

    bool m_snapping{ false };
    int m_snapTarget{ -1 };
    bool m_snapSwap{ false };
    ULONGLONG m_snapStartTick{ 0 };
    RECT m_snapFrom{};
    RECT m_snapTo{};

    float m_hoverProgress{ 0.0f };
    float m_hoverTarget{ 0.0f };
    bool m_hoverTracking{ false };
    bool m_hoverTimerActive{ false };
    ULONGLONG m_hoverLastTick{ 0 };
    float m_animPhase{ 0.0f };

    bool m_transitionActive{ false };
    bool m_transitionHide{ false };
    ULONGLONG m_transitionStart{ 0 };
    int m_transitionFromX{ 0 };
    int m_transitionToX{ 0 };
    int m_transitionTop{ 0 };
    int m_transitionWidth{ 0 };
    int m_transitionHeight{ 0 };
    BYTE m_alpha{ 255 };
    BYTE m_transitionFromAlpha{ 0 };
    BYTE m_transitionToAlpha{ 255 };
};
