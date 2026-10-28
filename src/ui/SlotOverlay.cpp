#define NOMINMAX
#include "ui/SlotOverlay.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr UINT_PTR kTimerFade = 401;
constexpr UINT kFrameMs = 15;
constexpr UINT kFadeInMs = 90;
constexpr UINT kFadeOutMs = 120;
constexpr wchar_t kWindowTitle[] = L"BatteryInTaskBar.SlotOverlay";

// Геометрия в DIP; окно рисует в физических пикселях, поэтому всё умножается на scale.
constexpr float kCornerDip = 8.0f;
constexpr float kStrokeDip = 1.0f;
constexpr float kActiveStrokeDip = 1.6f;
constexpr float kGlowDip = 2.5f;
constexpr float kPadDip = 7.0f;  // место под свечение активной ячейки

BYTE LerpByte(BYTE from, BYTE to, float t) {
    const float value = from + (static_cast<float>(to) - static_cast<float>(from)) * t;
    return static_cast<BYTE>(std::clamp(value, 0.0f, 255.0f));
}

}  // namespace

SlotOverlay::~SlotOverlay() { Destroy(); }

bool SlotOverlay::Create(HINSTANCE hInst) {
    if (m_hwnd) return true;
    m_hInst = hInst;

    if (!m_factory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_factory.GetAddressOf()))) {
            return false;
        }
    }

    if (m_className.empty()) {
        // Уникальное имя класса: при перезагрузке DLL старый WndProc не должен остаться.
        m_className = L"BatteryInTaskBar.Slots." +
                      std::to_wstring(reinterpret_cast<std::uintptr_t>(this)) + L"." +
                      std::to_wstring(GetTickCount64());
    }

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, StaticWndProc, 0, 0, m_hInst,
                       nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr,
                       m_className.c_str(), nullptr };
    if (!RegisterClassExW(&wc)) return false;
    return true;
}

