#include "core/DeviceManager.h"
#include "core/Diagnostics.h"
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
// Эмпирические оценки: ~95 часов автономной работы и ~2.5 часа на полный заряд.
constexpr double kMinutesPerPercentDischarging = 57.0;
constexpr double kMinutesPerPercentCharging = 1.5;

constexpr ULONGLONG kEnumerationTtlMs = 60'000;        // страховочное перечисление
constexpr ULONGLONG kEmptyEnumerationRetryMs = 5'000;  // поддерживаемых устройств не нашли

int ClampLevel(int level) { return std::clamp(level, 0, 100); }

// Имя модели не пришло от устройства — оставляем пустым, UI подставит
// локализованную подпись по языку системы.
bool HasPlaceholderName(const std::wstring& name) {
    return name.empty();
}

int EstimateMinutes(int level, bool charging) {
    if (level < 0) return -1;
    const double minutes = charging ? (100 - level) * kMinutesPerPercentCharging
                                    : level * kMinutesPerPercentDischarging;
    return static_cast<int>(std::lround(minutes));
}

bool QueryProviderSafe(IMouseProvider& provider, HANDLE hDevice, const HIDP_CAPS& caps, DeviceTelemetry& outTelemetry) noexcept {
    __try { return provider.Query(hDevice, caps, outTelemetry); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

DeviceManager::DeviceManager() : m_providers({ &m_logitech, &m_razer, &m_redSquare, &m_compx, &m_generic }) {}

bool DeviceManager::EnumerateCandidates() {
    const std::size_t previousCount = m_candidates.size();
    const bool hadEnumeration = m_lastEnumerationTick != 0;
    m_candidates.clear();
    m_lastEnumerationTick = GetTickCount64();

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA devData = { sizeof(SP_DEVICE_INTERFACE_DATA) };
    bool sawSupported = false;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &devData); ++i) {
        if (PollAbort::Requested()) break;
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &devData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) continue;

        // vector<uintptr_t> даёт выравнивание под PSP_DEVICE_INTERFACE_DETAIL_DATA_W
        // и позволяет обработать путь длиннее фиксированных 1024 байт.
        std::vector<std::uintptr_t> buffer((requiredSize + sizeof(std::uintptr_t) - 1) / sizeof(std::uintptr_t));
        auto* detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &devData, detailData, requiredSize, nullptr, nullptr)) continue;

        HANDLE hDevice = CreateFileW(detailData->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hDevice == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr = { sizeof(HIDD_ATTRIBUTES) };
        if (!HidD_GetAttributes(hDevice, &attr)) {
            CloseHandle(hDevice);
            continue;
        }

        // Описание интерфейса нужно generic-провайдеру: он цепляется не по VID/PID,
        // а по наличию поля Battery Strength (0x06/0x20) в отчётах.
        HIDP_CAPS caps{};
        bool haveCaps = false;
        PHIDP_PREPARSED_DATA preparsedData = nullptr;
        if (HidD_GetPreparsedData(hDevice, &preparsedData)) {
            haveCaps = HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS;
            HidD_FreePreparsedData(preparsedData);
        }

        // Один и тот же интерфейс может подойти нескольким провайдерам
        // (например, VXE и generic CompX): храним все варианты, Poll перебирает
        // их по приоритету, пока какой-нибудь не ответит.
        for (std::size_t p = 0; p < m_providers.size(); ++p) {
            if (!m_providers[p]->Match(attr.VendorID, attr.ProductID)) continue;
            if (haveCaps && !m_providers[p]->MatchDevice(hDevice, caps)) continue;
            m_candidates.push_back({ detailData->DevicePath, m_providers[p], p });
            sawSupported = true;
        }
        CloseHandle(hDevice);
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    std::stable_sort(m_candidates.begin(), m_candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.priority < b.priority; });
    // В журнал — только изменения набора: перечисление идёт по таймеру, и построчный
    // лог каждого прохода быстро раздул бы файл.
    if (!hadEnumeration || m_candidates.size() != previousCount) {
        Diag::Log(L"HID: %zu supported interface(s), enumeration %ls", m_candidates.size(),
                  sawSupported ? L"ok" : L"found nothing");
    }
    return sawSupported;
}

