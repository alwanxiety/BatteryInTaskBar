// min/max из windows.h ломают std::clamp/std::max — отключаем их до первого включения.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui/ContextMenu.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {


constexpr wchar_t kMenuWindowTitle[] = L"BatteryInTaskBar.ContextMenu";
constexpr wchar_t kSubmenuWindowTitle[] = L"BatteryInTaskBar.ContextSubmenu";
constexpr UINT_PTR kTimerFrame = 201;
// Период кадров анимации. WM_TIMER квантуется по ~15.6 мс (и это заметно рвано),
// поэтому кадры ведёт высокоточный waitable-таймер, а WM_TIMER остаётся запасным.
constexpr DWORD kFramePeriodMs = 7;

// Скрытая команда: столько кликов по названию устройства собирают отчёт.
constexpr int kReportClickCount = 5;

// Геометрия в DIP (умножается на DPI через render target).
constexpr float kShadowPad = 12.0f;
constexpr float kBodyPadX = 12.0f;
constexpr float kBodyPadTop = 4.0f;
constexpr float kBodyPadBottom = 4.0f;
constexpr float kTitleHeight = 32.0f;
constexpr float kInfoHeight = 22.0f;
constexpr float kActionHeight = 30.0f;
constexpr float kMinWidth = 196.0f;
constexpr float kMinWidthSubmenu = 140.0f;
constexpr float kMaxWidth = 320.0f;
constexpr float kCornerRadius = 8.0f;
constexpr float kRowRadius = 5.0f;
constexpr float kSubmenuGap = 2.0f;          // зазор между панелью и подменю
constexpr ULONGLONG kSubmenuOpenDelayMs = 150;  // задержка раскрытия при наведении

constexpr float kShowSeconds = 0.19f;
constexpr float kHideSeconds = 0.10f;
constexpr float kHoverTauSeconds = 0.07f;

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Мягкий «выскок»: чуть перелетает единицу и возвращается.
float EaseOutBack(float t) {
    constexpr float c1 = 0.9f;
    constexpr float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

D2D1_COLOR_F MakeColor(float r, float g, float b, float a = 1.0f) {
    return D2D1::ColorF(r, g, b, a);
}

}  // namespace

class Win11Menu {
public:
    Win11Menu(HINSTANCE hInst, HWND owner, bool darkTheme, RECT anchor,
              std::vector<ContextMenuItem> items, bool submenuStyle = false);
    ~Win11Menu();

    UINT Run();

    // Подменю живёт внутри родительского меню: ввод приходит от родителя,
    // поэтому сама панель фокуса и захвата мыши не берёт.
    bool IsDone() const noexcept { return m_done; }
    bool IsClosing() const noexcept { return m_closing; }
    UINT Result() const noexcept { return m_result; }
    bool ContainsScreenPoint(POINT screenPt) const noexcept;
    void ExternalMouseMove(POINT screenPt);
    bool ExternalClick(POINT screenPt);
    void ExternalKey(WPARAM vk);
    void Close() { Dismiss(); }

private:
    bool Init();
    bool CreateTextFormats();
    void BuildLayout();
    bool CreateMenuWindow();
    bool CreateRenderTarget();
    void Cleanup();

    void Render();
    void Tick();
    void Dismiss();
    void Choose(int index);
    void MoveSelection(int delta);
    int HitTest(POINT clientPt) const;
    int TitleIndexAt(POINT clientPt) const;

    void OpenSubmenu(int index);
    void CloseSubmenu(bool fromKeyboard = false);
    void SyncSubmenu();
    bool HasSubmenu(int index) const;
    static bool NeedsTrailingMark(const ContextMenuItem& item);