void SlotOverlay::Destroy() {
    if (m_hwnd) {
        KillTimer(m_hwnd, kTimerFade);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_memDC) {
        if (m_oldBitmap) {
            SelectObject(m_memDC, m_oldBitmap);
            m_oldBitmap = nullptr;
        }
        if (m_dib) {
            DeleteObject(m_dib);
            m_dib = nullptr;
        }
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    m_target.Reset();
    m_brush.Reset();
    m_factory.Reset();
    m_slots.clear();
    m_width = m_height = 0;

    if (!m_className.empty()) {
        UnregisterClassW(m_className.c_str(), m_hInst);
        m_className.clear();
    }
}

bool SlotOverlay::EnsureWindow(int width, int height) {
    if (!m_hwnd) {
        const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                              WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
        m_hwnd = CreateWindowExW(exStyle, m_className.c_str(), kWindowTitle, WS_POPUP,
                                 0, 0, width, height, nullptr, nullptr, m_hInst, this);
        if (!m_hwnd) return false;
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        // Подсветка ячеек должна быть над панелью задач и её XAML-островами.
        if (HMODULE hUser32 = GetModuleHandleW(L"user32.dll")) {
            using pfnSetWindowBand = BOOL(WINAPI*)(HWND, HWND, DWORD);
            if (auto pSetWindowBand = reinterpret_cast<pfnSetWindowBand>(GetProcAddress(hUser32, "SetWindowBand"))) {
                pSetWindowBand(m_hwnd, nullptr, 16);
            }
        }
    } else {
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    return true;
}

void SlotOverlay::Show(const std::vector<RECT>& slots, int active, bool dark, UINT dpi) {
    if (slots.empty() || !m_hInst) {
        Hide();
        return;
    }

    if (dpi == 0) dpi = 96;
    m_scale = static_cast<float>(dpi) / 96.0f;
    m_dark = dark;
    m_active = std::clamp(active, -1, static_cast<int>(slots.size()) - 1);

    RECT bounds = slots.front();
    for (const RECT& slot : slots) {
        bounds.left = std::min(bounds.left, slot.left);
        bounds.top = std::min(bounds.top, slot.top);
        bounds.right = std::max(bounds.right, slot.right);
        bounds.bottom = std::max(bounds.bottom, slot.bottom);
    }
    const int pad = static_cast<int>(std::lround(kPadDip * m_scale));
    const int width = (bounds.right - bounds.left) + pad * 2;
    const int height = (bounds.bottom - bounds.top) + pad * 2;
    if (width <= 0 || height <= 0) return;

    if (!EnsureWindow(width, height)) return;
    SetWindowPos(m_hwnd, HWND_TOPMOST, bounds.left - pad, bounds.top - pad, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    m_slots.clear();
    m_slots.reserve(slots.size());
    for (const RECT& slot : slots) {
        RECT local{ slot.left - (bounds.left - pad), slot.top - (bounds.top - pad),
                    slot.right - (bounds.left - pad), slot.bottom - (bounds.top - pad) };
        m_slots.push_back(local);
    }

    if (!Render()) return;

    // Если ячейки ещё гаснут, начинаем проявляться с текущей прозрачности.
    const bool continuing = IsWindowVisible(m_hwnd) != FALSE && m_fading && m_fadeTo == 0;
    m_alpha = continuing ? m_alpha : 0;
    Present();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    StartFade(255, kFadeInMs);
}

void SlotOverlay::SetActive(int active, bool dark) {
    if (!m_hwnd) return;
    if (active == m_active && dark == m_dark) return;
    m_active = active;
    m_dark = dark;
    if (!Render()) return;
    Present();
}

void SlotOverlay::Hide() {
    if (!m_hwnd || !IsWindowVisible(m_hwnd)) return;
    m_fadeFrom = m_alpha;
    StartFade(0, kFadeOutMs);
}

void SlotOverlay::StartFade(BYTE target, UINT durationMs) {
    if (!m_hwnd) return;
    m_fadeFrom = m_alpha;
    m_fadeTo = target;
    m_fadeStart = GetTickCount64();
    m_fadeDuration = durationMs;
    m_fading = true;
    SetTimer(m_hwnd, kTimerFade, kFrameMs, nullptr);
}

void SlotOverlay::TickFade() {
    if (!m_fading) return;
    const ULONGLONG elapsed = GetTickCount64() - m_fadeStart;
    const float t = m_fadeDuration == 0 ? 1.0f
                                        : std::clamp(static_cast<float>(elapsed) /
                                                         static_cast<float>(m_fadeDuration),
                                                     0.0f, 1.0f);
    // Плавное затухание: ease-out, как у меню.
    const float eased = 1.0f - (1.0f - t) * (1.0f - t);
    m_alpha = LerpByte(m_fadeFrom, m_fadeTo, eased);
    Present();

    if (t >= 1.0f) {
        m_fading = false;
        KillTimer(m_hwnd, kTimerFade);
        if (m_fadeTo == 0) ShowWindow(m_hwnd, SW_HIDE);
    }
}

bool SlotOverlay::Render() {
    if (!m_hwnd || m_slots.empty()) return false;
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return false;

    if (m_width != width || m_height != height || !m_memDC) {
        if (m_memDC) {
            if (m_oldBitmap) SelectObject(m_memDC, m_oldBitmap);
            if (m_dib) DeleteObject(m_dib);
            DeleteDC(m_memDC);
        }
        m_dib = nullptr;
        m_oldBitmap = nullptr;
        HDC screen = GetDC(nullptr);
        m_memDC = CreateCompatibleDC(screen);
        if (screen) ReleaseDC(nullptr, screen);
        if (!m_memDC) return false;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        m_dib = CreateDIBSection(m_memDC, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
        if (!m_dib) return false;
        m_oldBitmap = SelectObject(m_memDC, m_dib);
        m_width = width;
        m_height = height;
    }

    if (!m_target) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(m_factory->CreateDCRenderTarget(&props, m_target.GetAddressOf()))) return false;
        m_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(m_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f),
                                                   m_brush.GetAddressOf()))) {
            return false;
        }
    }

    RECT bindRect{ 0, 0, width, height };
    if (FAILED(m_target->BindDC(m_memDC, &bindRect))) return false;

    m_target->BeginDraw();
    m_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float scale = m_scale;
    const float radius = kCornerDip * scale;

    const D2D1_COLOR_F border = m_dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f)
                                       : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f);
    const D2D1_COLOR_F fill = m_dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f)
                                     : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.035f);
    const D2D1_COLOR_F accent = m_dark ? D2D1::ColorF(0.30f, 0.76f, 1.0f)
                                       : D2D1::ColorF(0.0f, 0.42f, 0.78f);

    // Сначала все пустые ячейки, поверх — активная: так свечение не перекрывается.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < m_slots.size(); ++i) {
            const bool isActive = static_cast<int>(i) == m_active;
            if ((pass == 1) != isActive) continue;

            const RECT& r = m_slots[i];
            const float inset = kStrokeDip * scale * 0.5f;
            const float activeInset = kActiveStrokeDip * scale * 0.5f;
            const float useInset = isActive ? activeInset : inset;
            const D2D1_ROUNDED_RECT rect = D2D1::RoundedRect(
                D2D1::RectF(static_cast<float>(r.left) + useInset,
                            static_cast<float>(r.top) + useInset,
                            static_cast<float>(r.right) - useInset,
                            static_cast<float>(r.bottom) - useInset),
                radius, radius);

            if (isActive) {
                // Мягкое свечение вокруг активной ячейки.
                m_brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.10f));
                const float glow = kGlowDip * scale;
                const D2D1_ROUNDED_RECT glowRect = D2D1::RoundedRect(
                    D2D1::RectF(rect.rect.left - glow, rect.rect.top - glow,
                                rect.rect.right + glow, rect.rect.bottom + glow),
                    radius + glow, radius + glow);
                m_target->FillRoundedRectangle(&glowRect, m_brush.Get());

                m_brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.14f));
                m_target->FillRoundedRectangle(&rect, m_brush.Get());

                m_brush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.85f));
                m_target->DrawRoundedRectangle(&rect, m_brush.Get(), kActiveStrokeDip * scale);
            } else {
                m_brush->SetColor(fill);
                m_target->FillRoundedRectangle(&rect, m_brush.Get());
                m_brush->SetColor(border);
                m_target->DrawRoundedRectangle(&rect, m_brush.Get(), kStrokeDip * scale);
            }
        }
    }

    const HRESULT hr = m_target->EndDraw();
    return SUCCEEDED(hr);
}

void SlotOverlay::Present() {
    if (!m_hwnd || !m_memDC) return;
    POINT src{ 0, 0 };
    SIZE size{ m_width, m_height };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, m_alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(m_hwnd, nullptr, nullptr, &size, m_memDC, &src, 0, &blend, ULW_ALPHA);
}

LRESULT CALLBACK SlotOverlay::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* self = reinterpret_cast<SlotOverlay*>(
            reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<SlotOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SlotOverlay::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == kTimerFade) {
            TickFade();
            return 0;
        }
        break;
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // клики и наведение проходят сквозь подсветку
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
