#pragma once
#include "core/AppState.h"
#include "hid/MouseProviders.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class DeviceManager {
public:
    DeviceManager();

    /// @param forceRescan — перечислить HID-устройства заново (подключение/отключение устройства)
    /// @param shouldAbort — необязательная проверка «опрос больше не нужен» (например, игра
    ///        ушла в полноэкранный режим); вызывается между попытками, чтобы не ждать
    ///        все таймауты молчащих интерфейсов.
    bool Poll(DeviceTelemetry& outTelemetry, bool forceRescan,
              bool (*shouldAbort)() noexcept = nullptr);

    /// Подробности последнего цикла (для отчёта). Читает только поток опроса.
    [[nodiscard]] const PollTrace& LastTrace() const noexcept { return m_lastTrace; }

private:
    struct Candidate {
        std::wstring path;
        IMouseProvider* provider = nullptr;
        std::size_t priority = 0;
    };

    bool EnumerateCandidates();
    bool QueryCandidate(const Candidate& candidate, DeviceTelemetry& outTelemetry,
                        PollAttempt::Failure& failure);
    static bool QueryBluetooth(DeviceTelemetry& outTelemetry);

    LogitechProvider m_logitech;
    RazerProvider m_razer;
    RedSquareProvider m_redSquare;
    CompxNordicProvider m_compx;
    GenericBatteryProvider m_generic;
    std::array<IMouseProvider*, 5> m_providers;
    std::vector<Candidate> m_candidates;
    std::wstring m_lastGoodPath;          // интерфейс, ответивший в прошлый раз (опрашиваем первым)
    std::size_t m_lastGoodProvider = static_cast<std::size_t>(-1);
    std::uint64_t m_lastEnumerationTick = 0;
    PollTrace m_lastTrace;
};
