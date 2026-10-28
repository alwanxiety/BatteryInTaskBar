#pragma once
#include <string>

// Отчёт диагностики HID: перечисляет все HID-интерфейсы с VID/PID, именами,
// usage и длинами отчётов, прогоняет провайдеров и Bluetooth-батарею.
// Нужен, чтобы добавлять поддержку новых устройств по присланному файлу.
namespace HidDump {

/// Дописывает секцию HID (интерфейсы + Bluetooth) к тексту отчёта.
void AppendTo(std::wstring& out);

/// Отдельный файл только с HID-диагностикой (режим `--dump-hid`).
/// @return полный путь к файлу или пустая строка, если записать не удалось.
[[nodiscard]] std::wstring WriteReport();

}  // namespace HidDump