    void DrawBody();
    void DrawItems();
    void DrawItemText(int index, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color);
    void DrawCheckmark(float right, float centerY);
    void DrawChevron(float x, float centerY);

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT SafeHandle(Win11Menu* self, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HINSTANCE m_hInst;
    HWND m_owner{ nullptr };
    HWND m_hwnd{ nullptr };
    bool m_dark;
    bool m_submenuStyle{ false };  // панель — подменю (без захвата ввода)
    bool m_openLeft{ false };      // подменю раскрылось влево от пункта
    RECT m_anchor{};
    std::vector<ContextMenuItem> m_items;
    std::wstring m_className;

    std::unique_ptr<Win11Menu> m_submenu;
    int m_submenuOwner{ -1 };       // индекс пункта, раскрывшего подменю
    int m_submenuHoverItem{ -1 };
    ULONGLONG m_submenuHoverStart{ 0 };

    UINT m_dpi{ 96 };
    float m_scale{ 1.0f };

    float m_contentW{ 0.0f };
    float m_contentH{ 0.0f };
    std::vector<float> m_itemTop;
    std::vector<float> m_itemH;
    std::vector<ComPtr<IDWriteTextLayout>> m_layouts;

    int m_hover{ -1 };
    bool m_hoverFromMouse{ false };
    bool m_escapeDown{ false };
    // Защита от повторного раскрытия подменю, закрытого клавишей: курсор остаётся
    // на строке, и системный WM_MOUSEMOVE иначе раскрыл бы подменю снова.
    bool m_ignoreReopen{ false };
    POINT m_reopenGuardPt{};
    std::vector<float> m_hoverProgress;

    float m_show{ 0.0f };
    float m_close{ 0.0f };
    bool m_showDone{ false };
    bool m_closing{ false };
    bool m_done{ false };
    ULONGLONG m_showStart{ 0 };
    ULONGLONG m_closeStart{ 0 };
    ULONGLONG m_lastTick{ 0 };
    UINT m_result{ 0 };
    int m_titleClicks{ 0 };

    int m_winX{ 0 };
    int m_winY{ 0 };
    int m_winW{ 0 };
    int m_winH{ 0 };

    ComPtr<ID2D1Factory> m_d2dFactory;
    ComPtr<IDWriteFactory> m_dwrite;
    ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    ComPtr<IDWriteTextFormat> m_formatTitle;
    ComPtr<IDWriteTextFormat> m_formatInfo;
    ComPtr<IDWriteTextFormat> m_formatAction;
    ComPtr<IDWriteInlineObject> m_ellipsis;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    // Круглые концы/стыки для мелких штрихов (галочка) — иначе они выглядят «рвано».
    ComPtr<ID2D1StrokeStyle> m_roundStroke;

    HDC m_memDC{ nullptr };
    HBITMAP m_dib{ nullptr };
    HGDIOBJ m_oldBitmap{ nullptr };
    void* m_bits{ nullptr };
};

Win11Menu::Win11Menu(HINSTANCE hInst, HWND owner, bool darkTheme, RECT anchor,
                     std::vector<ContextMenuItem> items, bool submenuStyle)
    : m_hInst(hInst), m_owner(owner), m_dark(darkTheme), m_submenuStyle(submenuStyle),
      m_anchor(anchor), m_items(std::move(items)) {}

Win11Menu::~Win11Menu() {
    Cleanup();
}

bool Win11Menu::Init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) return false;
    const D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 0.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
    if (FAILED(m_d2dFactory->CreateStrokeStyle(strokeProps, nullptr, 0,
                                               m_roundStroke.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf())))) return false;
    if (!CreateTextFormats()) return false;
    BuildLayout();
    if (!CreateMenuWindow()) return false;
    if (!CreateRenderTarget()) return false;
    return true;
}

bool Win11Menu::CreateTextFormats() {
    auto create = [this](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                         ComPtr<IDWriteTextFormat>& out) {
        HRESULT hr = m_dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU",
                                                out.GetAddressOf());
        if (FAILED(hr) && wcscmp(family, L"Segoe UI Variable Text") == 0) {
            hr = m_dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU",
                                            out.GetAddressOf());
        }
        if (FAILED(hr)) return false;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        return true;
    };

    if (!create(L"Segoe UI Variable Text", 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_formatTitle)) return false;
    if (!create(L"Segoe UI Variable Text", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, m_formatInfo)) return false;
    if (!create(L"Segoe UI Variable Text", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, m_formatAction)) return false;

    return SUCCEEDED(m_dwrite->CreateEllipsisTrimmingSign(m_formatAction.Get(), m_ellipsis.GetAddressOf()));
}

void Win11Menu::BuildLayout() {
    m_itemTop.clear();
    m_itemH.clear();
    m_layouts.clear();

    auto measureWidth = [this](const std::wstring& text, IDWriteTextFormat* format) -> float {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(m_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                              format, 10000.0f, 1000.0f, layout.GetAddressOf()))) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
        return metrics.widthIncludingTrailingWhitespace;
    };

    const float minWidth = m_submenuStyle ? kMinWidthSubmenu : kMinWidth;
    float widest = minWidth;
    for (const auto& item : m_items) {
        float height = kActionHeight;
        float width = minWidth;
        switch (item.kind) {
        case ContextMenuItem::Kind::Title:
            height = kTitleHeight;
            width = measureWidth(item.text, m_formatTitle.Get()) + kBodyPadX * 2.0f;
            break;
        case ContextMenuItem::Kind::Info:
            height = kInfoHeight;
            width = measureWidth(item.text, m_formatInfo.Get()) + kBodyPadX * 2.0f;
            break;
        case ContextMenuItem::Kind::Action:
            height = kActionHeight;
            width = measureWidth(item.text, m_formatAction.Get()) + kBodyPadX * 2.0f +
                    (NeedsTrailingMark(item) ? 16.0f : 0.0f);
            break;
        }
        m_itemH.push_back(height);
        widest = std::max(widest, width);
    }

    m_contentW = std::clamp(widest, minWidth, kMaxWidth);
    m_contentH = kBodyPadTop + kBodyPadBottom;
    for (float h : m_itemH) m_contentH += h;

    float y = kBodyPadTop;
    for (float h : m_itemH) {
        m_itemTop.push_back(y);
        y += h;
    }

    // Готовые текстовые раскладки: пересоздавать их на каждом кадре анимации незачем.
    m_layouts.assign(m_items.size(), nullptr);
    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& item = m_items[i];

        float left = kBodyPadX;
        IDWriteTextFormat* format = m_formatInfo.Get();
        if (item.kind == ContextMenuItem::Kind::Title) format = m_formatTitle.Get();
        else if (item.kind == ContextMenuItem::Kind::Action) format = m_formatAction.Get();
        const float width = m_contentW - left - kBodyPadX -
                            (NeedsTrailingMark(item) ? 16.0f : 0.0f);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(m_dwrite->CreateTextLayout(item.text.c_str(), static_cast<UINT32>(item.text.size()),
                                              format, width, m_itemH[i], layout.GetAddressOf()))) {
            continue;
        }
        DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        layout->SetTrimming(&trim, m_ellipsis.Get());
        m_layouts[i] = std::move(layout);
    }

    m_hoverProgress.assign(m_items.size(), 0.0f);
}

