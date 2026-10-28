#include "Application.h"
#include "airpods/AirPodsMonitor.h"
#include "core/AppState.h"
#include "core/DeviceManager.h"
#include "core/Diagnostics.h"
#include "core/TelemetryCache.h"

namespace {
// Период опроса устройства: 40 шагов по 100 мс с проверкой события остановки/пересканирования.
constexpr int kPollDelayStepMs = 100;
constexpr int kPollDelaySteps = 40;
// Мышь спит — незачем долбить её каждые 4 с: один такой цикл стоит ~4.5 с
// таймаутов по молчащим интерфейсам. После трёх неудач подряд ждём 30 с.
constexpr int kPollIdleSteps = 300;
constexpr int kFailuresBeforeSlow = 3;

/// Прервать текущий цикл, если на передний план вышло полноэкранное приложение.
bool FullscreenPauseRequested() noexcept {
    return AppState::Instance().IsFullscreenAppActive();
}
}

Application::Application(HMODULE hModule) : m_window(hModule) {}

void Application::Run() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) return;

    if (!m_window.Create()) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
        return;
    }

    AirPodsMonitor::Instance().Start();

    m_pollingThread = CreateThread(nullptr, 0, StaticPollingThread, this, 0, nullptr);
    if (!m_pollingThread) {
        Stop();
        return;
    }

    MSG msg;
    BOOL result = 0;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Stop();
}

void Application::Stop() {
    if (m_stopEvent) SetEvent(m_stopEvent);

    AirPodsMonitor::Instance().Stop();

    if (m_pollingThread) {
        WaitForSingleObject(m_pollingThread, INFINITE);
        CloseHandle(m_pollingThread);
        m_pollingThread = nullptr;
    }

    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }

    TelemetryCache::SaveFromState();
    m_window.Destroy();
}

DWORD WINAPI Application::StaticPollingThread(LPVOID param) {
    return static_cast<Application*>(param)->PollingWorker();
}

DWORD Application::PollingWorker() {
    DeviceManager manager;
    bool pendingRescan = false;
    bool paused = false;
    int failures = 0;
    ULONGLONG lastCacheSave = 0;
    DeviceTelemetry lastCached = AppState::Instance().GetTelemetry();

    while (WaitForSingleObject(m_stopEvent, 0) == WAIT_TIMEOUT) {
        // Кэш пишем не чаще раза в 5 минут и сразу при изменении данных:
        // после рестарта Explorer плитка не должна показывать «поиск».
        const ULONGLONG cacheTick = GetTickCount64();
        if (lastCacheSave == 0 || cacheTick - lastCacheSave >= 5 * 60 * 1000) {
            TelemetryCache::SaveFromState();
            lastCacheSave = cacheTick;
        }
        // Пока на переднем плане полноэкранное приложение (игра, кино), виджет скрыт,
        // поэтому HID не опрашиваем совсем: ни задержек от запросов, ни лишних
        // пробуждений устройства. Выход из режима ловится по 100-мс шагам ниже,
        // так что после игры данные обновляются сразу, а не через период.
        const bool fullscreen = AppState::Instance().IsFullscreenAppActive();
        if (fullscreen != paused) {
            paused = fullscreen;
            if (fullscreen) Diag::Log(L"telemetry: polling paused (fullscreen app on foreground)");
            else Diag::Log(L"telemetry: polling resumed (fullscreen app closed)");
        }

        if (!fullscreen) {
            const bool forceRescan = pendingRescan || AppState::Instance().CheckAndResetRescan();
            pendingRescan = false;

            // Неудачный опрос — это норма: беспроводная мышь засыпает после 10–15 с простоя.
            // Последние известные показатели не сбрасываем, иначе вместо заряда появлялось бы «СПИТ».
            DeviceTelemetry telemetry;
            const bool ok = manager.Poll(telemetry, forceRescan, FullscreenPauseRequested);
            failures = ok ? 0 : failures + 1;
            // Что именно пробовали и кто ответил — пригодится в отчёте.
            AppState::Instance().SetPollTrace(manager.LastTrace());

            if (ok) {
                const std::uint64_t previous = AppState::Instance().LastSuccessUnixMs();
                const std::uint64_t now = AppState::Instance().MarkPollSuccess();

                // В журнал — только первое пробуждение после старта и возврат данных после
                // долгой паузы: построчная запись каждого опроса раздула бы файл.
                if (previous == 0) {
                    Diag::Log(L"telemetry: first successful poll, device=%ls, level=%d%%",
                              telemetry.deviceName.empty() ? L"(unknown)" : telemetry.deviceName.c_str(),
                              telemetry.batteryLevel);
                } else if (now > previous && (now - previous) / 1000 >= 60) {
                    Diag::Log(L"telemetry: data resumed after %llu s",
                              static_cast<unsigned long long>((now - previous) / 1000));
                }

                if (AppState::Instance().UpdateTelemetry(telemetry)) {
                    // PostMessage потокобезопасен; окно живёт дольше этого потока (см. Stop).
                    m_window.NotifyTelemetryChanged();
                }
                if (!(telemetry == lastCached)) {
                    TelemetryCache::SaveFromState();
                    lastCached = telemetry;
                    lastCacheSave = GetTickCount64();
                }
            }
        }
        // Запросы на пересканирование во время паузы не теряем: флаг остаётся
        // выставленным и обрабатывается первым же опросом после выхода из игры.

        // Пока мышь спит, каждый цикл — это ~4.5 с таймаутов по молчащим интерфейсам.
        // После трёх неудач подряд переходим на редкий период; движение курсора
        // считается признаком «мышь проснулась» и возвращает опрос сразу.
        const bool slow = failures >= kFailuresBeforeSlow;
        const int steps = slow ? kPollIdleSteps : kPollDelaySteps;
        POINT waitCursor{};
        GetCursorPos(&waitCursor);

        for (int i = 0; i < steps; ++i) {
            if (WaitForSingleObject(m_stopEvent, kPollDelayStepMs) != WAIT_TIMEOUT) return 0;
            if (AppState::Instance().CheckAndResetRescan()) {
                // Устройство подключилось/отключилось — не ждём конца периода.
                pendingRescan = true;
                break;
            }
            // Режим сменился (игра открылась или закрылась) — не досыпаем остаток
            // периода: пауза должна начаться сразу, а данные — обновиться сразу.
            if (AppState::Instance().IsFullscreenAppActive() != fullscreen) break;
            if (slow) {
                POINT cursor{};
                if (GetCursorPos(&cursor) && (cursor.x != waitCursor.x || cursor.y != waitCursor.y)) break;
            }
        }
    }
    return 0;
}
