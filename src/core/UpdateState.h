#pragma once
#include <cstdint>
#include <string>

// Результат фоновой проверки обновлений. Пишет updater.exe, читает виджет.
// Отдельная ветка реестра, чтобы не смешивать с настройками и кэшем телеметрии.
namespace UpdateState {

struct Info {
    bool available = false;
    std::wstring latest;
    std::wstring error;
    std::uint64_t checkedAtUnixMs = 0;
};

[[nodiscard]] Info Read();
void Write(const Info& info);

}  // namespace UpdateState
