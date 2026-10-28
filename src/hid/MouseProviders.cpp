#include "hid/MouseProviders.h"
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

// Исправление конфликта макросов min/max из windows.h
#undef min
#undef max

namespace {
std::atomic<bool> g_pollAbort{ false };

std::wstring ToLowerCopy(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

bool ContainsNoCase(const std::wstring& haystack, const wchar_t* needle) {
    if (!needle || !*needle) return true;
    return ToLowerCopy(haystack).find(ToLowerCopy(needle)) != std::wstring::npos;
}

// Bluetooth-узел с «батарейкой» может оказаться наушниками, а не мышью.
bool LooksLikeAudioDevice(const std::wstring& name) {
    static constexpr const wchar_t* kAudioHints[] = {
        L"headphone", L"headset", L"audio", L"buds", L"speaker", L"sound",
        L"наушник", L"гарнитур", L"колонк", L"динамик"
    };
    for (const wchar_t* hint : kAudioHints) {
        if (ContainsNoCase(name, hint)) return true;
    }
    return false;
}

// Устройства, которые тоже умеют отдавать заряд, но мышью не являются.
// Иначе заряд наушников/телефона/клавиатуры мог бы попасть в плитку мыши.
bool LooksLikeNonMouse(const std::wstring& name) {
    static constexpr const wchar_t* kOtherHints[] = {
        L"keyboard", L"клавиатур", L"iphone", L"ipad", L"phone", L"телефон",
        L"tablet", L"планшет", L"smartwatch", L"watch", L"часы",
        L"airpods", L"beats", L"headphone", L"headset", L"buds", L"наушник",
        L"gamepad", L"joystick", L"controller", L"геймпад", L"джойстик",
        L"presenter", L"stylus", L"презентер", L"стилус"
    };
    for (const wchar_t* hint : kOtherHints) {
        if (ContainsNoCase(name, hint)) return true;
    }
    return false;
}

// Мышь важнее безымянного ресивера, ресивер — важнее прочей периферии.
int MouseNameScore(const std::wstring& name) {
    if (ContainsNoCase(name, L"mouse") || ContainsNoCase(name, L"мышь") ||
        ContainsNoCase(name, L"мыш")) {
        return 3;
    }
    if (ContainsNoCase(name, L"receiver") || ContainsNoCase(name, L"2.4g") ||
        ContainsNoCase(name, L"wireless") || ContainsNoCase(name, L"приемник") ||
        ContainsNoCase(name, L"приёмник") || ContainsNoCase(name, L"беспровод")) {
        return 2;
    }
    return 1;
}

constexpr DEVPROPKEY kPkeyBatteryLevel = {
    { 0x104ea319, 0x6ee2, 0x4701, { 0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5 } }, 2
};
constexpr GUID kGuidBth = {
    0xe0cbf06c, 0xcd8b, 0x4647, { 0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74 }
};
constexpr GUID kGuidHidClass = {
    0x745A17A0, 0x74D3, 0x11D0, { 0xB6, 0xFE, 0x00, 0xA0, 0xC9, 0x0F, 0x57, 0xDA }
};

// --- Battery Strength (Generic Device Controls 0x06 / 0x20) ---
constexpr USHORT kUsagePageBattery = 0x06;
constexpr USHORT kUsageBatteryStrength = 0x20;

bool FindBatteryCap(PHIDP_PREPARSED_DATA preparsed, HIDP_REPORT_TYPE type, HIDP_VALUE_CAPS& outCap) {
    USHORT count = 0;
    // Первый вызов с пустым буфером сообщает требуемое количество элементов.
    HidP_GetValueCaps(type, nullptr, &count, preparsed);
    if (count == 0) return false;

    std::vector<HIDP_VALUE_CAPS> caps(count);
    if (HidP_GetValueCaps(type, caps.data(), &count, preparsed) != HIDP_STATUS_SUCCESS) return false;
    for (USHORT i = 0; i < count; ++i) {
        // Поле идёт либо диапазоном, либо одиночным usage (Battery Strength — одиночный).
        if (!caps[i].IsRange && caps[i].UsagePage == kUsagePageBattery &&
            caps[i].NotRange.Usage == kUsageBatteryStrength) {
            outCap = caps[i];
            return true;
        }
    }
    return false;
}

bool ReadFeatureReport(HANDLE dev, const HIDP_CAPS& caps, UCHAR reportId,
                       std::vector<uint8_t>& report) {
    if (caps.FeatureReportByteLength == 0) return false;
    report.assign(caps.FeatureReportByteLength, 0);
    report[0] = reportId;
    return HidD_GetFeature(dev, report.data(), static_cast<ULONG>(report.size())) != FALSE;
}

bool ExtractBatteryPercent(PHIDP_PREPARSED_DATA preparsed, HIDP_REPORT_TYPE type,
                           const HIDP_VALUE_CAPS& cap, const std::vector<uint8_t>& report,
                           int& percent) {
    ULONG value = 0;
    if (HidP_GetUsageValue(type, cap.UsagePage, cap.LinkCollection, cap.NotRange.Usage, &value,
                           preparsed,
                           reinterpret_cast<PCHAR>(const_cast<uint8_t*>(report.data())),
                           static_cast<ULONG>(report.size())) != HIDP_STATUS_SUCCESS) {
        return false;
    }

    // По спецификации диапазон 0..100, но некоторые устройства объявляют свой.
    const LONG lo = cap.LogicalMin;
    const LONG hi = cap.LogicalMax;
    int result = static_cast<int>(value);
    if (hi > lo && (lo != 0 || hi != 100)) {
        result = static_cast<int>(std::lround((static_cast<double>(value) - lo) * 100.0 / (hi - lo)));
    }
    if (result < 0 || result > 100) return false;
    percent = result;
    return true;
}

/// Дождаться overlapped-запроса с таймаутом. По таймауту запрос отменяется, но
/// вызывающий обязан знать, что структура OVERLAPPED и буфер свободны только
/// после фактического завершения: ядро пишет результат именно туда.
bool WaitForIo(HANDLE dev, OVERLAPPED& ov, DWORD timeoutMs, DWORD& transferred) noexcept {
    if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0) {
        return GetOverlappedResult(dev, &ov, &transferred, FALSE) != FALSE;
    }
    // CancelIoEx завершает IRP, после чего GetOverlappedResult с ожиданием
    // гарантирует, что ядро больше не тронет наш OVERLAPPED.
    CancelIoEx(dev, &ov);
    DWORD ignored = 0;
    GetOverlappedResult(dev, &ov, &ignored, TRUE);
    return false;
}

void ResetOverlapped(OVERLAPPED& ov) noexcept {
    ov.Internal = 0;
    ov.InternalHigh = 0;
    ov.Offset = 0;
    ov.OffsetHigh = 0;
    if (ov.hEvent) ResetEvent(ov.hEvent);
}

bool ReadInputReport(HANDLE dev, const HIDP_CAPS& caps, UCHAR reportId,
                     std::vector<uint8_t>& report) {
    if (caps.InputReportByteLength == 0) return false;

    report.assign(caps.InputReportByteLength, 0);
    report[0] = reportId;
    // Часть устройств отдаёт последний входной отчёт напрямую.
    if (HidD_GetInputReport(dev, report.data(), static_cast<ULONG>(report.size()))) return true;

    // Остальные — только потоком; ждём недолго, опрос не должен залипать.
    std::vector<uint8_t> buffer(caps.InputReportByteLength, 0);
    buffer[0] = reportId;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    DWORD read = 0;
    bool ok = false;
    if (ReadFile(dev, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &ov)) {
        ok = read > 0;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        ok = WaitForIo(dev, ov, 150, read) && read > 0;
    }
    // После WaitForIo отмена уже дождалась завершения, поэтому и OVERLAPPED,
    // и буфер можно освобождать.
    CloseHandle(ov.hEvent);
    if (ok) {
        buffer.resize(std::min<DWORD>(read, static_cast<DWORD>(buffer.size())));
        report = std::move(buffer);
    }
    return ok;
}

}  // namespace