bool Win11Menu::CreateMenuWindow() {
    m_dpi = GetDpiForWindow(m_owner);
    if (m_dpi == 0) m_dpi = 96;
    m_scale = static_cast<float>(m_dpi) / 96.0f;

    const int bodyW = static_cast<int>(std::lround(m_contentW * m_scale));
    const int bodyH = static_cast<int>(std::lround(m_contentH * m_scale));
    const int shadow = static_cast<int>(std::lround(kShadowPad * m_scale));
    const int gap = static_cast<int>(std::lround(6.0f * m_scale));

    MONITORINFO mi{ sizeof(MONITORINFO) };
    RECT anchor = m_anchor;
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &mi)) return false;
    const RECT work = mi.rcWork;
    const int workLeft = static_cast<int>(work.left);
    const int workRight = static_cast<int>(work.right);
    // По вертикали ограничиваемся монитором, а не рабочей областью: виджет стоит
    // на панели задач, и меню-всплывашка может чуть перекрывать её, как в Win11.
    const int boundsTop = static_cast<int>(mi.rcMonitor.top);
    const int boundsBottom = static_cast<int>(mi.rcMonitor.bottom);

    int bodyLeft = static_cast<int>(m_anchor.left);
    int bodyTop = 0;
    if (m_submenuStyle) {
        // Подменю прижимается верхом к строке-родителю и раскрывается вправо;
        // у правого края экрана — влево, как в Windows 11.
        const int gap = static_cast<int>(std::lround(kSubmenuGap * m_scale));
        const int rightSide = static_cast<int>(m_anchor.right) + gap;
        const int leftSide = static_cast<int>(m_anchor.left) - gap - bodyW;
        m_openLeft = rightSide + bodyW > workRight - 8 && leftSide >= workLeft + 8;
        bodyLeft = m_openLeft ? leftSide : rightSide;
        bodyLeft = std::clamp(bodyLeft, workLeft + 8,
                              std::max(workLeft + 8, workRight - bodyW - 8));
        bodyTop = static_cast<int>(m_anchor.top) - static_cast<int>(std::lround(kBodyPadTop * m_scale));
        bodyTop = std::clamp(bodyTop, boundsTop + 8,
                             std::max(boundsTop + 8, boundsBottom - 8 - bodyH));
    } else {
        if (bodyLeft + bodyW > workRight - 8) bodyLeft = static_cast<int>(m_anchor.right) - bodyW;
        bodyLeft = std::clamp(bodyLeft, workLeft + 8,
                              std::max(workLeft + 8, workRight - bodyW - 8));

        bodyTop = static_cast<int>(m_anchor.top) - gap - bodyH;
        if (bodyTop < boundsTop + 8) bodyTop = static_cast<int>(m_anchor.bottom) + gap;
        if (bodyTop + bodyH > boundsBottom - 8) bodyTop = boundsBottom - 8 - bodyH;
        bodyTop = std::max(bodyTop, boundsTop + 8);
    }

    m_winX = bodyLeft - shadow;
    m_winY = bodyTop - shadow;
    m_winW = bodyW + shadow * 2;
    m_winH = bodyH + shadow * 2;

    // Уникальность класса: id процесса + адрес объекта + тик. Адрес нужен,
    // чтобы основное меню и его подменю не столкнулись в одну миллисекунду.
    m_className = L"BatteryInTaskBar.Menu." +
                  std::to_wstring(reinterpret_cast<std::uintptr_t>(m_hInst)) + L"." +
                  std::to_wstring(reinterpret_cast<std::uintptr_t>(this)) + L"." +
                  std::to_wstring(GetTickCount64());

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, StaticWndProc, 0, 0,
                       m_hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr,
                       m_className.c_str(), nullptr };
    if (!RegisterClassExW(&wc)) return false;

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    if (m_submenuStyle) exStyle |= WS_EX_NOACTIVATE;  // фокус остаётся у основного меню
    m_hwnd = CreateWindowExW(
        exStyle,
        m_className.c_str(), m_submenuStyle ? kSubmenuWindowTitle : kMenuWindowTitle,
        WS_POPUP,
        m_winX, m_winY, m_winW, m_winH,
        nullptr, nullptr, m_hInst, this);
    if (!m_hwnd) return false;

    m_showStart = GetTickCount64();
    m_lastTick = m_showStart;
    m_hover = -1;
    return true;
}

