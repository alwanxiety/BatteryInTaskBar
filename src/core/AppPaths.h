#pragma once
#include <string>

// Пути данных приложения. Пишем в %LOCALAPPDATA%\BatteryInTaskBar: журнал и
// отчёты диагностики не должны появляться рядом с DLL в папке Explorer.
namespace AppPaths {

/// Каталог данных (создаётся при первом обращении). Может быть пустым,
/// если систему не удалось спросить о %LOCALAPPDATA% и %TEMP%.
[[nodiscard]] const std::wstring& DataDirectory();

/// Полный путь к файлу внутри каталога данных.
[[nodiscard]] std::wstring DataFile(const wchar_t* name);

/// Записать текст в UTF-8 с BOM (Блокнот и «Отправить» не ломают кириллицу).
/// @return false, если файл создать/записать не удалось.
bool WriteUtf8File(const std::wstring& path, const std::wstring& text);

}  // namespace AppPaths
