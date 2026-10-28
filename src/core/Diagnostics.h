#pragma once
#include <string>

// Журнал для разбора проблем у пользователя:
// %LOCALAPPDATA%\BatteryInTaskBar\widget.log
// Пишем только ключевые события (старт, найденные HID-интерфейсы, смена темы),
// файл ротируется, чтобы не расти бесконечно.
namespace Diag {

void Init();

/// printf-подобное форматирование, %ls — для std::wstring::c_str().
void Log(const wchar_t* format, ...);

[[nodiscard]] std::wstring LogPath();

}  // namespace Diag