bool Win11Menu::CreateRenderTarget() {
    HDC screen = GetDC(nullptr);
    m_memDC = CreateCompatibleDC(screen);
    if (screen) ReleaseDC(nullptr, screen);
    if (!m_memDC) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m_winW;
    bmi.bmiHeader.biHeight = -m_winH;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_dib = CreateDIBSection(m_memDC, &bmi, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    if (!m_dib) return false;
    m_oldBitmap = SelectObject(m_memDC, m_dib);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(m_dpi), static_cast<float>(m_dpi));
    if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, m_renderTarget.GetAddressOf()))) return false;

    RECT rc{ 0, 0, m_winW, m_winH };
    if (FAILED(m_renderTarget->BindDC(m_memDC, &rc))) return false;
    m_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), m_brush.GetAddressOf()))) {
        return false;
    }

    // Первый кадр рисуем до показа окна, чтобы не мигнуть пустым прямоугольником.
    Render();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_hwnd, HWND_TOPMOST, m_winX, m_winY, m_winW, m_winH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!m_submenuStyle) {
        // Фокус и мышь остаются у основного меню: подменю получает ввод через него.
        SetForegroundWindow(m_hwnd);
        SetFocus(m_hwnd);
        SetCapture(m_hwnd);
    }
    // Кадры тикает цикл Run(): подменю не нужно собственного таймера, его
    // анимацию двигает родительское меню.
    return true;
}

void Win11Menu::Cleanup() {
    if (m_hwnd) {
        KillTimer(m_hwnd, kTimerFrame);
        if (GetCapture() == m_hwnd) ReleaseCapture();
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_memDC && m_oldBitmap) {
        SelectObject(m_memDC, m_oldBitmap);
        m_oldBitmap = nullptr;
    }
    m_renderTarget.Reset();
    m_brush.Reset();
    m_roundStroke.Reset();
    m_layouts.clear();
    m_ellipsis.Reset();
    m_formatTitle.Reset();
    m_formatInfo.Reset();
    m_formatAction.Reset();
    m_dwrite.Reset();
    m_d2dFactory.Reset();
    if (m_dib) { DeleteObject(m_dib); m_dib = nullptr; }
    if (m_memDC) { DeleteDC(m_memDC); m_memDC = nullptr; }
    if (!m_className.empty()) {
        UnregisterClassW(m_className.c_str(), m_hInst);
        m_className.clear();
    }
}

UINT Win11Menu::Run() {
    if (!Init()) {
        Cleanup();
        return 0;
    }

    // Высокоточный таймер кадров: SetTimer/WM_TIMER дают шаг ~15.6 мс, из-за чего
    // анимация открытия выглядела как несколько рывков. Фолбэк — обычный WM_TIMER.
    HANDLE frameTimer = CreateWaitableTimerExW(nullptr, nullptr,
                                               CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
    if (frameTimer) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(kFramePeriodMs) * 10000LL;  // 100-нс единицы
        if (!SetWaitableTimer(frameTimer, &due, static_cast<LONG>(kFramePeriodMs),
                              nullptr, nullptr, FALSE)) {
            CloseHandle(frameTimer);
            frameTimer = nullptr;
        }
    }
    if (!frameTimer) SetTimer(m_hwnd, kTimerFrame, kFramePeriodMs, nullptr);

    MSG msg{};
    bool gotQuit = false;
    WPARAM quitParam = 0;
    const DWORD msgIndex = frameTimer ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0;
    while (!m_done) {
        const DWORD wait = MsgWaitForMultipleObjects(frameTimer ? 1 : 0, &frameTimer, FALSE,
                                                     INFINITE, QS_ALLINPUT);
        if (frameTimer && wait == WAIT_OBJECT_0) {
            Tick();
            continue;
        }
        if (wait != msgIndex) break;  // ошибка ожидания

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                gotQuit = true;
                quitParam = msg.wParam;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (gotQuit) break;
    }

    if (frameTimer) CloseHandle(frameTimer);
    Cleanup();
    // Если во вложенном цикле пришёл WM_QUIT (например, виджет закрыли из меню),
    // возвращаем его внешнему циклу приложения, иначе тот продолжит ждать сообщений.
    if (gotQuit) PostQuitMessage(static_cast<int>(quitParam));
    return m_result;
}

void Win11Menu::Render() {
    if (!m_renderTarget) return;

    m_renderTarget->BeginDraw();
    m_renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
    m_renderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    float fade = m_showDone ? 1.0f : Clamp01(m_show);
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;

    if (!m_showDone) {
        const float t = Clamp01(static_cast<float>(GetTickCount64() - m_showStart) /
                                (kShowSeconds * 1000.0f));
        const float eased = EaseOutBack(t);
        scale = 0.90f + 0.10f * eased;
        offsetY = (1.0f - eased) * 10.0f;
        // Подменю «выезжает» из строки-родителя по горизонтали.
        if (m_submenuStyle) offsetX = (1.0f - eased) * (m_openLeft ? -6.0f : 6.0f);
        fade = Clamp01(t * 1.8f);
    }
    if (m_closing) {
        fade *= (1.0f - m_close);
        scale *= (1.0f - 0.02f * m_close);
        offsetY += 4.0f * m_close;
    }

    const D2D1_POINT_2F anchor = m_submenuStyle
        ? D2D1::Point2F(kShadowPad + (m_openLeft ? m_contentW : 0.0f), kShadowPad + m_contentH * 0.5f)
        : D2D1::Point2F(kShadowPad + m_contentW * 0.5f, kShadowPad + m_contentH);
    const D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Scale(scale, scale, anchor) *
        D2D1::Matrix3x2F::Translation(offsetX, offsetY);
    m_renderTarget->SetTransform(transform);

    DrawBody();
    DrawItems();

    m_renderTarget->EndDraw();
    m_renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());

    BLENDFUNCTION blend{ AC_SRC_OVER, 0,
                         static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(fade * 255.0f)), 0, 255)),
                         AC_SRC_ALPHA };
    POINT dst{ m_winX, m_winY };
    SIZE size{ m_winW, m_winH };
    POINT src{ 0, 0 };
    UpdateLayeredWindow(m_hwnd, nullptr, &dst, &size, m_memDC, &src, 0, &blend, ULW_ALPHA);
}