void PollAbort::Set(bool requested) noexcept {
    g_pollAbort.store(requested, std::memory_order_relaxed);
}

bool PollAbort::Requested() noexcept {
    return g_pollAbort.load(std::memory_order_relaxed);
}

bool LooksLikeReceiverName(const std::wstring& name) {
    if (name.empty()) return false;
    const std::wstring lower = ToLowerCopy(name);
    static constexpr const wchar_t* kHints[] = {
        L"receiver", L"приемник", L"приёмник", L"dongle", L"донгл"
    };
    for (const wchar_t* hint : kHints) {
        if (lower.find(hint) != std::wstring::npos) return true;
    }
    return false;
}

// --- LogitechProvider ---
bool LogitechProvider::Match(uint16_t vid, uint16_t) const noexcept { return vid == 0x046D; }
bool LogitechProvider::Query(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) {
    if (caps.UsagePage < 0xFF00 || caps.InputReportByteLength < 20 || caps.OutputReportByteLength < 20 || caps.InputReportByteLength > 256 || caps.OutputReportByteLength > 256) return false;
    ReportSizes sizes{ caps.InputReportByteLength, caps.OutputReportByteLength };
    HIDD_ATTRIBUTES attr{ sizeof(HIDD_ATTRIBUTES) };
    if (!HidD_GetAttributes(hDev, &attr)) return false;

    // Сначала индексы, которые уже отвечали: у спящей мыши это единственная
    // попытка вместо полного перебора шести индексов и 0xFF. Порядок — по
    // возрастанию, как раньше: мышь (1..6) важнее служебного 0xFF, иначе
    // плитка могла показать имя ресивера вместо модели мыши.
    std::vector<uint8_t> cachedIndices;
    cachedIndices.reserve(m_cache.size());
    for (const BatteryCache& cached : m_cache) {
        if (cached.pid != attr.ProductID || cached.type == BatteryCache::BatteryType::None) continue;
        cachedIndices.push_back(cached.deviceIndex);
    }
    std::sort(cachedIndices.begin(), cachedIndices.end());
    for (const uint8_t index : cachedIndices) {
        if (QueryCachedOrDiscover(hDev, sizes, attr.ProductID, index, outData)) return true;
        if (PollAbort::Requested()) return false;
    }
    const auto alreadyTried = [this, pid = attr.ProductID](uint8_t index) {
        for (const BatteryCache& cached : m_cache) {
            if (cached.pid == pid && cached.deviceIndex == index &&
                cached.type != BatteryCache::BatteryType::None) {
                return true;
            }
        }
        return false;
    };
    for (uint8_t d = 1; d <= 6; ++d) {
        if (PollAbort::Requested()) return false;
        if (!alreadyTried(d) && QueryCachedOrDiscover(hDev, sizes, attr.ProductID, d, outData)) return true;
    }
    if (PollAbort::Requested()) return false;
    return !alreadyTried(0xFF) && QueryCachedOrDiscover(hDev, sizes, attr.ProductID, 0xFF, outData);
}

