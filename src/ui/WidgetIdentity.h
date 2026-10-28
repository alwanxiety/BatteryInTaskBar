#pragma once

// Идентификация окна виджета для внешних процессов (инжектор, сторож).
// ВАЖНО: класс окна уникален для каждой загрузки DLL (см. WidgetWindow::RegisterWindowClass),
// поэтому снаружи окно ищется по заголовку, а не по имени класса.

inline constexpr wchar_t kWidgetWindowTitle[] = L"BatteryInTaskBar.Widget";

// Префикс имени класса. Полное имя = префикс + адрес модуля + тик загрузки.
inline constexpr wchar_t kWidgetClassNamePrefix[] = L"BatteryTaskbarWidgetBandOverlay";

// Имя класса у версий до появления уникальных имён. Такие окна остались без заголовка,
// поэтому их можно найти только по классу — используется при обновлении поверх старой версии.
inline constexpr wchar_t kLegacyWidgetClassName[] = L"BatteryTaskbarWidgetBandOverlay";
