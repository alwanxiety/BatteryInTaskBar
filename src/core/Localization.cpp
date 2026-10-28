#include "core/Localization.h"
#include <windows.h>
#include <cwchar>
#include <iterator>

namespace {

enum class Lang { Ru, En };

struct Entry {
    const wchar_t* ru;
    const wchar_t* en;
};

// Порядок записей обязан совпадать с enum Str.
constexpr Entry kStrings[] = {
    /* DeviceFallback   */ { L"Беспроводная мышь", L"Wireless mouse" },
    /* DeviceGeneric    */ { L"Беспроводное устройство", L"Wireless device" },
    /* SearchDevice     */ { L"поиск устройства…", L"searching for device…" },
    /* Battery          */ { L"Заряд: ", L"Battery: " },
    /* NoData           */ { L"нет данных", L"no data" },
    /* ChargingSuffix   */ { L" (заряжается)", L" (charging)" },
    /* Voltage          */ { L"Напряжение: ~", L"Voltage: ~" },
    /* EstimatedTime    */ { L"Осталось: ", L"Time left: " },
    /* EstimatedToFull  */ { L"До полного заряда: ", L"Time to full: " },
    /* Theme            */ { L"Тема", L"Theme" },
    /* ThemeGlass       */ { L"Матовая", L"Matte" },
    /* ThemeDark        */ { L"Тёмная", L"Dark" },
    /* ThemeLight       */ { L"Светлая", L"Light" },
    /* RefreshNow       */ { L"Обновить статус сейчас", L"Refresh status now" },
    /* StartWithWindows */ { L"Запускать при старте системы", L"Start with Windows" },
    /* CloseWidget      */ { L"Закрыть виджет", L"Close widget" },
    /* SubSearching     */ { L"Поиск устройства…", L"Searching for device…" },
    /* SubConnected     */ { L"Подключено", L"Connected" },
    /* SubCharging      */ { L"⚡ Зарядка", L"⚡ Charging" },
    /* HoursShort       */ { L"ч", L"h" },
    /* MinutesShort     */ { L"м", L"m" },
    /* LeftPrefix       */ { L"Осталось ", L"" },
    /* LeftSuffix       */ { L"", L" left" },
    /* DumpTitle        */ { L"Диагностика HID", L"HID diagnostics" },
    /* DumpSaved        */ { L"Файл диагностики сохранён:\n%ls\n\nОтправьте его разработчику.",
                            L"Diagnostics file saved to:\n%ls\n\nPlease send it to the developer." },
    /* DumpFailed       */ { L"Не удалось создать файл диагностики.", L"Could not create the diagnostics file." },
    /* ReportTitle      */ { L"Диагностика BatteryInTaskBar", L"BatteryInTaskBar diagnostics" },
    /* ReportSaved      */ { L"Отчёт сохранён:\n%ls\n\nОтправьте его разработчику.",
                            L"Report saved to:\n%ls\n\nPlease send it to the developer." },
    /* ReportFailed     */ { L"Не удалось создать файл отчёта.", L"Could not create the report file." },
    /* AgoJustNow       */ { L"только что", L"just now" },
    /* AgoMinuteOne     */ { L"%d минуту назад", L"%d minute ago" },
    /* AgoMinuteFew     */ { L"%d минуты назад", L"%d minutes ago" },
    /* AgoMinuteMany    */ { L"%d минут назад", L"%d minutes ago" },
    /* AgoHourOne       */ { L"%d час назад", L"%d hour ago" },
    /* AgoHourFew       */ { L"%d часа назад", L"%d hours ago" },
    /* AgoHourMany      */ { L"%d часов назад", L"%d hours ago" },
    /* AirPodsLeftShort */ { L"Л", L"L" },
    /* AirPodsRightShort*/ { L"П", L"R" },
    /* AirPodsCaseShort */ { L"К", L"C" },
    /* SubDisconnected  */ { L"нет связи", L"disconnected" },
    /* HeadsetName      */ { L"Наушники", L"Headphones" },
    /* HideWidget       */ { L"Скрыть виджет", L"Hide widget" },
};

static_assert(std::size(kStrings) == static_cast<std::size_t>(Str::HideWidget) + 1,
              "таблица строк разошлась с enum Str");

Lang g_lang = Lang::En;

Lang LanguageFromCode(const wchar_t* code) {
    if (!code || !*code) return Lang::En;
    if (_wcsicmp(code, L"ru") == 0 || _wcsicmp(code, L"rus") == 0) return Lang::Ru;
    return Lang::En;  // английский — безопасное значение по умолчанию
}

bool ReadOverride(std::wstring& outCode) {
    wchar_t env[32]{};
    const DWORD envLength = GetEnvironmentVariableW(L"BIT_LANG", env, 32);
    if (envLength > 0 && envLength < std::size(env)) {
        outCode.assign(env, envLength);
        return true;
    }

    wchar_t value[32]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BatteryInTaskBar", L"Lang",
                     RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
        outCode.assign(value);
        return !outCode.empty();
    }
    return false;
}