LogitechProvider::BatteryCache* LogitechProvider::FindCache(uint16_t pid, uint8_t devIdx) noexcept {
    // Запись с типом None — это «уже пробовали и не нашли»: она хранит паузу
    // перед повторным перечислением, поэтому pid/deviceIndex не сбрасываем.
    for (auto& c : m_cache) if (c.pid == pid && c.deviceIndex == devIdx) return &c;
    for (auto& c : m_cache) if (c.pid == 0 && c.deviceIndex == 0) { c = BatteryCache{}; c.pid = pid; c.deviceIndex = devIdx; return &c; }
    return nullptr;
}

int LogitechProvider::VoltageToPercent(int voltageMv) noexcept {
    static constexpr std::array<int, 100> table = { 4186,4156,4143,4133,4122,4113,4103,4094,4086,4075,4067,4059,4051,4043,4035,4027,4019,4011,4003,3997,3989,3983,3976,3969,3961,3955,3949,3942,3935,3929,3922,3916,3909,3902,3896,3890,3883,3877,3870,3865,3859,3853,3848,3842,3837,3833,3828,3824,3819,3815,3811,3808,3804,3800,3797,3793,3790,3787,3784,3781,3778,3775,3772,3769,3766,3763,3760,3757,3754,3751,3748,3745,3742,3739,3736,3733,3730,3727,3724,3721,3718,3715,3712,3709,3706,3703,3700,3697,3694,3691,3688,3685,3682,3679,3676,3673,3670,3667,3664,3661 };
    if (voltageMv >= table[0]) return 100; if (voltageMv < table.back()) return 0;
    for (size_t i = 0; i + 1 < table.size(); ++i) {
        if (voltageMv <= table[i] && voltageMv >= table[i + 1]) {
            double span = static_cast<double>(table[i] - table[i + 1]);
            double t = span > 0.0 ? static_cast<double>(table[i] - voltageMv) / span : 0.0;
            return std::clamp(static_cast<int>(std::lround(100.0 - static_cast<double>(i) - t)), 0, 100);
        }
    }
    return 0;
}