void Win11Menu::Tick() {
    const ULONGLONG now = GetTickCount64();
    float dt = static_cast<float>(now - m_lastTick) / 1000.0f;
    m_lastTick = now;
    dt = std::clamp(dt, 0.0f, 0.1f);

    bool needsPaint = false;

    if (!m_showDone) {
        const float t = Clamp01(static_cast<float>(now - m_showStart) / (kShowSeconds * 1000.0f));
        m_show = t;
        if (t >= 1.0f) {
            m_show = 1.0f;
            m_showDone = true;
        }
        needsPaint = true;
    }

    if (m_closing) {
        m_close = Clamp01(static_cast<float>(now - m_closeStart) / (kHideSeconds * 1000.0f));
        needsPaint = true;
        if (m_close >= 1.0f) {
            m_done = true;
            // Будим GetMessage, не трогая очередь WM_QUIT внешнего цикла приложения.
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            return;
        }
    }

    const float smoothing = 1.0f - std::exp(-dt / kHoverTauSeconds);
    for (size_t i = 0; i < m_hoverProgress.size(); ++i) {
        const float target = (static_cast<int>(i) == m_hover) ? 1.0f : 0.0f;
        float next = m_hoverProgress[i] + (target - m_hoverProgress[i]) * smoothing;
        if (std::abs(next - target) < 0.004f) next = target;
        if (std::abs(next - m_hoverProgress[i]) > 0.001f) {
            m_hoverProgress[i] = next;
            needsPaint = true;
        } else {
            m_hoverProgress[i] = next;
        }
    }

    // Подменю своего таймера не имеет: его кадры двигает родительское меню.
    if (m_submenu && !m_submenu->IsDone()) m_submenu->Tick();

    SyncSubmenu();
    if (m_done) return;

    // Реагируем только на нажатие, а не на удержание: первое нажатие закрывает
    // подменю, второе — всё меню. WM_KEYDOWN помечает нажатие обработанным.
    const bool escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (escapeDown && !m_escapeDown) {
        m_escapeDown = true;
        if (m_submenu && !m_submenu->IsDone() && !m_submenu->IsClosing()) {
            CloseSubmenu(true);
        } else {
            Dismiss();
        }
        return;
    }
    m_escapeDown = escapeDown;

    if (needsPaint) Render();
}

void Win11Menu::Dismiss() {
    if (m_closing || m_done) return;
    if (m_submenu) m_submenu->Close();
    m_closing = true;
    m_close = 0.0f;
    m_closeStart = GetTickCount64();
}

void Win11Menu::Choose(int index) {
    if (index < 0 || index >= static_cast<int>(m_items.size())) return;
    const auto& item = m_items[index];
    if (item.kind != ContextMenuItem::Kind::Action || !item.enabled) return;
    if (!item.submenu.empty()) {
        OpenSubmenu(index);
        return;
    }
    m_result = item.id;
    Dismiss();
}

void Win11Menu::MoveSelection(int delta) {
    m_hoverFromMouse = false;
    std::vector<int> actions;
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].kind == ContextMenuItem::Kind::Action && m_items[i].enabled) {
            actions.push_back(static_cast<int>(i));
        }
    }
    if (actions.empty()) return;

    int pos = -1;
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i] == m_hover) { pos = static_cast<int>(i); break; }
    }
    if (pos < 0) pos = (delta > 0) ? 0 : static_cast<int>(actions.size()) - 1;
    else pos = (pos + delta + static_cast<int>(actions.size())) % static_cast<int>(actions.size());

    m_hover = actions[pos];
}

int Win11Menu::HitTest(POINT clientPt) const {
    if (m_scale <= 0.0f) return -1;
    const float x = static_cast<float>(clientPt.x) / m_scale - kShadowPad;
    const float y = static_cast<float>(clientPt.y) / m_scale - kShadowPad;
    if (x < 0.0f || y < 0.0f || x >= m_contentW || y >= m_contentH) return -1;

    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].kind != ContextMenuItem::Kind::Action) continue;
        const float top = m_itemTop[i];
        if (y >= top && y < top + m_itemH[i]) {
            return m_items[i].enabled ? static_cast<int>(i) : -2;
        }
    }
    return -2;  // внутри панели, но не интерактивный пункт
}

