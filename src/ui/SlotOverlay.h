#pragma once
#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <string>
#include <vector>

// Подсветка ячеек, в которые «примагничивается» виджет: отдельное click-through
// окно с per-pixel alpha. Живёт только пока идёт перетаскивание, в простое не
// существует — поэтому на производительность не влияет.
class SlotOverlay {
public:
    SlotOverlay() = default;
    ~SlotOverlay();

    SlotOverlay(const SlotOverlay&) = delete;
    SlotOverlay& operator=(const SlotOverlay&) = delete;

    bool Create(HINSTANCE hInst);
    void Destroy();

    /// @param slots — прямоугольники ячеек в экранных координатах (px).
    /// @param active — индекс подсвечиваемой ячейки (-1 — ни одной).
    void Show(const std::vector<RECT>& slots, int active, bool dark, UINT dpi);
    void SetActive(int active, bool dark);
    void Hide();

    [[nodiscard]] bool IsVisible() const noexcept {
        return m_hwnd != nullptr && IsWindowVisible(m_hwnd) != FALSE;
    }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool EnsureWindow(int width, int height);
    bool Render();
    void Present();
    void StartFade(BYTE target, UINT durationMs);
    void TickFade();

    HINSTANCE m_hInst{ nullptr };
    std::wstring m_className;
    HWND m_hwnd{ nullptr };

    bool m_dark{ true };
    float m_scale{ 1.0f };
    std::vector<RECT> m_slots;  // в клиентских координатах окна
    int m_active{ -1 };

    BYTE m_alpha{ 0 };
    BYTE m_fadeFrom{ 0 };
    BYTE m_fadeTo{ 0 };
    ULONGLONG m_fadeStart{ 0 };
    UINT m_fadeDuration{ 0 };
    bool m_fading{ false };

    Microsoft::WRL::ComPtr<ID2D1Factory> m_factory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_target;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;

    HDC m_memDC{ nullptr };
    HBITMAP m_dib{ nullptr };
    HGDIOBJ m_oldBitmap{ nullptr };
    int m_width{ 0 };
    int m_height{ 0 };
};
