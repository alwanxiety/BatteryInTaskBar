#pragma once
#include <string>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <shared_mutex>
#include <mutex>
#include <utility>
#include "core/PollTrace.h"

// Убираем windows.h - AppState не использует Windows API напрямую
struct DeviceTelemetry {
    // Пустое имя — модель неизвестна; подпись подставляет UI на языке системы.
    std::wstring deviceName;
    int batteryLevel = -1;
    bool isCharging = false;
    int estimatedMinutes = -1;
    int voltageMv = -1;      // -1 = напряжение неизвестно (устройство его не сообщает)
    // Был ли хотя бы один успешный опрос. Отличает «данных ещё нет» от «устройство уснуло»:
    // во втором случае показываем последний известный заряд, а не «СПИТ».
    bool hasData = false;

    bool operator==(const DeviceTelemetry&) const = default;
};

// Заряд AirPods приходит из BLE-рекламы Apple: отдельные значения для левого,
// правого наушника и кейса. -1 — компонент неизвестен (в кейсе, не надет, старая
// прошивка): нельзя подставлять вместо него среднее или чужой процент.
struct AirPodsTelemetry {
    std::wstring deviceName;
    int leftLevel = -1;
    int rightLevel = -1;
    int caseLevel = -1;

    bool leftCharging = false;
    bool rightCharging = false;
    bool caseCharging = false;

    bool connected = false;  // реклама устройства видна прямо сейчас (или недавно)
    // true — состояние подключения известно (Bluetooth-вотчер поднялся или
    // ушёл в запасной режим). Нужно, чтобы «скрыть виджет» не снималось
    // раньше, чем мы реально узнали об отключении.
    bool connectionKnown = false;
    bool hasData = false;    // хотя бы один раз удалось разобрать заряд
    std::uint64_t lastSeenUnixMs = 0;   // последняя реклама AirPods
    std::uint64_t lastSuccessUnixMs = 0;  // последняя реклама с валидным зарядом

    [[nodiscard]] bool HasAnyLevel() const {
        return leftLevel >= 0 || rightLevel >= 0 || caseLevel >= 0;
    }

    /// Совпадает ли всё, что реально видно в плитке и меню. Метки времени
    /// меняются на каждой рекламе (~раз в секунду), но перерисовывать из-за
    /// них плитку незачем.
    [[nodiscard]] bool SameDisplayed(const AirPodsTelemetry& other) const {
        return deviceName == other.deviceName &&
               leftLevel == other.leftLevel &&
               rightLevel == other.rightLevel &&
               caseLevel == other.caseLevel &&
               leftCharging == other.leftCharging &&
               rightCharging == other.rightCharging &&
               caseCharging == other.caseCharging &&
               connected == other.connected &&
               hasData == other.hasData;
    }

    bool operator==(const AirPodsTelemetry&) const = default;
};

class AppState {
public:
    static AppState& Instance() {
        static AppState s_instance;
        return s_instance;
    }

    /// @return true, если снимок изменился (значит, UI нужно перерисовать)
    bool UpdateTelemetry(const DeviceTelemetry& data) {
        std::unique_lock lock(m_mutex);
        if (m_telemetry == data) return false;
        m_telemetry = data;
        return true;
    }

    [[nodiscard]] DeviceTelemetry GetTelemetry() const {
        std::shared_lock lock(m_mutex);
        return m_telemetry;
    }

    /// Восстановить показания из кэша вместе с исходной меткой времени.
    /// Возраст в меню будет честным, а не «только что».
    void RestoreTelemetry(const DeviceTelemetry& data, std::uint64_t lastSuccessUnixMs) {
        std::unique_lock lock(m_mutex);
        m_telemetry = data;
        m_lastSuccessUnixMs = lastSuccessUnixMs;
    }

    void RequestRescan() noexcept {
        m_forceRescan.store(true, std::memory_order_release);
    }