int Win11Menu::TitleIndexAt(POINT clientPt) const {
    if (m_scale <= 0.0f) return -1;
    const float x = static_cast<float>(clientPt.x) / m_scale - kShadowPad;
    const float y = static_cast<float>(clientPt.y) / m_scale - kShadowPad;
    if (x < 0.0f || y < 0.0f || x >= m_contentW || y >= m_contentH) return -1;

    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_items[i].kind != ContextMenuItem::Kind::Title) continue;
        if (y >= m_itemTop[i] && y < m_itemTop[i] + m_itemH[i]) return static_cast<int>(i);
    }
    return -1;
}

bool Win11Menu::HasSubmenu(int index) const {
    return index >= 0 && index < static_cast<int>(m_items.size()) && !m_items[index].submenu.empty();
}

bool Win11Menu::NeedsTrailingMark(const ContextMenuItem& item) {
    return item.kind == ContextMenuItem::Kind::Action && (item.checked || !item.submenu.empty());
}

void Win11Menu::OpenSubmenu(int index) {
    if (m_submenuStyle || m_closing || m_done) return;
    if (!HasSubmenu(index)) return;
    if (m_submenu) {
        // Прежнее подменю сначала должно дозакрыться: два окна разом — визуальный шум.
        if (m_submenuOwner != index) m_submenu->Close();
        return;
    }

    const int shadow = static_cast<int>(std::lround(kShadowPad * m_scale));
    const int bodyLeft = m_winX + shadow;
    const int bodyTop = m_winY + shadow;
    const int rowTop = bodyTop + static_cast<int>(std::lround(m_itemTop[index] * m_scale));
    const int rowBottom = rowTop + static_cast<int>(std::lround(m_itemH[index] * m_scale));
    RECT row{ bodyLeft, rowTop,
              bodyLeft + static_cast<int>(std::lround(m_contentW * m_scale)), rowBottom };

    auto submenu = std::make_unique<Win11Menu>(m_hInst, m_owner, m_dark, row,
                                               m_items[index].submenu, true);
    if (!submenu->Init()) {
        submenu->Cleanup();
        return;
    }
    m_submenu = std::move(submenu);
    m_submenuOwner = index;
    m_submenuHoverItem = index;
}

void Win11Menu::CloseSubmenu(bool fromKeyboard) {
    if (m_submenu) m_submenu->Close();
    m_submenuHoverItem = -1;
    // После закрытия клавишей подменю не должно раскрываться само, пока курсор
    // не сдвинется: иначе Esc и стрелка «влево» закрывали бы его бесконечно.
    m_hoverFromMouse = false;
    if (fromKeyboard) {
        m_ignoreReopen = true;
        GetCursorPos(&m_reopenGuardPt);
    }
}

void Win11Menu::SyncSubmenu() {
    if (m_submenu) {
        // Выбор в подменю закрывает всё меню, как в Windows 11.
        if (m_submenu->Result() != 0) {
            m_result = m_submenu->Result();
            Dismiss();
            return;
        }
        if (m_submenu->IsDone()) {
            m_submenu.reset();
            m_submenuOwner = -1;
            m_submenuHoverItem = -1;
        }
    }

    if (m_submenu || m_closing || m_done || m_submenuStyle || !m_hoverFromMouse) return;
    if (HasSubmenu(m_hover)) {
        const ULONGLONG now = GetTickCount64();
        if (m_submenuHoverItem != m_hover) {
            m_submenuHoverItem = m_hover;
            m_submenuHoverStart = now;
        } else if (now - m_submenuHoverStart >= kSubmenuOpenDelayMs) {
            OpenSubmenu(m_hover);
        }
    } else {
        m_submenuHoverItem = -1;
    }
}

bool Win11Menu::ContainsScreenPoint(POINT screenPt) const noexcept {
    if (!m_hwnd) return false;
    RECT rc{};
    if (!GetWindowRect(m_hwnd, &rc)) return false;
    return PtInRect(&rc, screenPt) != FALSE;
}

void Win11Menu::ExternalMouseMove(POINT screenPt) {
    if (!m_hwnd) return;
    POINT pt = screenPt;
    if (!ScreenToClient(m_hwnd, &pt)) return;
    const int hit = HitTest(pt);
    m_hover = (hit >= 0) ? hit : -1;
}

bool Win11Menu::ExternalClick(POINT screenPt) {
    if (!m_hwnd) return false;
    POINT pt = screenPt;
    if (!ScreenToClient(m_hwnd, &pt)) return false;
    const int hit = HitTest(pt);
    if (hit >= 0) {
        Choose(hit);
        return true;
    }
    // Клик по «телу» подменю вне пункта: гасим его, но родителю не отдаём.
    return hit == -2;
}

void Win11Menu::ExternalKey(WPARAM vk) {
    if (!m_hwnd || m_closing || m_done) return;
    switch (vk) {
    case VK_UP: MoveSelection(-1); break;
    case VK_DOWN: MoveSelection(1); break;
    case VK_RETURN:
    case VK_SPACE:
        if (m_hover >= 0) Choose(m_hover);
        break;
    default: break;
    }
}