bool LogitechProvider::QueryCachedOrDiscover(HANDLE hDev, const ReportSizes& sizes, uint16_t pid, uint8_t devIdx, DeviceTelemetry& outData) {
    BatteryCache* cache = FindCache(pid, devIdx);
    if (!cache) return false;
    if (cache->type == BatteryCache::BatteryType::None) {
        // Недавно перечисляли и не нашли: спящая мышь не ответит и сейчас,
        // а каждый такой перебор — это несколько таймаутов SendPacket.
        if (GetTickCount64() < cache->retryDiscoveryTick) return false;
        cache->unifiedIndex = GetFeatureIndex(hDev, sizes, devIdx, 0x1004); cache->legacyIndex = GetFeatureIndex(hDev, sizes, devIdx, 0x1000); cache->voltageIndex = GetFeatureIndex(hDev, sizes, devIdx, 0x1001);
        if (cache->unifiedIndex) cache->type = BatteryCache::BatteryType::Unified; else if (cache->legacyIndex) cache->type = BatteryCache::BatteryType::Legacy; else if (cache->voltageIndex) cache->type = BatteryCache::BatteryType::Voltage;
        if (cache->type == BatteryCache::BatteryType::None) {
            // Конфигурацию не нашли — не стираем pid/индекс, иначе следующий
            // цикл снова начнёт с нуля. Просто выдерживаем паузу.
            cache->retryDiscoveryTick = GetTickCount64() + 15000;
            return false;
        }
        cache->nameIndex = GetFeatureIndex(hDev, sizes, devIdx, 0x0005);
        if (cache->nameIndex) {
            cache->name = QueryDeviceNameWithIndex(hDev, sizes, devIdx, cache->nameIndex);
            // HID++ иногда отдаёт имя самого ресивера, а не мыши.
            if (LooksLikeReceiverName(cache->name)) cache->name.clear();
        }
    }
    std::array<uint8_t, 20> resp{};
    switch (cache->type) {
    case BatteryCache::BatteryType::Unified: {
        if (!SendPacket(hDev, sizes, devIdx, cache->unifiedIndex, 0x01, {}, resp)) return false;

        // Сглаживание с плавающей точкой (0.8/0.2) — без целочисленного «залипания».
        const int rawLevel = std::clamp<int>(resp[4], 0, 100);
        if (cache->smoothedLevel < 0.0) {
            cache->smoothedLevel = static_cast<double>(rawLevel);
        } else {
            cache->smoothedLevel = cache->smoothedLevel * 0.8 + static_cast<double>(rawLevel) * 0.2;
            if (std::abs(cache->smoothedLevel - rawLevel) < 0.5) cache->smoothedLevel = static_cast<double>(rawLevel);
        }
        outData.batteryLevel = std::clamp(static_cast<int>(std::lround(cache->smoothedLevel)), 0, 100);

        // resp[6] — не битовая маска, а код состояния (HID++ 2.0): 0 — разряд,
        // 1 — заряжается, 2 — почти полный, 3 — полный, 4 — медленная зарядка,
        // 5/6 — некорректная батарея/перегрев. Раньше здесь проверялся бит 0x80,
        // которого в этом байте не бывает, поэтому зарядка не отображалась.
        outData.isCharging = (resp[6] >= 1 && resp[6] <= 4);
        break;
    }
    case BatteryCache::BatteryType::Legacy: if (!SendPacket(hDev, sizes, devIdx, cache->legacyIndex, 0x00, {}, resp)) return false; if (resp[4] > 100) return false; outData.batteryLevel = resp[4]; outData.isCharging = (resp[6] >= 1 && resp[6] <= 4); break;
    case BatteryCache::BatteryType::Voltage: {
        if (!SendPacket(hDev, sizes, devIdx, cache->voltageIndex, 0x00, {}, resp)) return false;
        const int millivolts = (resp[4] << 8) | resp[5];
        outData.voltageMv = millivolts;
        outData.batteryLevel = VoltageToPercent(millivolts);
        // 0x1001: бит 7 — питание подключено, младшие 2 бита — ChargeStatus
        // (0 — идёт зарядка, 1 — заряжено, 2 — не заряжается).
        outData.isCharging = (resp[6] & 0x80) != 0 && ((resp[6] & 0x03) == 0 || (resp[6] & 0x03) == 1);
        break;
    }
    default: return false;
    }
    if (!cache->name.empty()) outData.deviceName = cache->name; return true;
}

bool LogitechProvider::SendPacket(HANDLE hDev, const ReportSizes& sizes, uint8_t devIdx, uint8_t featIdx, uint8_t fnId, std::span<const uint8_t> params, std::span<uint8_t, 20> outResponse) noexcept {
    const bool longReport = sizes.outputLen >= 20 && sizes.inputLen >= 20; const uint8_t reportId = longReport ? 0x11 : 0x10;
    std::array<uint8_t, 256> report{}; report[0] = reportId; report[1] = devIdx; report[2] = featIdx; report[3] = static_cast<uint8_t>((fnId << 4) | 0x07);
    if (params.size()) memcpy(&report[4], params.data(), std::min(params.size(), (size_t)(longReport ? 16 : 3)));

    OVERLAPPED writeOv{};
    writeOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!writeOv.hEvent) return false;
    DWORD written = 0;
    bool sent = false;
    if (WriteFile(hDev, report.data(), sizes.outputLen, &written, &writeOv)) {
        sent = true;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        sent = WaitForIo(hDev, writeOv, 75, written);
    }
    CloseHandle(writeOv.hEvent);
    if (!sent) return false;

    std::array<uint8_t, 256> response{};
    OVERLAPPED readOv{};
    readOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readOv.hEvent) return false;
    for (int i = 0; i < 2; ++i) {
        if (PollAbort::Requested()) break;
        DWORD read = 0;
        ResetOverlapped(readOv);
        const bool started = ReadFile(hDev, response.data(), sizes.inputLen, &read, &readOv) != FALSE;
        if (!started && GetLastError() != ERROR_IO_PENDING) continue;
        if (started || WaitForIo(hDev, readOv, 100, read)) {
            if (read >= 7 && response[0] == reportId && response[1] == devIdx && response[2] == featIdx) {
                memcpy(outResponse.data(), response.data(), std::min<size_t>(read, 20));
                CloseHandle(readOv.hEvent);
                return true;
            }
        }
    }
    CloseHandle(readOv.hEvent);
    return false;
}

