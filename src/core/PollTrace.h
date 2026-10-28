#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Подробности последнего цикла опроса: какие интерфейсы пробовали, кто ответил,
// сколько это заняло. HID-дамп показывает все устройства системы, а трейс — то,
// что реально выбрал сам виджет; вместе они объясняют «данных нет, а устройство есть».
struct PollAttempt {
    enum class Failure : std::uint8_t { None, Open, NoCaps, NoAnswer };
    std::wstring path;
    std::wstring provider;
    bool ok = false;
    Failure failure = Failure::None;
    int elapsedMs = 0;
};

struct PollTrace {
    std::vector<PollAttempt> attempts;
    std::wstring selectedPath;
    std::wstring selectedProvider;
    int selectedMs = 0;
    int selectedAttempt = 0;  // 1-based номер удачной попытки, 0 — никто не ответил
    int totalMs = 0;
    bool bluetoothTried = false;
    bool bluetoothAnswered = false;
    bool aborted = false;     // цикл прерван (например, игра ушла в полный экран)
    bool succeeded = false;
    std::uint64_t finishedUnixMs = 0;
    bool valid = false;       // был ли опрос вообще
};