void Win11Menu::DrawBody() {
    const float left = kShadowPad;
    const float top = kShadowPad;
    const float right = kShadowPad + m_contentW;
    const float bottom = kShadowPad + m_contentH;

    // Мягкая тень: несколько полупрозрачных скруглённых слоёв со сдвигом вниз.
    for (int i = 4; i >= 1; --i) {
        const float grow = static_cast<float>(i) * 2.2f;
        const D2D1_ROUNDED_RECT shadow = D2D1::RoundedRect(
            D2D1::RectF(left - grow, top - grow + 2.0f, right + grow, bottom + grow + 2.0f),
            kCornerRadius + grow, kCornerRadius + grow);
        m_brush->SetColor(MakeColor(0.0f, 0.0f, 0.0f, 0.045f));
        m_renderTarget->FillRoundedRectangle(shadow, m_brush.Get());
    }

    const D2D1_ROUNDED_RECT body = D2D1::RoundedRect(
        D2D1::RectF(left, top, right, bottom), kCornerRadius, kCornerRadius);
    m_brush->SetColor(m_dark ? MakeColor(0.173f, 0.173f, 0.173f)
                             : MakeColor(0.976f, 0.976f, 0.976f));
    m_renderTarget->FillRoundedRectangle(body, m_brush.Get());

    m_brush->SetColor(m_dark ? MakeColor(1.0f, 1.0f, 1.0f, 0.08f)
                             : MakeColor(0.0f, 0.0f, 0.0f, 0.10f));
    m_renderTarget->DrawRoundedRectangle(body, m_brush.Get(), 1.0f);
}

void Win11Menu::DrawItems() {
    const D2D1_COLOR_F primary = m_dark ? MakeColor(1.0f, 1.0f, 1.0f, 0.96f)
                                        : MakeColor(0.05f, 0.05f, 0.05f, 0.96f);
    const D2D1_COLOR_F secondary = m_dark ? MakeColor(1.0f, 1.0f, 1.0f, 0.62f)
                                          : MakeColor(0.35f, 0.35f, 0.35f, 0.96f);
    const D2D1_COLOR_F hoverColor = m_dark ? MakeColor(1.0f, 1.0f, 1.0f, 0.065f)
                                           : MakeColor(0.0f, 0.0f, 0.0f, 0.05f);

    const float bodyLeft = kShadowPad;
    const float bodyTop = kShadowPad;
    const float bodyRight = bodyLeft + m_contentW;

    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& item = m_items[i];
        const float top = bodyTop + m_itemTop[i];
        const float height = m_itemH[i];

        if (item.kind == ContextMenuItem::Kind::Action) {
            const float progress = m_hoverProgress[i];
            if (progress > 0.001f) {
                D2D1_COLOR_F color = hoverColor;
                color.a *= progress;
                m_brush->SetColor(color);
                const D2D1_ROUNDED_RECT highlight = D2D1::RoundedRect(
                    D2D1::RectF(bodyLeft + 3.0f, top + 1.0f, bodyRight - 3.0f, top + height - 1.0f),
                    kRowRadius, kRowRadius);
                m_renderTarget->FillRoundedRectangle(highlight, m_brush.Get());
            }
            if (item.checked) {
                // Галочка справа: слева не оставляем пустую «иконочную» колонку.
                DrawCheckmark(bodyRight - kBodyPadX - 4.5f, top + height * 0.5f);
            } else if (!item.submenu.empty()) {
                // Стрелка раскрывающегося подменю — там же, где была бы галочка.
                DrawChevron(bodyRight - kBodyPadX - 7.5f, top + height * 0.5f);
            }
        }

        D2D1_COLOR_F color = primary;
        if (item.kind == ContextMenuItem::Kind::Info) color = secondary;
        if (item.kind == ContextMenuItem::Kind::Action && !item.enabled) color = secondary;

        DrawItemText(static_cast<int>(i),
                     D2D1::RectF(bodyLeft + kBodyPadX, top, bodyRight - kBodyPadX, top + height), color);
    }
}

void Win11Menu::DrawItemText(int index, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    if (index < 0 || index >= static_cast<int>(m_layouts.size())) return;
    const auto& layout = m_layouts[index];
    if (!layout) return;

    // Формат уже центрирует строку по вертикали внутри высоты строки меню.
    m_brush->SetColor(color);
    m_renderTarget->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), m_brush.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Win11Menu::DrawCheckmark(float right, float centerY) {
    // Галочка в цвете текста — чуть мягче и заметно мельче прежней «акцентной».
    m_brush->SetColor(m_dark ? MakeColor(1.0f, 1.0f, 1.0f, 0.86f)
                             : MakeColor(0.22f, 0.22f, 0.22f, 0.94f));

    const D2D1_POINT_2F p1 = D2D1::Point2F(right - 8.2f, centerY + 0.3f);
    const D2D1_POINT_2F p2 = D2D1::Point2F(right - 5.1f, centerY + 3.4f);
    const D2D1_POINT_2F p3 = D2D1::Point2F(right, centerY - 3.4f);
    m_renderTarget->DrawLine(p1, p2, m_brush.Get(), 1.3f, m_roundStroke.Get());
    m_renderTarget->DrawLine(p2, p3, m_brush.Get(), 1.3f, m_roundStroke.Get());
}