    /// Полноэкранное приложение на переднем плане: виджет скрыт, поэтому HID можно
    /// не трогать вовсе — ни задержек от опроса, ни лишних пробуждений устройства.
    void SetFullscreenAppActive(bool active) noexcept {
        m_fullscreenApp.store(active, std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsFullscreenAppActive() const noexcept {
        return m_fullscreenApp.load(std::memory_order_relaxed);
    }

    /// Отметить успешный опрос устройства. Время хранится отдельно от телеметрии,
    /// чтобы обновление метки не считалось изменением данных (иначе плитка
    /// перерисовывалась бы на каждом опросе).
    /// @return момент опроса в мс от epoch.
    std::uint64_t MarkPollSuccess() {
        const std::uint64_t now = NowUnixMs();
        std::unique_lock lock(m_mutex);
        m_lastSuccessUnixMs = now;
        return now;
    }

    /// Момент последнего успешного опроса (мс от epoch), 0 — данных ещё не было.
    [[nodiscard]] std::uint64_t LastSuccessUnixMs() const {
        std::shared_lock lock(m_mutex);
        return m_lastSuccessUnixMs;
    }

    /// Секунд с последнего успешного опроса; -1 — данных ещё не было.
    [[nodiscard]] std::int64_t SecondsSinceLastSuccess() const {
        std::shared_lock lock(m_mutex);
        if (m_lastSuccessUnixMs == 0) return -1;
        const std::uint64_t now = NowUnixMs();
        return (now > m_lastSuccessUnixMs)
                   ? static_cast<std::int64_t>((now - m_lastSuccessUnixMs) / 1000)
                   : 0;
    }

    [[nodiscard]] bool CheckAndResetRescan() noexcept {
        return m_forceRescan.exchange(false, std::memory_order_acq_rel);
    }

    /// Сохранить подробности последнего цикла опроса (для отчёта диагностики).
    void SetPollTrace(PollTrace trace) {
        trace.finishedUnixMs = NowUnixMs();
        std::unique_lock lock(m_mutex);
        m_pollTrace = std::move(trace);
    }

    [[nodiscard]] PollTrace GetPollTrace() const {
        std::shared_lock lock(m_mutex);
        return m_pollTrace;
    }

    /// @return true, если снимок AirPods изменился (нужно перерисовать вторую плитку)
    bool UpdateAirPods(const AirPodsTelemetry& data) {
        std::unique_lock lock(m_mutex);
        if (m_airPods == data) return false;
        m_airPods = data;
        return true;
    }

    [[nodiscard]] AirPodsTelemetry GetAirPods() const {
        std::shared_lock lock(m_mutex);
        return m_airPods;
    }

    /// Секунд с последнего разобранного заряда AirPods; -1 — данных ещё не было.
    [[nodiscard]] std::int64_t SecondsSinceAirPodsSuccess() const {
        std::shared_lock lock(m_mutex);
        if (m_airPods.lastSuccessUnixMs == 0) return -1;
        const std::uint64_t now = NowUnixMs();
        return (now > m_airPods.lastSuccessUnixMs)
                   ? static_cast<std::int64_t>((now - m_airPods.lastSuccessUnixMs) / 1000)
                   : 0;
    }

    /// Показывать ли вторую плитку. Настройка живёт в реестре, но монитор
    /// Bluetooth должен видеть её без чтения реестра на каждом цикле.
    void SetAirPodsEnabled(bool enabled) noexcept {
        m_airPodsEnabled.store(enabled, std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsAirPodsEnabled() const noexcept {
        return m_airPodsEnabled.load(std::memory_order_relaxed);
    }

    /// Немедленная проверка Bluetooth: подключение наушников, открытие меню,
    /// ручное обновление. Обычный скан идёт по своему расписанию.
    void RequestAirPodsScan() noexcept {
        m_airPodsScanRequested.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool CheckAndResetAirPodsScan() noexcept {
        return m_airPodsScanRequested.exchange(false, std::memory_order_acq_rel);
    }

private:
    AppState() = default;

    static std::uint64_t NowUnixMs() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    mutable std::shared_mutex m_mutex;
    DeviceTelemetry m_telemetry;
    AirPodsTelemetry m_airPods;
    std::uint64_t m_lastSuccessUnixMs{ 0 };
    PollTrace m_pollTrace;
    std::atomic<bool> m_forceRescan{ false };
    std::atomic<bool> m_fullscreenApp{ false };
    std::atomic<bool> m_airPodsEnabled{ false };
    std::atomic<bool> m_airPodsScanRequested{ false };
};
