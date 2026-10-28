#include "ui/Renderer.h"
#include <algorithm>
#include <cmath>
#include "core/Localization.h"

namespace {
constexpr float kBaseTileHeight = 38.0f;  // высота плитки в DIP, относительно неё считается масштаб
constexpr float kTileRadius = 8.0f;       // скругление карточки в духе Windows 11
constexpr float kPaddingX = 12.0f;        // боковые отступы содержимого
constexpr float kBarHeight = 3.0f;        // высота полосы заряда
constexpr float kBarTop = 30.5f;          // верх полосы: заметный зазор под подстрокой

// Glass подстраивается под системную тему: панель задач под виджетом тоже
// следует ей, и тёмный текст на тёмной панели был бы нечитаем.
bool IsDarkAppearance(WidgetTheme theme, bool systemDark) {
    return theme == WidgetTheme::Dark || (theme == WidgetTheme::Glass && systemDark);
}

/// Средний заряд наушников для плитки. Кейс — только если самих наушников нет:
/// смешивать их с кейсом в одно число было бы враньём.
int HeadsetAverage(const AirPodsTelemetry& airPods) {
    int sum = 0;
    int count = 0;
    if (airPods.leftLevel >= 0) { sum += airPods.leftLevel; ++count; }
    if (airPods.rightLevel >= 0) { sum += airPods.rightLevel; ++count; }
    if (count == 0) return airPods.caseLevel;
    return (sum + count / 2) / count;
}

std::wstring PodLevelText(int level) {
    return level < 0 ? std::wstring(L"—") : std::to_wstring(level) + L"%";
}

/// Цвет полосы по уровню — тот же, что у мыши в обычном состоянии.
/// Зарядка цвет не меняет: у наушников в кейсе она почти всегда включена,
/// и синяя полоса выглядела бы постоянной, а не информативной.
void SetBarColor(ID2D1SolidColorBrush* brush, int level, float pulseAlpha) {
    if (level <= 15) {
        const float rPulse = 0.80f + 0.20f * pulseAlpha;
        brush->SetColor(D2D1::ColorF(1.0f * rPulse, 0.30f, 0.30f, 1.0f));
    } else if (level <= 25) {
        brush->SetColor(D2D1::ColorF(1.0f, 0.72f, 0.30f, 1.0f));
    } else {
        // Зелёный акцент Windows 11 (#6CCB5F).
        brush->SetColor(D2D1::ColorF(0.42f, 0.80f, 0.37f, 1.0f));
    }
}
}  // namespace

bool Renderer::Initialize() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) return false;

    if (FAILED(m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 8.5f, L10n::LocaleName(), m_textFormatDevice.GetAddressOf()))) return false;
    if (FAILED(m_textFormatDevice->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP))) return false;

    if (FAILED(m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.5f, L10n::LocaleName(), m_textFormatPercent.GetAddressOf()))) return false;
    if (FAILED(m_textFormatPercent->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING))) return false;

    if (FAILED(m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.5f, L10n::LocaleName(), m_textFormatSub.GetAddressOf()))) return false;
    if (FAILED(m_textFormatSub->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP))) return false;

    // Заголовок второй плитки — чуть крупнее, чтобы имя модели не терялось
    // рядом с тремя значениями заряда.
    if (FAILED(m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.0f, L10n::LocaleName(), m_textFormatHeadsetTitle.GetAddressOf()))) return false;
    if (FAILED(m_textFormatHeadsetTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP))) return false;

    return true;
}

void Renderer::DiscardDeviceResources() {
    m_renderTarget.Reset();
    m_brushBg.Reset();
    m_bgStops.Reset();
    m_brushHover.Reset();
    m_brushBorder.Reset();
    m_brushTextPrimary.Reset();
    m_brushTextSecondary.Reset();
    m_brushTextBright.Reset();
    m_brushBarTrack.Reset();
    m_brushBarFill.Reset();
    m_brushGlow.Reset();
}