uint8_t LogitechProvider::GetFeatureIndex(HANDLE hDev, const ReportSizes& sizes, uint8_t devIdx, uint16_t featureId) noexcept {
    std::array<uint8_t, 2> params = { static_cast<uint8_t>(featureId >> 8), static_cast<uint8_t>(featureId & 0xFF) };
    std::array<uint8_t, 20> resp{};
    return SendPacket(hDev, sizes, devIdx, 0x00, 0x00, params, resp) ? resp[4] : 0;
}

std::wstring LogitechProvider::QueryDeviceNameWithIndex(HANDLE hDev, const ReportSizes& sizes, uint8_t devIdx, uint8_t fNameIdx) noexcept {
    std::array<uint8_t, 20> resp{};
    if (!SendPacket(hDev, sizes, devIdx, fNameIdx, 0x00, {}, resp)) return {};
    uint8_t totalLen = resp[4]; if (totalLen == 0 || totalLen > 64) return {};
    std::string rawName; rawName.reserve(totalLen);
    for (uint8_t read = 0; read < totalLen;) {
        std::array<uint8_t, 1> p = { read };
        if (!SendPacket(hDev, sizes, devIdx, fNameIdx, 0x01, p, resp)) break;
        for (size_t b = 0; b < 16 && read < totalLen; ++b, ++read) if (resp[4 + b]) rawName.push_back(resp[4 + b]);
    }
    if (rawName.empty()) return {};
    // Конвертируем ровно len байт: -1 добавлял нулевой символ и давал запись за границу строки.
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, rawName.data(), static_cast<int>(rawName.size()), nullptr, 0);
    if (wideLen <= 0) return {};
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, rawName.data(), static_cast<int>(rawName.size()), wide.data(), wideLen) != wideLen) return {};
    return wide;
}

// --- RazerProvider ---
bool RazerProvider::Match(uint16_t vid, uint16_t) const noexcept { return vid == 0x1532; }
bool RazerProvider::Query(HANDLE hDev, const HIDP_CAPS&, DeviceTelemetry& outData) {
    std::array<uint8_t, 90> report{}; report[5] = 0x02; report[6] = 0x07; report[7] = 0x80;
    for (size_t i = 2; i < 88; ++i) report[88] ^= report[i];
    if (HidD_SetFeature(hDev, report.data(), 90)) {
        Sleep(10); report.fill(0);
        if (HidD_GetFeature(hDev, report.data(), 90) && report[9] > 0) {
            outData.batteryLevel = (report[9] <= 100) ? report[9] : static_cast<int>(report[9] * 100.0f / 255.0f);
            outData.isCharging = (report[10] != 0); return true;
        }
    }
    return false;
}

// --- CompxNordicProvider ---
bool CompxNordicProvider::Match(uint16_t vid, uint16_t) const noexcept {
    // Compx/Nordic-ресиверы: VXE/ATK, часть Attack Shark, VGN, Glorious и др.
    return vid == 0x258A || vid == 0x24AE || vid == 0x093A || vid == 0x1915 ||
           vid == 0x2BA2 || vid == 0x3554 || vid == 0x25A7 || vid == 0x25A8 ||
           vid == 0x1D57;
}

bool CompxNordicProvider::MatchDevice(HANDLE, const HIDP_CAPS& caps) noexcept {
    // Не трогаем клавиатуры и прочую периферию того же вендора: отчёт 0x08
    // имеет смысл только у мышиных/vendor-коллекций.
    if (caps.UsagePage == 0x0001) return caps.Usage == 0x02 || caps.Usage == 0x01;
    return caps.UsagePage == 0xFFA0 || caps.UsagePage == 0xFF00;
}
bool CompxNordicProvider::Query(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) {
    if (caps.UsagePage != 0xFFA0 && caps.UsagePage != 0xFF00 && caps.UsagePage != 0x0001) return false;
    std::array<uint8_t, 64> buf{ 0x08 };
    if (HidD_GetFeature(hDev, buf.data(), 64) || HidD_GetInputReport(hDev, buf.data(), 64)) {
        // i + 1 читается как признак зарядки, поэтому i ограничен 6 (а не 7).
        for (size_t i = 1; i + 1 < 8; ++i) {
            if (buf[i] > 0 && buf[i] <= 100) {
                outData.batteryLevel = buf[i];
                outData.isCharging = (buf[i + 1] == 1 || buf[i + 1] == 2);
                return true;
            }
        }
    }
    buf.fill(0); buf[0] = 0x04;
    if (HidD_GetFeature(hDev, buf.data(), 16) && buf[1] > 0 && buf[1] <= 100) { outData.batteryLevel = buf[1]; outData.isCharging = (buf[2] == 0x01); return true; }
    return false;
}