void Win11Menu::DrawChevron(float x, float centerY) {
    // Тонкий «уголок» справа — как у раскрывающихся пунктов Windows 11.
    m_brush->SetColor(m_dark ? MakeColor(1.0f, 1.0f, 1.0f, 0.62f)
                             : MakeColor(0.35f, 0.35f, 0.35f, 0.96f));
    const D2D1_POINT_2F p1 = D2D1::Point2F(x - 3.0f, centerY - 5.0f);
    const D2D1_POINT_2F p2 = D2D1::Point2F(x + 2.0f, centerY);
    const D2D1_POINT_2F p3 = D2D1::Point2F(x - 3.0f, centerY + 5.0f);
    m_renderTarget->DrawLine(p1, p2, m_brush.Get(), 1.4f);
    m_renderTarget->DrawLine(p2, p3, m_brush.Get(), 1.4f);
}

LRESULT CALLBACK Win11Menu::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win11Menu* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Win11Menu*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Win11Menu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return SafeHandle(self, hwnd, msg, wParam, lParam);
}

LRESULT Win11Menu::SafeHandle(Win11Menu* self, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    __try {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

LRESULT Win11Menu::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        // Система присылает WM_MOUSEMOVE и без движения курсора (например, когда
        // подменю закрылось и окно под курсором сменилось). После закрытия подменю
        // клавишей такие события игнорируем, иначе подменю тут же раскроется снова.
        if (m_ignoreReopen) {
            if (screen.x == m_reopenGuardPt.x && screen.y == m_reopenGuardPt.y) return 0;
            m_ignoreReopen = false;
        }
        // Подменю своего захвата не имеет: пока курсор над ним, ввод идёт в него.
        if (m_submenu && !m_submenu->IsDone() && !m_submenu->IsClosing() &&
            m_submenu->ContainsScreenPoint(screen)) {
            m_submenu->ExternalMouseMove(screen);
            return 0;
        }
        m_hoverFromMouse = true;
        const int hit = HitTest(pt);
        const int hover = (hit >= 0) ? hit : -1;
        if (hover != m_hover) {
            // Ушли с пункта, раскрывшего подменю, — подменю закрывается.
            if (m_submenu && m_submenuOwner >= 0 && hover != m_submenuOwner) {
                CloseSubmenu();
                m_hoverFromMouse = true;  // закрыли мышью, а не клавишей
            }
            m_hover = hover;
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (m_submenu && !m_submenu->IsDone() && !m_submenu->IsClosing() &&
            m_submenu->ContainsScreenPoint(screen)) {
            if (msg == WM_LBUTTONUP) m_submenu->ExternalClick(screen);
            return 0;
        }
        const int hit = HitTest(pt);
        if (msg == WM_LBUTTONUP) {
            if (hit >= 0) {
                Choose(hit);
            } else if (hit == -1) {
                Dismiss();
            } else if (!m_submenuStyle) {
                // Скрытая команда поддержки: пять кликов по названию устройства
                // собирают единый отчёт (версия, HID-дамп, журнал). Пункт-заголовок
                // не выбирается обычным кликом, поэтому счётчик живёт только здесь.
                const int titleIndex = TitleIndexAt(pt);
                if (titleIndex >= 0 && ++m_titleClicks >= kReportClickCount) {
                    m_result = m_items[titleIndex].id;
                    Dismiss();
                }
            }
        }
        return 0;
    }

    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        Dismiss();
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) m_escapeDown = true;  // чтобы Tick не закрыл меню повторно
        // Пока открыто подменю, стрелки и Enter работают в нём.
        if (m_submenu && !m_submenu->IsDone() && !m_submenu->IsClosing()) {
            switch (wParam) {
            case VK_ESCAPE:
            case VK_LEFT:
                CloseSubmenu(true);
                return 0;
            case VK_UP:
                m_submenu->ExternalKey(VK_UP);
                return 0;
            case VK_DOWN:
                m_submenu->ExternalKey(VK_DOWN);
                return 0;
            case VK_RETURN:
            case VK_SPACE:
                m_submenu->ExternalKey(VK_RETURN);
                return 0;
            default:
                break;
            }
        }
        switch (wParam) {
        case VK_ESCAPE:
            Dismiss();
            break;
        case VK_UP:
            MoveSelection(-1);
            break;
        case VK_DOWN:
            MoveSelection(1);
            break;
        case VK_RIGHT:
            if (HasSubmenu(m_hover)) OpenSubmenu(m_hover);
            break;
        case VK_RETURN:
        case VK_SPACE:
            if (m_hover >= 0) {
                if (HasSubmenu(m_hover)) {
                    OpenSubmenu(m_hover);
                    if (m_submenu) m_submenu->ExternalKey(VK_DOWN);  // сразу выбираем первый пункт
                } else {
                    Choose(m_hover);
                }
            }
            break;
        default:
            break;
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && m_showDone && !m_closing &&
            (GetTickCount64() - m_showStart) > 250) {
            Dismiss();
        }
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_CAPTURECHANGED:
        if (!m_closing && !m_done) Dismiss();
        return 0;

    case WM_TIMER:
        if (wParam == kTimerFrame) Tick();
        return 0;

    case WM_DESTROY:
        m_hwnd = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

UINT ShowWin11Menu(HINSTANCE hInst, HWND owner, bool darkTheme, const RECT& anchorRect,
                   const std::vector<ContextMenuItem>& items) {
    if (items.empty()) return 0;
    Win11Menu menu(hInst, owner, darkTheme, anchorRect, items);
    return menu.Run();
}
