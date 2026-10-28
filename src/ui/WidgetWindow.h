#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include "core/AppState.h"
#include "ui/HeadsetTile.h"
#include "ui/Renderer.h"
#include "ui/SlotOverlay.h"

class WidgetWindow {
public:
    explicit WidgetWindow(HMODULE hModule);
    bool Create();
    void Destroy();
    void Relayout();
    void ForceRestore() const;
    void NotifyTelemetryChanged() const noexcept;

    // --- API для второй плитки (HeadsetTile) ---
    [[nodiscard]] bool CanStartDrag() const noexcept;
    [[nodiscard]] bool ComputeSlotsForDrag(std::vector<RECT>& slots, int& stepDip) const;
    [[nodiscard]] int MouseSlot() const noexcept { return m_currentMouseSlot; }
    [[nodiscard]] int StepDip() const noexcept { return m_currentStepDip; }
    [[nodiscard]] bool MenuDark() const noexcept { return IsMenuDark(); }
    [[nodiscard]] bool SystemDark() const noexcept { return m_isDarkTheme; }
    [[nodiscard]] WidgetTheme CurrentTheme() const noexcept { return m_theme; }
    void ApplyThemeFromMenu(WidgetTheme theme) { ApplyTheme(theme, true); }
    void ShowSlotOverlay(const std::vector<RECT>& slots, int active);
    void UpdateSlotOverlay(int active);
    void HideSlotOverlay();
    /// Поменять плитки местами: мышь в mouseSlot, наушники в headsetSlot.
    void SwapTiles(int mouseSlot, int headsetSlot);
    /// «Скрыть виджет» из меню наушников: убрать плитку и остановить опрос.
    void HideHeadsetTile();
    /// Предпросмотр обмена при перетаскивании наушников: мышь временно
    /// переезжает в освободившуюся ячейку, не меняя сохранённую позицию.
    void PreviewMouseToSlot(int slot);
    void EndMousePreview();
    /// Синхронизировать сетку ячеек перед предпросмотром со стороны наушников.
    void UpdateSlotCache(const std::vector<RECT>& slots, int stepDip) {
        m_lastSlots = slots;
        m_currentStepDip = stepDip > 0 ? stepDip : 176;
    }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK StaticWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD);
    static int LogStructuredException(DWORD code) noexcept;

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool RegisterWindowClass();
    void UnregisterWindowClass();
    void RefreshAnimationState();
    void UpdateAnimationTimer(bool needAnim);
    bool IsFullscreenAppActive() const noexcept;
    bool IsTaskbarShowing() const noexcept;
    static bool QuerySystemIsDark() noexcept;
    static bool IsAutoRunEnabled();
    static void ToggleAutoRun(HMODULE hInst);
    static void SyncAutoRunCommand(HMODULE hInst);
    static std::wstring BuildAutoRunCommand(HMODULE hInst);
    static WidgetTheme LoadTheme();
    static void SaveTheme(WidgetTheme theme);
    void ApplyTheme(WidgetTheme theme, bool persist);
    bool IsMenuDark() const noexcept;
    static void SignalUserExit() noexcept;
    void ShowContextMenu(HWND hwnd, HMODULE hInst);
    void ShowDiagnosticsReport(HWND hwnd);
    void SetHoverTarget(float target);
    void TickHover();

    // Вторая плитка (наушники): видимость, скрытие и возобновление по событию
    // подключения Bluetooth-устройства.
    [[nodiscard]] bool ShouldShowHeadsetTile() const;
    void UpdateHeadsetState();
    void ResumeHeadsetProbe();

    // Перетаскивание по панели задач: ячейки считаются по свободной полосе
    // между кнопками приложений и треем, поэтому виджет никогда не ложится
    // на чужие кнопки.
    bool ComputeSlots(std::vector<RECT>& slots, int& stepDip, bool forceAppsRefresh = false) const;
    int StoredSlotIndex(int slotCount, int stepDip) const;
    void StartDrag(HWND hwnd);
    void UpdateDrag(HWND hwnd);
    void FinishDrag(HWND hwnd, bool allowSnap);
    void CancelDrag();
    void StartSnap(HWND hwnd, int slotIndex);
    void TickSnap(HWND hwnd);
    int NearestSlot(int centerX) const;
    static int LoadPosOffset();
    static void SavePosOffset(int dip);

    HMODULE m_hInst;
    std::wstring m_className;  // уникальное имя класса для этой загрузки DLL
    HWND m_hwnd{ nullptr };
    HWND m_hTaskbarWnd{ nullptr };
    HWINEVENTHOOK m_hEventHookForeground{ nullptr };
    HWINEVENTHOOK m_hEventHookLocation{ nullptr };
    HDEVNOTIFY m_hDevNotify{ nullptr };
    UINT m_wmShellHook{ 0 };

    Renderer m_renderer;
    HeadsetTile m_headset{ *this };
    WidgetTheme m_theme{ WidgetTheme::Glass };  // выбранная тема виджета
    bool m_isDarkTheme{ true };                 // системная тема (для Glass и меню)
    bool m_animActive{ false };
    float m_animPhase{ 0.0f };
    bool m_hoverTracking{ false };
    bool m_hoverTimerActive{ false };
    bool m_menuOpen{ false };
    float m_hoverProgress{ 0.0f };
    float m_hoverTarget{ 0.0f };
    ULONGLONG m_hoverLastTick{ 0 };

    int m_currentMouseSlot{ 0 };
    int m_currentStepDip{ 176 };
    std::vector<RECT> m_lastSlots;          // свободные ячейки на момент последнего Relayout
    bool m_mousePreviewActive{ false };     // мышь временно показана в чужой ячейке

    bool m_headsetUserHidden{ false };      // «Скрыть виджет» до следующего подключения
    bool m_headsetWanted{ false };          // данные есть; места на панели может не хватить
    bool m_headsetProbeActive{ false };     // после события устройства проверяем, вернулись ли наушники
    ULONGLONG m_headsetProbeDeadline{ 0 };
    std::uint64_t m_headsetHiddenSinceUnixMs{ 0 };  // данные старше этого момента не считаем свежими
    AirPodsTelemetry m_lastAirPods;         // для точечной перерисовки без мигания

    // Перетаскивание и «магнит» к ячейкам.
    bool m_dragging{ false };
    bool m_dragMoved{ false };
    POINT m_dragStartCursor{};
    RECT m_dragStartRect{};
    std::vector<RECT> m_dragSlots;
    int m_dragStepDip{ 176 };
    int m_dragActive{ -1 };
    int m_dragHeadsetSlot{ -1 };
    bool m_previewHeadsetActive{ false };
    int m_posOffsetDip{ 0 };  // сохранённое смещение от ячейки у трея, в DIP
    bool m_snapping{ false };
    int m_snapIndex{ 0 };
    int m_snapStepDip{ 176 };
    int m_snapOldMouseSlot{ 0 };
    ULONGLONG m_snapStartTick{ 0 };
    RECT m_snapFrom{};
    RECT m_snapTo{};
    SlotOverlay m_slotOverlay;

    inline static WidgetWindow* s_instance{ nullptr };
};