// --- RedSquareProvider (VXE / Red Square, ATK, VGN) ---
bool RedSquareProvider::Match(uint16_t vid, uint16_t) const noexcept {
    // 0x3554 — CompX/«Red Square» (VXE R1 SE+, R1 Pro Max, VGN), 0x373B — ATK
    // (в том числе VXE R1 SE+ с 1K-донглом). Модель определяется по PID, но
    // сама поддержка — по возможностям интерфейса, поэтому новые PID тоже работают.
    return vid == 0x3554 || vid == 0x373B;
}

const wchar_t* RedSquareProvider::ModelName(uint16_t vid, uint16_t pid) noexcept {
    if (vid == 0x3554) {
        switch (pid) {
        case 0xF58A: case 0xF58C: return L"VXE R1 Pro Max";
        case 0xF58E: case 0xF58F: return L"VXE R1 SE+";
        case 0xF503: case 0xF502: return L"VGN F1 Pro";
        case 0xFB3E: case 0xFB3D: return L"VGN F2 Pro Max";
        default: return L"VXE / Red Square";
        }
    }
    switch (pid) {
    case 0x1085: return L"VXE R1 SE+";
    case 0x1031: case 0x102E: return L"ATK F1 Ultimate";
    case 0x104D: case 0x103F: return L"VXE MAD R";
    case 0x1040: case 0x104C: return L"VXE MAD R Major+";
    case 0x11D9: case 0x11B6: return L"ATK A9 Ultimate";
    case 0x1155: case 0x124F: case 0x1154: return L"ATK Zero";
    case 0x1278: return L"ATK X1 Air+";
    default: return L"ATK / VXE";
    }
}

bool RedSquareProvider::Query(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) {
    // Vendor-коллекция ATK/VXE: 17 байт в обе стороны. Страницы 0xFF02 и 0xFF00
    // встречаются в разных ревизиях ресиверов; остальное отсекаем по геометрии отчёта.
    if ((caps.UsagePage == 0xFF02 || caps.UsagePage == 0xFF00) &&
        caps.InputReportByteLength == 17 && caps.OutputReportByteLength == 17) {
        // Основной протокол — cmd 0x04 (процент, флаг питания, напряжение).
        // У части прошивок работает только 0x17 (шаг 2%): запоминаем удачную команду,
        // чтобы не тратить таймаут на перебор при каждом опросе.
        const bool legacyFirst = (m_command == BatteryCommand::Legacy);
        const uint8_t first = legacyFirst ? 0x17 : 0x04;
        const uint8_t second = legacyFirst ? 0x04 : 0x17;

        std::array<uint8_t, 17> response{};
        bool ok = Exchange17(hDev, first, response) && ParseBattery17(first, response, outData);
        if (!ok) {
            response.fill(0);
            ok = Exchange17(hDev, second, response) && ParseBattery17(second, response, outData);
            if (ok) m_command = (second == 0x04) ? BatteryCommand::Full : BatteryCommand::Legacy;
        } else {
            m_command = (first == 0x04) ? BatteryCommand::Full : BatteryCommand::Legacy;
        }
        if (!ok) return false;
    } else if (caps.UsagePage == 0xFF04 && caps.FeatureReportByteLength >= 8) {
        // 1K-ресиверы некоторых ревизий держат заряд в feature report 0x06
        // (байт 3 — процент, байты 6..7 — напряжение little-endian).
        if (!QueryFeatureBattery(hDev, caps, outData)) return false;
    } else {
        return false;
    }

    HIDD_ATTRIBUTES attr{ sizeof(HIDD_ATTRIBUTES) };
    if (HidD_GetAttributes(hDev, &attr)) {
        const wchar_t* name = ModelName(attr.VendorID, attr.ProductID);
        if (name && *name) outData.deviceName = name;
    }
    return true;
}