Lang DetectLanguage() {
    std::wstring overrideCode;
    if (ReadOverride(overrideCode)) return LanguageFromCode(overrideCode.c_str());

    // Основной путь: язык интерфейса Windows, а не региональные форматы.
    const LANGID uiLanguage = GetUserDefaultUILanguage();
    return PRIMARYLANGID(uiLanguage) == LANG_RUSSIAN ? Lang::Ru : Lang::En;
}

const Entry& EntryOf(Str id) {
    const std::size_t index = static_cast<std::size_t>(id);
    if (index >= std::size(kStrings)) return kStrings[0];
    return kStrings[index];
}

/// Подставляет число в шаблон с правильной формой слова для текущего языка.
std::wstring FormatCount(int count, Str one, Str few, Str many) {
    const wchar_t* pattern = nullptr;
    if (g_lang == Lang::Ru) {
        const int tail100 = count % 100;
        const int tail10 = count % 10;
        if (tail100 >= 11 && tail100 <= 14) {
            pattern = L10n::Get(many);
        } else if (tail10 == 1) {
            pattern = L10n::Get(one);
        } else if (tail10 >= 2 && tail10 <= 4) {
            pattern = L10n::Get(few);
        } else {
            pattern = L10n::Get(many);
        }
    } else {
        pattern = L10n::Get(count == 1 ? one : many);
    }
    wchar_t buffer[64]{};
    swprintf_s(buffer, pattern, count);
    return buffer;
}

std::wstring ShortDuration(int minutes) {
    std::wstring text = L"~";
    const int hours = minutes / 60;
    if (hours > 0) {
        text += std::to_wstring(hours);
        text += L10n::Get(Str::HoursShort);
        const int rest = minutes % 60;
        if (rest > 0) {
            text += L" ";
            text += std::to_wstring(rest);
            text += L10n::Get(Str::MinutesShort);
        }
    } else {
        text += std::to_wstring(minutes % 60);
        text += L10n::Get(Str::MinutesShort);
    }
    return text;
}

}  // namespace

void L10n::Init() {
    g_lang = DetectLanguage();
}

const wchar_t* L10n::Get(Str id) {
    const Entry& entry = EntryOf(id);
    return (g_lang == Lang::Ru) ? entry.ru : entry.en;
}

bool L10n::IsRussian() {
    return g_lang == Lang::Ru;
}

const wchar_t* L10n::LanguageCode() {
    return (g_lang == Lang::Ru) ? L"ru" : L"en";
}

std::wstring L10n::Remaining(int minutes) {
    if (minutes <= 0) return {};
    return ShortDuration(minutes);
}

std::wstring L10n::RemainingShort(int minutes) {
    if (minutes <= 0) return {};
    return std::wstring(Get(Str::LeftPrefix)) + ShortDuration(minutes) + Get(Str::LeftSuffix);
}

std::wstring L10n::ChargingRemaining(int minutes) {
    std::wstring text = Get(Str::SubCharging);
    if (minutes > 0) {
        text += L" • ";
        text += ShortDuration(minutes);
    }
    return text;
}

std::wstring L10n::Ago(std::int64_t seconds) {
    if (seconds < 0) return {};
    const int minutes = static_cast<int>(seconds / 60);
    if (minutes < 1) return Get(Str::AgoJustNow);
    if (minutes < 60) {
        return FormatCount(minutes, Str::AgoMinuteOne, Str::AgoMinuteFew, Str::AgoMinuteMany);
    }
    return FormatCount(minutes / 60, Str::AgoHourOne, Str::AgoHourFew, Str::AgoHourMany);
}