bool DeviceManager::Poll(DeviceTelemetry& outTelemetry, bool forceRescan,
                         bool (*shouldAbort)() noexcept) {
    const ULONGLONG cycleStart = GetTickCount64();
    m_lastTrace = PollTrace{};
    m_lastTrace.valid = true;
    // Время цикла проставляется на любом выходе, включая раннее прерывание.
    struct CycleTimer {
        PollTrace& trace;
        ULONGLONG start;
        ~CycleTimer() { trace.totalMs = static_cast<int>(GetTickCount64() - start); }
    } cycleTimer{ m_lastTrace, cycleStart };
    // Провайдеры проверяют флаг PollAbort внутри своих переборов; после цикла
    // его обязательно сбрасываем, иначе следующий опрос завершится сразу.
    struct AbortReset {
        ~AbortReset() { PollAbort::Set(false); }
    } abortReset;
    const auto abortRequested = [shouldAbort] {
        return shouldAbort != nullptr && shouldAbort();
    };

    const ULONGLONG now = GetTickCount64();
    bool needEnumerate = false;
    if (forceRescan || m_lastEnumerationTick == 0) {
        needEnumerate = true;
    } else if (m_candidates.empty()) {
        needEnumerate = (now - m_lastEnumerationTick) >= kEmptyEnumerationRetryMs;
    } else {
        needEnumerate = (now - m_lastEnumerationTick) >= kEnumerationTtlMs;
    }
    if (needEnumerate) EnumerateCandidates();
    m_lastTrace.attempts.reserve(m_candidates.size() + 1);

    auto tryCandidate = [&](const Candidate& candidate) -> bool {
        if (!candidate.provider) return false;
        // Свежий флаг: провайдер прекратит перебор, если игра уже на весь экран.
        PollAbort::Set(abortRequested());

        const ULONGLONG attemptStart = GetTickCount64();
        // Свежий снимок на каждую попытку: провайдер не «загрязняет» данные следующего.
        DeviceTelemetry candidateTelemetry;
        PollAttempt::Failure failure = PollAttempt::Failure::None;
        const bool ok = QueryCandidate(candidate, candidateTelemetry, failure);
        const int elapsedMs = static_cast<int>(GetTickCount64() - attemptStart);

        PollAttempt attempt;
        attempt.path = candidate.path;
        attempt.provider = candidate.provider->Name();
        attempt.ok = ok;
        attempt.failure = failure;
        attempt.elapsedMs = elapsedMs;

        if (!ok) {
            m_lastTrace.attempts.push_back(std::move(attempt));
            return false;
        }

        outTelemetry = std::move(candidateTelemetry);
        outTelemetry.batteryLevel = ClampLevel(outTelemetry.batteryLevel);
        outTelemetry.estimatedMinutes = EstimateMinutes(outTelemetry.batteryLevel, outTelemetry.isCharging);
        outTelemetry.hasData = true;
        m_lastGoodPath = candidate.path;
        m_lastGoodProvider = candidate.priority;

        m_lastTrace.succeeded = true;
        m_lastTrace.selectedPath = candidate.path;
        m_lastTrace.selectedProvider = attempt.provider;
        m_lastTrace.selectedMs = elapsedMs;
        m_lastTrace.attempts.push_back(std::move(attempt));
        m_lastTrace.selectedAttempt = static_cast<int>(m_lastTrace.attempts.size());
        return true;
    };

    // Интерфейс и провайдер, ответившие в прошлый раз, опрашиваем первыми:
    // «молчащие» интерфейсы ресивера иначе тратят таймауты на каждом цикле.
    const bool haveLastGood = !m_lastGoodPath.empty() && m_lastGoodProvider != static_cast<std::size_t>(-1);
    const auto syncAbort = [&abortRequested] {
        const bool aborted = abortRequested();
        PollAbort::Set(aborted);
        return aborted;
    };
    const auto stop = [this] { m_lastTrace.aborted = true; PollAbort::Set(false); return false; };
    if (syncAbort()) return stop();
    if (haveLastGood) {
        for (const Candidate& candidate : m_candidates) {
            if (candidate.path == m_lastGoodPath && candidate.priority == m_lastGoodProvider && tryCandidate(candidate)) return true;
        }
    }
    for (const Candidate& candidate : m_candidates) {
        if (haveLastGood && candidate.path == m_lastGoodPath && candidate.priority == m_lastGoodProvider) continue;
        // Полноэкранная игра могла начаться прямо сейчас — не тратим ещё ~1 с на
        // следующий молчащий интерфейс.
        if (syncAbort()) return stop();
        if (tryCandidate(candidate)) return true;
    }

    if (syncAbort()) return stop();
    m_lastTrace.bluetoothTried = true;
    DeviceTelemetry bluetoothTelemetry;
    if (QueryBluetooth(bluetoothTelemetry)) {
        outTelemetry = std::move(bluetoothTelemetry);
        outTelemetry.batteryLevel = ClampLevel(outTelemetry.batteryLevel);
        outTelemetry.estimatedMinutes = EstimateMinutes(outTelemetry.batteryLevel, outTelemetry.isCharging);
        outTelemetry.hasData = true;
        m_lastTrace.bluetoothAnswered = true;
        m_lastTrace.succeeded = true;
        return true;
    }

    return false;
}

bool DeviceManager::QueryCandidate(const Candidate& candidate, DeviceTelemetry& outTelemetry,
                                   PollAttempt::Failure& failure) {
    failure = PollAttempt::Failure::NoAnswer;
    HANDLE hDevice = CreateFileW(candidate.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDevice == INVALID_HANDLE_VALUE) {
        failure = PollAttempt::Failure::Open;
        return false;
    }

    bool success = false;
    PHIDP_PREPARSED_DATA preparsedData = nullptr;
    if (!HidD_GetPreparsedData(hDevice, &preparsedData)) {
        failure = PollAttempt::Failure::NoCaps;
        CloseHandle(hDevice);
        return false;
    }

    HIDP_CAPS caps{};
    if (HidP_GetCaps(preparsedData, &caps) != HIDP_STATUS_SUCCESS) {
        failure = PollAttempt::Failure::NoCaps;
    } else if (QueryProviderSafe(*candidate.provider, hDevice, caps, outTelemetry)) {
        if (HasPlaceholderName(outTelemetry.deviceName)) {
            std::array<WCHAR, 128> productString{};
            if (HidD_GetProductString(hDevice, productString.data(),
                                      static_cast<ULONG>(productString.size() * sizeof(WCHAR)))) {
                const std::wstring product = productString.data();
                if (!LooksLikeReceiverName(product)) outTelemetry.deviceName = product;
            }
        }
        success = true;
        failure = PollAttempt::Failure::None;
    }
    HidD_FreePreparsedData(preparsedData);

    CloseHandle(hDevice);
    return success;
}

bool DeviceManager::QueryBluetooth(DeviceTelemetry& outTelemetry) {
    // Сначала HID-провайдеры, затем общий PnP-запас: Windows иногда сам знает
    // заряд устройства, даже если vendor-протокол нам неизвестен.
    return PnpBatteryProvider::Query(outTelemetry);
}