bool RedSquareProvider::Exchange17(HANDLE hDev, uint8_t command, std::array<uint8_t, 17>& response) noexcept {
    std::array<uint8_t, 17> request{};
    request[0] = 0x08;
    request[1] = command;
    request[16] = static_cast<uint8_t>(0x4D - command);  // контрольная сумма запроса

    OVERLAPPED writeOv{};
    writeOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!writeOv.hEvent) return false;

    bool sent = false;
    DWORD written = 0;
    if (WriteFile(hDev, request.data(), static_cast<DWORD>(request.size()), &written, &writeOv)) {
        sent = true;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        sent = WaitForIo(hDev, writeOv, 100, written);
    }
    CloseHandle(writeOv.hEvent);
    if (!sent) return false;

    // Ответ приходит отдельным input-отчётом; чужие отчёты (движение мыши)
    // пропускаем, пока не истечёт короткий таймаут. Спящая мышь просто не ответит.
    OVERLAPPED readOv{};
    readOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readOv.hEvent) return false;

    const ULONGLONG deadline = GetTickCount64() + 260;
    std::array<uint8_t, 17> report{};
    bool found = false;
    while (!found && GetTickCount64() < deadline) {
        if (PollAbort::Requested()) break;
        DWORD read = 0;
        report.fill(0);
        ResetOverlapped(readOv);
        if (!ReadFile(hDev, report.data(), static_cast<DWORD>(report.size()), &read, &readOv)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            const DWORD waitMs = static_cast<DWORD>(deadline > GetTickCount64() ? deadline - GetTickCount64() : 1);
            // WaitForIo при таймауте сам отменяет запрос и дожидается отмены —
            // освобождать OVERLAPPED раньше нельзя.
            if (!WaitForIo(hDev, readOv, waitMs, read)) break;
        }
        // Эхо отчёта: 0x08 — report ID, вторым байтом возвращается команда.
        if (read >= 3 && report[0] == 0x08 && report[1] == command) found = true;
    }
    CloseHandle(readOv.hEvent);
    if (!found) return false;

    response = report;
    return true;
}

bool RedSquareProvider::ParseBattery17(uint8_t command, const std::array<uint8_t, 17>& response, DeviceTelemetry& outData) noexcept {
    if (response[0] != 0x08 || response[1] != command) return false;

    if (command == 0x04) {
        const int level = response[6];
        if (level > 100) return false;
        outData.batteryLevel = level;
        // Байт 7: 0 — питание от батареи, 1 — кабель/зарядка.
        outData.isCharging = response[7] != 0;
        const int millivolts = (static_cast<int>(response[8]) << 8) | response[9];
        if (millivolts >= 2500 && millivolts <= 5000) outData.voltageMv = millivolts;
        return true;
    }

    if (command == 0x17) {
        // Старый вариант: заряд отдаётся половинными шагами, отсюда умножение на 2.
        const int level = response[5] * 2;
        if (level <= 0 || level > 100) return false;
        outData.batteryLevel = level;
        outData.isCharging = false;
        return true;
    }

    return false;
}

bool RedSquareProvider::QueryFeatureBattery(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) noexcept {
    std::array<uint8_t, 64> buffer{};
    buffer[0] = 0x06;
    ULONG length = caps.FeatureReportByteLength;
    if (length > static_cast<ULONG>(buffer.size())) length = static_cast<ULONG>(buffer.size());
    if (length < 8) return false;
    if (!HidD_GetFeature(hDev, buffer.data(), length)) return false;
    if (buffer[0] != 0x06) return false;

    const int level = buffer[3];
    const int millivolts = buffer[6] | (buffer[7] << 8);
    if (level > 100) return false;
    // Ноль без правдоподобного напряжения — это «нет данных», а не разряженная мышь.
    if (level == 0 && (millivolts < 2500 || millivolts > 5000)) return false;

    outData.batteryLevel = level;
    if (millivolts >= 2500 && millivolts <= 5000) outData.voltageMv = millivolts;
    outData.isCharging = false;
    return true;
}

// --- BluetoothProvider ---
bool BluetoothProvider::Query(DeviceTelemetry& outData) {
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&kGuidBth, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA devInfoData = { sizeof(SP_DEVINFO_DATA) };

    DeviceTelemetry best;
    bool found = false;

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); ++i) {
        uint8_t batVal = 0; DWORD propSize = sizeof(batVal);
        if (!SetupDiGetDevicePropertyW(hDevInfo, &devInfoData, &kPkeyBatteryLevel, nullptr, &batVal, sizeof(batVal), &propSize, 0)) continue;
        // 0 — это честный «разряжен», а не «нет данных»: свойство батареи
        // существует только у устройств с батареей.
        if (batVal > 100) continue;

        DeviceTelemetry candidate;
        std::array<WCHAR, 128> name{};
        if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<PBYTE>(name.data()), sizeof(name), nullptr))
            candidate.deviceName = name.data();

        // Аудиоустройства (AirPods, наушники, колонки) — не мышь. У AirPods есть
        // отдельная плитка; если брать их здесь, спящая мышь подменялась бы
        // наушниками. Bluetooth-мышь под это правило не попадает.
        if (LooksLikeAudioDevice(candidate.deviceName)) continue;

        candidate.batteryLevel = batVal;
        candidate.isCharging = false;
        best = std::move(candidate);
        found = true;
        break;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    if (!found) return false;
    outData = std::move(best);
    return true;
}

