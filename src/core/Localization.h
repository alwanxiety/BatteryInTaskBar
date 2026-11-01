#pragma once
#include <cstdint>
#include <string>

// Строки интерфейса. Язык выбирается сам по локали Windows (русский/английский),
// без настройки в меню. Для отладки можно переопределить: переменная окружения
// BIT_LANG=ru|en или значение Lang в HKCU\Software\BatteryInTaskBar.
enum class Str {
    DeviceFallback,     // «Беспроводная мышь»
    DeviceGeneric,      // «Беспроводное устройство» (запасное имя устройства)
    SearchDevice,       // «поиск устройства…» в меню
    Battery,            // «Заряд: »
    NoData,             // «нет данных»
    ChargingSuffix,     // « (заряжается)»
    Voltage,            // «Напряжение: ~»
    EstimatedTime,      // «Осталось: » (разряд)
    EstimatedToFull,    // «До полного заряда: » (зарядка)
    Theme,              // «Тема»
    ThemeGlass,         // «Матовая»
    ThemeDark,          // «Тёмная»
    ThemeLight,         // «Светлая»
    RefreshNow,         // «Обновить статус сейчас»
    StartWithWindows,   // «Запускать при старте системы»
    CloseWidget,        // «Закрыть виджет»
    SubSearching,       // «Поиск устройства…» на плитке
    SubConnected,       // «Подключено»
    SubCharging,        // «⚡ Зарядка»
    HoursShort,         // «ч»
    MinutesShort,       // «м»
    LeftPrefix,         // «Осталось » / пусто
    LeftSuffix,         // пусто / « left»
    DumpTitle,          // заголовок окна диагностики HID
    DumpSaved,          // «Файл диагностики сохранён: %ls»
    DumpFailed,         // «Не удалось создать файл диагностики.»
    ReportTitle,        // заголовок окна полного отчёта
    ReportSaved,        // «Отчёт сохранён: %ls»
    ReportFailed,       // «Не удалось создать файл отчёта.»
    AgoJustNow,         // «только что»
    AgoMinuteOne,       // «%d минуту назад»
    AgoMinuteFew,       // «%d минуты назад»
    AgoMinuteMany,      // «%d минут назад»
    AgoHourOne,         // «%d час назад»
    AgoHourFew,         // «%d часа назад»
    AgoHourMany,        // «%d часов назад»
    AirPodsLeftShort,   // «Л» / «L»
    AirPodsRightShort,  // «П» / «R»
    AirPodsCaseShort,   // «К» / «C»
    SubDisconnected,    // «нет связи»
    HeadsetName,        // «Наушники» (запасное имя второй плитки)
    HideWidget,         // «Скрыть виджет»
    InjectFailedTitle,  // заголовок ошибки запуска
    InjectFailedText,   // «Не удалось внедрить виджет в Explorer…»
    LevelUnchangedHint, // «Заряд не менялся больше суток» (залипший индикатор)
    CheckUpdates,       // «Проверить обновления» в меню
    UpdateTitle,        // заголовок окна обновления
    UpdateChecking,     // «Проверка обновлений…»
    UpdateUpToDate,     // «У вас последняя версия %ls.»
    UpdateAvailable,    // «Доступна версия %ls (у вас %ls).»
    UpdateDownloading,  // «Скачивание %ls…»
    UpdateVerifying,    // «Проверка контрольных сумм…»
    UpdateInstalling,   // «Установка обновления…»
    UpdateRestarting,   // «Перезапуск виджета…»
    UpdateDone,         // «Обновление установлено. Виджет перезапущен.»
    UpdateFailed,       // «Не удалось обновить: %ls»
    UpdateNetworkError, // «Не удалось проверить обновления…»
    UpdateNoAssets,     // «В релизе нет нужных файлов.»
    UpdateChecksumMismatch, // «Контрольная сумма не совпала: %ls»
    UpdateFileBusy,     // «Не удалось заменить %ls: файл занят.»
    UpdateWidgetLoaded, // «Виджет всё ещё загружен в Explorer…»
    UpdateNoUpdater,    // «updater.exe не найден рядом с виджетом…»
    UpdateAlreadyRunning, // «Обновление уже запущено.»
    UpdateNoWriteAccess,  // «Нет прав на запись в папку программы.»
    UpdateButton,       // «Обновить»
    UpdateLater,        // «Позже»
    UpdateClose,        // «Закрыть»
    UpdateOpenRelease,  // «Открыть страницу релиза»
};

namespace L10n {

void Init();

[[nodiscard]] const wchar_t* Get(Str id);

[[nodiscard]] bool IsRussian();

/// "ru" / "en" — для журнала и отчётов.
[[nodiscard]] const wchar_t* LanguageCode();

/// "ru-RU" / "en-US" — культура для DirectWrite (а не только язык строк).
[[nodiscard]] const wchar_t* LocaleName();

/// "~8ч 24м" / "~8h 24m"
[[nodiscard]] std::wstring Remaining(int minutes);

/// "Осталось ~13ч" / "~13h left"
[[nodiscard]] std::wstring RemainingShort(int minutes);

/// "⚡ Зарядка • ~824м" / "⚡ Charging • ~824m"
[[nodiscard]] std::wstring ChargingRemaining(int minutes);

/// "только что" / "2 минуты назад" / "3 часа назад"; seconds < 0 — пустая строка.
[[nodiscard]] std::wstring Ago(std::int64_t seconds);

}  // namespace L10n