void Renderer::Render(HWND hwnd, const DeviceTelemetry& telemetry, WidgetTheme theme, bool systemDark,
                      float hoverProgress, float animPhase) {
    const bool dark = IsDarkAppearance(theme, systemDark);
    // Смена темы или системной темы меняет цвета кистей — ресурсы пересоздаём.
    if (m_renderTarget && (theme != m_theme || dark != m_darkAppearance)) DiscardDeviceResources();
    CreateDeviceResources(hwnd, theme, systemDark);
    if (!m_renderTarget) return;

    // Окно может менять размер (перетаскивание, смена DPI), а HwndRenderTarget
    // сам размер не меняет: без Resize карточка рисовалась бы в старом буфере.
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const D2D1_SIZE_U wanted = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(rc.right - rc.left, 0)),
            static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top, 0)));
        const D2D1_SIZE_U current = m_renderTarget->GetPixelSize();
        if (wanted.width != current.width || wanted.height != current.height) {
            if (FAILED(m_renderTarget->Resize(wanted))) {
                DiscardDeviceResources();
                return;
            }
        }
    }

    // GetSize() возвращает размер в DIP — масштаб DPI уже учтён, дублировать его нельзя.
    const D2D1_SIZE_F size = m_renderTarget->GetSize();
    if (size.width < 1.0f || size.height < 1.0f) return;

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float scale = size.height / kBaseTileHeight;
    const float pulseAlpha = 0.5f * (sinf(animPhase) + 1.0f);
    DrawMouseCard(D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f), telemetry,
                  hoverProgress, pulseAlpha, scale, dark);

    const HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void Renderer::RenderHeadset(HWND hwnd, const AirPodsTelemetry& airPods, WidgetTheme theme, bool systemDark,
                             float hoverProgress, float animPhase) {
    const bool dark = IsDarkAppearance(theme, systemDark);
    if (m_renderTarget && (theme != m_theme || dark != m_darkAppearance)) DiscardDeviceResources();
    CreateDeviceResources(hwnd, theme, systemDark);
    if (!m_renderTarget) return;

    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const D2D1_SIZE_U wanted = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(rc.right - rc.left, 0)),
            static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top, 0)));
        const D2D1_SIZE_U current = m_renderTarget->GetPixelSize();
        if (wanted.width != current.width || wanted.height != current.height) {
            if (FAILED(m_renderTarget->Resize(wanted))) {
                DiscardDeviceResources();
                return;
            }
        }
    }

    const D2D1_SIZE_F size = m_renderTarget->GetSize();
    if (size.width < 1.0f || size.height < 1.0f) return;

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float scale = size.height / kBaseTileHeight;
    const float pulseAlpha = 0.5f * (sinf(animPhase) + 1.0f);
    DrawHeadsetCard(D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f), airPods,
                    hoverProgress, pulseAlpha, scale, dark);

    const HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void Renderer::DrawMouseCard(const D2D1_RECT_F& card, const DeviceTelemetry& telemetry, float hoverProgress,
                             float pulseAlpha, float scale, bool dark) {
    const float radius = kTileRadius * scale;
    const D2D1_ROUNDED_RECT tileRect = D2D1::RoundedRect(card, radius, radius);

    const bool hasData = telemetry.hasData;
    // Заряд показываем всегда, как только он хоть раз был получен: беспроводная мышь
    // засыпает через 10–15 с простоя, и «СПИТ» вместо процентов только мешает.
    const bool hasLevel = hasData && telemetry.batteryLevel >= 0;
    const int level = std::clamp(telemetry.batteryLevel, 0, 100);

    m_brushBg->SetStartPoint(D2D1::Point2F(0.0f, 0.0f));
    m_brushBg->SetEndPoint(D2D1::Point2F(0.0f, card.bottom));
    m_renderTarget->FillRoundedRectangle(tileRect, m_brushBg.Get());

    // Подсветка в духе Windows 11: не скачок цвета, а мягкое «осветление» плитки,
    // которым управляет плавно интерполированный hoverProgress.
    const float hover = std::clamp(hoverProgress, 0.0f, 1.0f);
    if (hover > 0.001f) {
        m_brushHover->SetColor(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f * hover)
                                    : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.055f * hover));
        m_renderTarget->FillRoundedRectangle(tileRect, m_brushHover.Get());
    }

    if (hasLevel && level <= 15 && !telemetry.isCharging) {
        m_brushGlow->SetColor(D2D1::ColorF(1.0f, 0.15f, 0.15f, 0.10f + 0.20f * pulseAlpha));
        m_renderTarget->FillRoundedRectangle(tileRect, m_brushGlow.Get());

        m_brushGlow->SetColor(D2D1::ColorF(1.0f, 0.20f, 0.20f, 0.30f + 0.40f * pulseAlpha));
        m_renderTarget->DrawRoundedRectangle(tileRect, m_brushGlow.Get(), 1.5f * scale);
    } else {
        const float boost = 0.14f * hover;
        m_brushBorder->SetColor(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f + boost)
                                     : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f + boost * 0.7f));
        m_renderTarget->DrawRoundedRectangle(tileRect, m_brushBorder.Get(), 1.0f);
    }

    // Устройство могло не сообщить модель — тогда показываем нейтральную подпись
    // на языке системы, а не пустое место.
    const std::wstring displayName =
        telemetry.deviceName.empty() ? std::wstring(L10n::Get(Str::DeviceGeneric)) : telemetry.deviceName;
    m_renderTarget->DrawText(displayName.c_str(), static_cast<UINT32>(displayName.length()), m_textFormatDevice.Get(),
        D2D1::RectF(card.left + kPaddingX * scale, 4.0f * scale, card.right - 48.0f * scale, 19.0f * scale),
        m_brushTextPrimary.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    std::wstring percStr = hasLevel ? (std::to_wstring(level) + L"%") : L"…";
    m_renderTarget->DrawText(percStr.c_str(), static_cast<UINT32>(percStr.length()), m_textFormatPercent.Get(),
        D2D1::RectF(card.right - 46.0f * scale, 3.5f * scale, card.right - kPaddingX * scale, 19.0f * scale),
        m_brushTextPrimary.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

    std::wstring subStr;
    if (hasLevel && telemetry.estimatedMinutes > 0) {
        subStr = telemetry.isCharging ? L10n::ChargingRemaining(telemetry.estimatedMinutes)
                                      : L10n::RemainingShort(telemetry.estimatedMinutes);
    } else if (!hasData) {
        subStr = L10n::Get(Str::SubSearching);
    } else {
        subStr = telemetry.isCharging ? L10n::Get(Str::SubCharging)
                                      : (hasLevel ? L10n::Get(Str::SubConnected) : L"---");
    }

    m_renderTarget->DrawText(subStr.c_str(), static_cast<UINT32>(subStr.length()), m_textFormatSub.Get(),
        D2D1::RectF(card.left + kPaddingX * scale, 15.0f * scale, card.right - kPaddingX * scale, 28.0f * scale),
        m_brushTextBright.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    const float barLeft = card.left + kPaddingX * scale;
    const float barRight = card.right - kPaddingX * scale;
    const float barTop = kBarTop * scale;
    const float barBottom = barTop + kBarHeight * scale;
    const float barRadius = (kBarHeight * scale) * 0.5f;
    const float barWidth = barRight > barLeft ? (barRight - barLeft) : 0.0f;
    D2D1_ROUNDED_RECT trackBar = D2D1::RoundedRect(
        D2D1::RectF(barLeft, barTop, barRight, barBottom), barRadius, barRadius);
    m_renderTarget->FillRoundedRectangle(trackBar, m_brushBarTrack.Get());

    float fillLength = (hasLevel && level > 0) ? (barWidth * (static_cast<float>(level) / 100.0f)) : 0.0f;
    if (fillLength > 0.001f) {
        const float halfFill = fillLength * 0.5f;
        const float fillRadius = halfFill < barRadius ? halfFill : barRadius;
        D2D1_ROUNDED_RECT fillBar = D2D1::RoundedRect(
            D2D1::RectF(barLeft, barTop, barLeft + fillLength, barBottom), fillRadius, fillRadius);
        if (telemetry.isCharging) {
            // Голубой акцент Windows 11 (#4CC2FF) с мягким дыханием.
            const float bright = 0.78f + 0.22f * pulseAlpha;
            m_brushBarFill->SetColor(D2D1::ColorF(0.30f * bright, 0.76f * bright, 1.0f, 1.0f));
        } else {
            SetBarColor(m_brushBarFill.Get(), level, pulseAlpha);
        }
        m_renderTarget->FillRoundedRectangle(fillBar, m_brushBarFill.Get());
    }
}

void Renderer::DrawHeadsetCard(const D2D1_RECT_F& card, const AirPodsTelemetry& airPods, float hoverProgress,
                               float pulseAlpha, float scale, bool dark) {
    const float radius = kTileRadius * scale;
    const D2D1_ROUNDED_RECT tileRect = D2D1::RoundedRect(card, radius, radius);

    const int level = HeadsetAverage(airPods);
    const bool hasLevel = level >= 0;
    const bool low = hasLevel && level <= 15;

    m_brushBg->SetStartPoint(D2D1::Point2F(0.0f, 0.0f));
    m_brushBg->SetEndPoint(D2D1::Point2F(0.0f, card.bottom));
    m_renderTarget->FillRoundedRectangle(tileRect, m_brushBg.Get());

    const float hover = std::clamp(hoverProgress, 0.0f, 1.0f);
    if (hover > 0.001f) {
        m_brushHover->SetColor(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f * hover)
                                    : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.055f * hover));
        m_renderTarget->FillRoundedRectangle(tileRect, m_brushHover.Get());
    }

    if (low) {
        m_brushGlow->SetColor(D2D1::ColorF(1.0f, 0.15f, 0.15f, 0.10f + 0.20f * pulseAlpha));
        m_renderTarget->FillRoundedRectangle(tileRect, m_brushGlow.Get());
        m_brushGlow->SetColor(D2D1::ColorF(1.0f, 0.20f, 0.20f, 0.30f + 0.40f * pulseAlpha));
        m_renderTarget->DrawRoundedRectangle(tileRect, m_brushGlow.Get(), 1.5f * scale);
    } else {
        const float boost = 0.14f * hover;
        m_brushBorder->SetColor(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f + boost)
                                     : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f + boost * 0.7f));
        m_renderTarget->DrawRoundedRectangle(tileRect, m_brushBorder.Get(), 1.0f);
    }

    const std::wstring name = airPods.deviceName.empty() ? std::wstring(L10n::Get(Str::HeadsetName))
                                                         : airPods.deviceName;
    m_renderTarget->DrawText(name.c_str(), static_cast<UINT32>(name.length()), m_textFormatHeadsetTitle.Get(),
        D2D1::RectF(card.left + kPaddingX * scale, 3.5f * scale, card.right - 46.0f * scale, 19.0f * scale),
        m_brushTextPrimary.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    const std::wstring percent = hasLevel ? (std::to_wstring(level) + L"%") : L"…";
    m_renderTarget->DrawText(percent.c_str(), static_cast<UINT32>(percent.length()), m_textFormatPercent.Get(),
        D2D1::RectF(card.right - 46.0f * scale, 3.0f * scale, card.right - kPaddingX * scale, 19.0f * scale),
        m_brushTextPrimary.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

    // Три значения в одной строке: так видно, что заряд не «общий», а по компонентам.
    const std::wstring sub = std::wstring(L10n::Get(Str::AirPodsLeftShort)) + L" " +
                             PodLevelText(airPods.leftLevel) + L"  " +
                             L10n::Get(Str::AirPodsRightShort) + L" " + PodLevelText(airPods.rightLevel) + L"  " +
                             L10n::Get(Str::AirPodsCaseShort) + L" " + PodLevelText(airPods.caseLevel);
    m_renderTarget->DrawText(sub.c_str(), static_cast<UINT32>(sub.length()), m_textFormatSub.Get(),
        D2D1::RectF(card.left + kPaddingX * scale, 15.0f * scale, card.right - kPaddingX * scale, 28.0f * scale),
        m_brushTextBright.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    const float barLeft = card.left + kPaddingX * scale;
    const float barRight = card.right - kPaddingX * scale;
    const float barTop = kBarTop * scale;
    const float barBottom = barTop + kBarHeight * scale;
    const float barRadius = (kBarHeight * scale) * 0.5f;
    const float barWidth = barRight > barLeft ? (barRight - barLeft) : 0.0f;
    D2D1_ROUNDED_RECT trackBar = D2D1::RoundedRect(
        D2D1::RectF(barLeft, barTop, barRight, barBottom), barRadius, barRadius);
    m_renderTarget->FillRoundedRectangle(trackBar, m_brushBarTrack.Get());

    const float fillLength = (hasLevel && level > 0) ? (barWidth * (static_cast<float>(level) / 100.0f)) : 0.0f;
    if (fillLength > 0.001f) {
        const float halfFill = fillLength * 0.5f;
        const float fillRadius = halfFill < barRadius ? halfFill : barRadius;
        D2D1_ROUNDED_RECT fillBar = D2D1::RoundedRect(
            D2D1::RectF(barLeft, barTop, barLeft + fillLength, barBottom), fillRadius, fillRadius);
        SetBarColor(m_brushBarFill.Get(), level, pulseAlpha);
        m_renderTarget->FillRoundedRectangle(fillBar, m_brushBarFill.Get());
    }
}

void Renderer::CreateDeviceResources(HWND hwnd, WidgetTheme theme, bool systemDark) {
    if (m_renderTarget) return;

    const bool dark = IsDarkAppearance(theme, systemDark);
    const bool glass = theme == WidgetTheme::Glass;

    RECT rc;
    GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(rc.right - rc.left, 0)),
                                   static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top, 0)));
    if (size.width == 0 || size.height == 0) return;

    // Явно задаём DPI окна: иначе Direct2D берёт системный DPI, и на мониторе
    // с другим масштабом геометрия и шрифты расходятся.
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = 96;

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi),
        static_cast<float>(dpi)
    );

    if (FAILED(m_d2dFactory->CreateHwndRenderTarget(rtProps, D2D1::HwndRenderTargetProperties(hwnd, size), m_renderTarget.GetAddressOf()))) {
        m_renderTarget.Reset();
        return;
    }
    // Grayscale-сглаживание: на полупрозрачной карточке ClearType даёт цветную
    // бахрому и выглядит «пиксельно», особенно на мелких цифрах.
    m_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    auto createBrush = [this](const D2D1_COLOR_F& color, ComPtr<ID2D1SolidColorBrush>& brush) {
        return SUCCEEDED(m_renderTarget->CreateSolidColorBrush(color, brush.GetAddressOf()));
    };

    bool ok = true;
    // Фон — вертикальный градиент: у карточек Windows 11 верх чуть светлее низа.
    // В матовой теме альфа низкая: сквозь плитку просвечивает акрил панели задач,
    // поэтому виджет выглядит её частью, а не наклейкой поверх.
    D2D1_GRADIENT_STOP bgStops[2] = {};
    bgStops[0].position = 0.0f;
    bgStops[1].position = 1.0f;
    if (glass && dark) {
        // Светлый тинт поверх тёмной панели задач: плитка чуть «приподнята» над
        // акрилом, как кнопка панели задач под курсором, но остаётся прозрачной.
        bgStops[0].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.090f);
        bgStops[1].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.035f);
    } else if (glass) {
        bgStops[0].color = D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.62f);
        bgStops[1].color = D2D1::ColorF(0.94f, 0.94f, 0.96f, 0.46f);
    } else if (dark) {
        bgStops[0].color = D2D1::ColorF(0.17f, 0.17f, 0.19f, 0.94f);
        bgStops[1].color = D2D1::ColorF(0.11f, 0.11f, 0.13f, 0.94f);
    } else {
        bgStops[0].color = D2D1::ColorF(0.99f, 0.99f, 0.99f, 0.94f);
        bgStops[1].color = D2D1::ColorF(0.94f, 0.94f, 0.95f, 0.94f);
    }
    ok = ok && SUCCEEDED(m_renderTarget->CreateGradientStopCollection(bgStops, 2, m_bgStops.GetAddressOf()));
    if (m_bgStops) {
        ok = ok && SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, kBaseTileHeight)),
            m_bgStops.Get(), m_brushBg.GetAddressOf()));
    }

    if (dark) {
        // У стекла граница и дорожка полосы чуть заметнее: подложка полупрозрачная.
        const float borderAlpha = glass ? 0.16f : 0.10f;
        const float trackAlpha = glass ? 0.16f : 0.12f;
        ok = ok && createBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f), m_brushHover);
        ok = ok && createBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, borderAlpha), m_brushBorder);
        ok = ok && createBrush(D2D1::ColorF(0.97f, 0.97f, 0.97f, 1.0f), m_brushTextPrimary);
        ok = ok && createBrush(D2D1::ColorF(0.70f, 0.70f, 0.74f, 0.95f), m_brushTextSecondary);
        // Подстрока наушников чуть светлее обычной вторичной: три значения в
        // мелком шрифте иначе приходится разглядывать.
        ok = ok && createBrush(D2D1::ColorF(0.86f, 0.86f, 0.90f, 0.98f), m_brushTextBright);
        ok = ok && createBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, trackAlpha), m_brushBarTrack);
    } else {
        const float borderAlpha = glass ? 0.12f : 0.07f;
        const float trackAlpha = glass ? 0.13f : 0.10f;
        ok = ok && createBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), m_brushHover);
        ok = ok && createBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, borderAlpha), m_brushBorder);
        ok = ok && createBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 1.0f), m_brushTextPrimary);
        ok = ok && createBrush(D2D1::ColorF(0.33f, 0.33f, 0.36f, 0.95f), m_brushTextSecondary);
        ok = ok && createBrush(D2D1::ColorF(0.20f, 0.20f, 0.23f, 0.98f), m_brushTextBright);
        ok = ok && createBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, trackAlpha), m_brushBarTrack);
    }
    ok = ok && createBrush(D2D1::ColorF(0.42f, 0.80f, 0.37f, 1.0f), m_brushBarFill);
    ok = ok && createBrush(D2D1::ColorF(1.0f, 0.30f, 0.30f, 0.0f), m_brushGlow);

    if (!ok) {
        DiscardDeviceResources();
        return;
    }

    m_theme = theme;
    m_darkAppearance = dark;
}