// --- PnpBatteryProvider ---
bool PnpBatteryProvider::Query(DeviceTelemetry& outData) {
    DeviceTelemetry best;
    int bestScore = -1;

    const auto scanClass = [&best, &bestScore](const GUID& classGuid, DWORD flags) {
        HDEVINFO set = SetupDiGetClassDevsW(&classGuid, nullptr, nullptr, flags);
        if (set == INVALID_HANDLE_VALUE) return;

        SP_DEVINFO_DATA info{ sizeof(SP_DEVINFO_DATA) };
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
            if (PollAbort::Requested()) break;
            DEVPROPTYPE type = 0;
            DWORD needed = 0;
            if (!SetupDiGetDevicePropertyW(set, &info, &kPkeyBatteryLevel, &type, nullptr, 0, &needed, 0) ||
                needed == 0) {
                continue;
            }
            std::vector<BYTE> raw(needed);
            if (!SetupDiGetDevicePropertyW(set, &info, &kPkeyBatteryLevel, &type, raw.data(), needed,
                                           &needed, 0)) {
                continue;
            }

            int level = -1;
            if (type == DEVPROP_TYPE_BYTE && needed >= 1) {
                level = raw[0];
            } else if (type == DEVPROP_TYPE_UINT16 && needed >= 2) {
                level = *reinterpret_cast<const WORD*>(raw.data());
            } else if (type == DEVPROP_TYPE_UINT32 && needed >= 4) {
                level = static_cast<int>(*reinterpret_cast<const DWORD*>(raw.data()));
            } else if (type == DEVPROP_TYPE_INT32 && needed >= 4) {
                level = *reinterpret_cast<const LONG*>(raw.data());
            }
            if (level < 0 || level > 100) continue;

            wchar_t friendly[256]{};
            std::wstring name;
            if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr,
                                                  reinterpret_cast<PBYTE>(friendly), sizeof(friendly),
                                                  nullptr)) {
                name = friendly;
            }
            if (LooksLikeAudioDevice(name) || LooksLikeNonMouse(name)) continue;
            // Имя ресивера не должно попадать на плитку как модель мыши.
            if (LooksLikeReceiverName(name)) name.clear();

            const int score = MouseNameScore(name);
            // Ноль у ресивера или безымянного устройства — это «нет данных», а
            // не разряженная мышь: иначе плитка переключается на «USB Receiver».
            // Настоящий 0 % принимаем только у устройства, явно похожего на мышь.
            if (level == 0 && score < 3) continue;
            if (score <= bestScore) continue;

            bestScore = score;
            best.deviceName = name;
            best.batteryLevel = level;
            best.isCharging = false;
            best.voltageMv = -1;
            best.hasData = true;
        }
        SetupDiDestroyDeviceInfoList(set);
    };

    scanClass(kGuidHidClass, DIGCF_PRESENT);
    scanClass(kGuidBth, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (bestScore < 0) return false;
    outData = std::move(best);
    return true;
}

// --- GenericBatteryProvider ---
bool GenericBatteryProvider::MatchDevice(HANDLE hDev, const HIDP_CAPS&) noexcept {
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(hDev, &preparsed)) return false;

    HIDP_VALUE_CAPS cap{};
    const bool found = FindBatteryCap(preparsed, HidP_Input, cap) ||
                       FindBatteryCap(preparsed, HidP_Feature, cap);
    HidD_FreePreparsedData(preparsed);
    return found;
}

bool GenericBatteryProvider::Query(HANDLE hDev, const HIDP_CAPS& caps, DeviceTelemetry& outData) {
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(hDev, &preparsed)) return false;

    int percent = -1;
    bool found = false;
    for (const HIDP_REPORT_TYPE type : { HidP_Input, HidP_Feature }) {
        HIDP_VALUE_CAPS cap{};
        if (!FindBatteryCap(preparsed, type, cap)) continue;

        std::vector<uint8_t> report;
        const bool read = (type == HidP_Input)
                              ? ReadInputReport(hDev, caps, cap.ReportID, report)
                              : ReadFeatureReport(hDev, caps, cap.ReportID, report);
        if (read && ExtractBatteryPercent(preparsed, type, cap, report, percent)) {
            found = true;
            break;
        }
    }
    HidD_FreePreparsedData(preparsed);

    if (!found) return false;
    outData.batteryLevel = percent;
    outData.isCharging = false;  // стандартное поле сообщает только уровень
    return true;
}