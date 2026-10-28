#pragma once
#include <string>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "core/AppState.h"

// Форвард-декларация
struct HWND__;
typedef HWND__* HWND;

using Microsoft::WRL::ComPtr;

// Оформление плитки.
//   Glass — полупрозрачная «матовая» карточка: сквозь неё видна панель задач,
//           поэтому виджет сливается с ней, как элемент самой панели;
//   Dark  — почти непрозрачная тёмная карточка;
//   Light — почти непрозрачная светлая карточка.
enum class WidgetTheme : int {
    Glass = 0,
    Dark = 1,
    Light = 2,
};

class Renderer {
public:
    Renderer() = default;

    bool Initialize();
    void DiscardDeviceResources();

    void Render(HWND hwnd, const DeviceTelemetry& telemetry, WidgetTheme theme, bool systemDark,
                float hoverProgress, float animPhase);
    void RenderHeadset(HWND hwnd, const AirPodsTelemetry& airPods, WidgetTheme theme, bool systemDark,
                       float hoverProgress, float animPhase);

private:
    void CreateDeviceResources(HWND hwnd, WidgetTheme theme, bool systemDark);
    void DrawMouseCard(const D2D1_RECT_F& card, const DeviceTelemetry& telemetry, float hover,
                       float pulseAlpha, float scale, bool dark);
    void DrawHeadsetCard(const D2D1_RECT_F& card, const AirPodsTelemetry& airPods, float hover,
                         float pulseAlpha, float scale, bool dark);

    WidgetTheme m_theme{ WidgetTheme::Glass };
    bool m_darkAppearance{ true };  // итоговая «темнота» с учётом системной темы для Glass

    ComPtr<ID2D1Factory> m_d2dFactory;
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    ComPtr<IDWriteTextFormat> m_textFormatDevice;
    ComPtr<IDWriteTextFormat> m_textFormatPercent;
    ComPtr<IDWriteTextFormat> m_textFormatSub;
    ComPtr<IDWriteTextFormat> m_textFormatHeadsetTitle;
    ComPtr<ID2D1GradientStopCollection> m_bgStops;
    ComPtr<ID2D1LinearGradientBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushHover;
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushTextPrimary;
    ComPtr<ID2D1SolidColorBrush> m_brushTextSecondary;
    ComPtr<ID2D1SolidColorBrush> m_brushTextBright;
    ComPtr<ID2D1SolidColorBrush> m_brushBarTrack;
    ComPtr<ID2D1SolidColorBrush> m_brushBarFill;
    ComPtr<ID2D1SolidColorBrush> m_brushGlow;
};
