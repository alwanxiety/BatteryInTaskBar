#pragma once
#include <windows.h>
#include <string>
#include "core/AppState.h"

// Единый файл диагностики: версия, окружение, настройки, текущие показания,
// HID-дамп и хвост журнала. Обычный текст — можно открыть и переслать.
namespace Report {

/// Снимок состояния на момент сбора отчёта.
struct Snapshot {
    DeviceTelemetry telemetry;
    AirPodsTelemetry airPods;
    bool airPodsEnabled = false;
    int theme = 0;
    bool systemDark = false;
    bool fullscreenApp = false;          // опрос HID при этом приостановлен
    std::int64_t secondsSincePoll = -1;  // -1 — успешных опросов ещё не было
    std::uint64_t lastPollUnixMs = 0;
    PollTrace pollTrace;                 // что пробовали в последнем цикле
};

/// @param widget — окно виджета (для DPI и координат), может быть nullptr.
/// @return полный путь к файлу или пустая строка, если записать не удалось.
[[nodiscard]] std::wstring Write(HWND widget, const Snapshot& snapshot);

}  // namespace Report
