#define NOMINMAX
#include "airpods/AirPodsMonitor.h"
#include "core/AppState.h"
#include "core/Diagnostics.h"
#include "core/Localization.h"

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Скан идёт окнами: 5 секунд слушаем рекламу, потом 25 секунд отдыхаем.
// Подключение/отключение отслеживается отдельным Bluetooth-вотчером, поэтому
// для появления/исчезновения плитки скан не нужен — он только обновляет заряд.
constexpr DWORD kScanWindowMs = 5000;
constexpr DWORD kScanPeriodMs = 30000;
// Запасной вариант, если Windows не отдаёт состояние подключения: реклама
// должна идти не реже полутора минут. Это старая стабильная схема без мигания.
constexpr std::uint64_t kAdvertFallbackMs = 90000;
// Ограничение на карту адресов: в отчёте не нужен весь эфир.
constexpr std::size_t kMaxAdvertRecords = 48;

std::uint64_t NowUnixMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::wstring HexBytes(const std::vector<std::uint8_t>& bytes) {
    std::wstring text;
    wchar_t buffer[4]{};
    for (const std::uint8_t byte : bytes) {
        swprintf_s(buffer, L"%02X ", byte);
        text += buffer;
    }
    if (!text.empty()) text.pop_back();
    return text;
}

std::wstring AddressText(std::uint64_t address) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02llX:%02llX:%02llX:%02llX:%02llX:%02llX",
               static_cast<unsigned long long>((address >> 40) & 0xFF),
               static_cast<unsigned long long>((address >> 32) & 0xFF),
               static_cast<unsigned long long>((address >> 24) & 0xFF),
               static_cast<unsigned long long>((address >> 16) & 0xFF),
               static_cast<unsigned long long>((address >> 8) & 0xFF),
               static_cast<unsigned long long>(address & 0xFF));
    return buffer;
}

std::wstring LocalTimeText(std::uint64_t unixMs) {
    if (unixMs == 0) return L"never";
    constexpr std::uint64_t kEpochOffset100ns = 116444736000000000ULL;  // 1601 → 1970
    const ULONGLONG ticks = unixMs * 10000ULL + kEpochOffset100ns;

    FILETIME utc{};
    utc.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
    utc.dwHighDateTime = static_cast<DWORD>(ticks >> 32);

    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return L"?";

    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::wstring AgeText(std::uint64_t unixMs) {
    if (unixMs == 0) return L"never";
    const std::uint64_t now = NowUnixMs();
    if (now <= unixMs) return L"just now";
    const std::uint64_t seconds = (now - unixMs) / 1000;
    if (seconds < 60) return std::to_wstring(seconds) + L" s ago";
    if (seconds < 3600) return std::to_wstring(seconds / 60) + L" min ago";
    if (seconds < 86400) return std::to_wstring(seconds / 3600) + L" h ago";
    return std::to_wstring(seconds / 86400) + L" d ago";
}

// Значения AirPods: 0..9 — шаг 10%, 0xA..0xE — 100%, 0xF — неизвестно.
int DecodeNibble(int value) {
    if (value < 0) return -1;
    if (value <= 9) return value * 10;
    if (value <= 0xE) return 100;
    return -1;
}

/// Модель из байтов [3..4] сообщения 0x07. Нужна и для имени плитки: BLE-имя
/// у AirPods обычно пустое, а «Bluetooth 5b:8c:...» показывать бессмысленно.
const wchar_t* ModelName(std::uint16_t model) {
    switch (model) {
    case 0x0220: return L"AirPods";
    case 0x0F20: return L"AirPods 2";
    case 0x1320: return L"AirPods 3";
    case 0x1920: return L"AirPods 4";
    case 0x1B20: return L"AirPods 4 ANC";
    case 0x0A20: return L"AirPods Max";
    case 0x1F20: return L"AirPods Max";
    case 0x0E20: return L"AirPods Pro";
    case 0x1420: return L"AirPods Pro 2";
    case 0x2420: return L"AirPods Pro 2";
    default: return L"AirPods";
    }
}

std::wstring UuidText(const winrt::guid& uuid) {
    wchar_t buffer[48]{};
    swprintf_s(buffer, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
               static_cast<unsigned long>(uuid.Data1), uuid.Data2, uuid.Data3,
               uuid.Data4[0], uuid.Data4[1], uuid.Data4[2], uuid.Data4[3],
               uuid.Data4[4], uuid.Data4[5], uuid.Data4[6], uuid.Data4[7]);
    return buffer;
}

std::vector<std::uint8_t> BufferBytes(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    const std::uint8_t* data = buffer.data();
    return std::vector<std::uint8_t>(data, data + buffer.Length());
}

std::wstring Utf8ToWide(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0,
                                           reinterpret_cast<const char*>(bytes.data()),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()),
                        static_cast<int>(bytes.size()), text.data(), length);
    return text;
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}

/// Поддерживаемые наушники — Apple/Beats. Обычные BT-гарнитуры пока не
/// отслеживаются: для них нет источника заряда.
bool LooksLikeHeadsetName(const std::wstring& name) {
    const std::wstring lower = Lower(name);
    return lower.find(L"airpods") != std::wstring::npos ||
           lower.find(L"beats") != std::wstring::npos;
}

/// "72:16:8B:42:CC:6A" → 0x72168B42CC6A. Ноль, если строка не похожа на адрес.
std::uint64_t ParseBluetoothAddress(const std::wstring& text) {
    std::uint64_t value = 0;
    int digits = 0;
    for (const wchar_t ch : text) {
        int digit = -1;
        if (ch >= L'0' && ch <= L'9') digit = ch - L'0';
        else if (ch >= L'a' && ch <= L'f') digit = ch - L'a' + 10;
        else if (ch >= L'A' && ch <= L'F') digit = ch - L'A' + 10;
        if (digit < 0) continue;
        value = (value << 4) | static_cast<std::uint64_t>(digit);
        ++digits;
    }
    return digits >= 12 ? value : 0;
}

const wchar_t* AccessText(winrt::Windows::Devices::Enumeration::DeviceAccessStatus status) {
    using winrt::Windows::Devices::Enumeration::DeviceAccessStatus;
    switch (status) {
    case DeviceAccessStatus::Allowed: return L"allowed";
    case DeviceAccessStatus::DeniedByUser: return L"denied by user";
    case DeviceAccessStatus::DeniedBySystem: return L"denied by system";
    default: return L"unspecified";
    }
}

const wchar_t* GattStatusText(
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus status) {
    using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
    switch (status) {
    case GattCommunicationStatus::Success: return L"success";
    case GattCommunicationStatus::Unreachable: return L"unreachable";
    case GattCommunicationStatus::ProtocolError: return L"protocol error";
    case GattCommunicationStatus::AccessDenied: return L"access denied";
    default: return L"unknown";
    }
}

const winrt::guid kBatteryServiceUuid{
    0x0000180F, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
const winrt::guid kBatteryLevelUuid{
    0x00002A19, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
const winrt::guid kUserDescriptionUuid{
    0x00002901, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

}  // namespace

struct AirPodsMonitor::Impl {
    struct Advert {
        std::uint64_t address = 0;
        int rssi = 0;
        std::wstring name;
        std::uint16_t company = 0;
        std::vector<std::uint8_t> data;
        std::uint64_t seenUnixMs = 0;
        bool apple = false;
        std::uint8_t appleType = 0;
    };

    struct GattEntry {
        std::wstring service;
        std::wstring characteristic;
        std::wstring description;
        std::wstring properties;
        std::wstring status;
        int level = -1;
        std::wstring hex;
    };

    struct GattInfo {
        std::uint64_t address = 0;
        std::wstring name;
        std::wstring access;
        std::wstring status;
        std::wstring error;
        std::vector<GattEntry> entries;
        std::uint64_t queriedMs = 0;
        bool airPods = false;
    };

    struct DeviceRecord {
        std::wstring name;
        std::wstring id;
        std::uint64_t address = 0;
    };

    HANDLE stopEvent{ nullptr };
    HANDLE thread{ nullptr };

    mutable std::mutex mutex;
    std::map<std::uint64_t, Advert> adverts;
    std::wstring deviceName;
    std::uint64_t lastAdvertUnixMs{ 0 };
    std::uint64_t lastSuccessUnixMs{ 0 };
    std::uint64_t lastAppleAddress{ 0 };
    int leftLevel{ -1 };
    int rightLevel{ -1 };
    int caseLevel{ -1 };
    bool leftCharging{ false };
    bool rightCharging{ false };
    bool caseCharging{ false };
    std::uint64_t leftSeenMs{ 0 };
    std::uint64_t rightSeenMs{ 0 };
    std::uint64_t caseSeenMs{ 0 };

    GattInfo gatt;
    std::vector<DeviceRecord> deviceList;
    std::uint64_t lastGattOkMs{ 0 };
    bool gattLogged{ false };

    // Отслеживание именно подключения классического Bluetooth-устройства:
    // реклама может замолкать, а связь при этом остаётся. Плитка не должна
    // из-за этого мигать.
    winrt::Windows::Devices::Enumeration::DeviceWatcher btWatcher{ nullptr };
    winrt::event_token btAddedToken{};
    winrt::event_token btRemovedToken{};
    winrt::event_token btEnumToken{};
    std::map<std::wstring, std::wstring> connectedBtDevices;  // id -> имя
    bool btWatcherReady{ false };
    bool btWatcherFailed{ false };
    bool deviceConnected{ false };

    bool scanning{ false };
    int scanCount{ 0 };
    int advertCount{ 0 };
    std::wstring lastError;

    void StartBtWatcher() {
        using namespace winrt::Windows::Devices::Bluetooth;
        using namespace winrt::Windows::Devices::Enumeration;

        try {
            btWatcher = DeviceInformation::CreateWatcher(
                BluetoothDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected));

            btAddedToken = btWatcher.Added(
                [this](const DeviceWatcher&, const DeviceInformation& info) {
                    try {
                        const std::wstring name = info.Name().c_str();
                        if (!LooksLikeHeadsetName(name)) return;
                        const std::wstring id = info.Id().c_str();
                        {
                            std::lock_guard lock(mutex);
                            connectedBtDevices[id] = name;
                            deviceConnected = true;
                        }
                        Diag::Log(L"airpods: headset connected (%ls)", name.c_str());
                        AppState::Instance().RequestAirPodsScan();
                    } catch (...) {
                    }
                });

            btRemovedToken = btWatcher.Removed(
                [this](const DeviceWatcher&, const DeviceInformationUpdate& update) {
                    try {
                        const std::wstring id = update.Id().c_str();
                        bool disconnected = false;
                        std::wstring name;
                        {
                            std::lock_guard lock(mutex);
                            const auto it = connectedBtDevices.find(id);
                            if (it != connectedBtDevices.end()) {
                                name = it->second;
                                connectedBtDevices.erase(it);
                            }
                            if (connectedBtDevices.empty() && deviceConnected) {
                                deviceConnected = false;
                                disconnected = true;
                            }
                        }
                        if (disconnected) {
                            Diag::Log(L"airpods: headset disconnected (%ls)",
                                      name.empty() ? L"unknown" : name.c_str());
                        }
                    } catch (...) {
                    }
                });

            btEnumToken = btWatcher.EnumerationCompleted(
                [this](const DeviceWatcher&, const winrt::Windows::Foundation::IInspectable&) {
                    try {
                        std::size_t count = 0;
                        {
                            std::lock_guard lock(mutex);
                            btWatcherReady = true;
                            deviceConnected = !connectedBtDevices.empty();
                            count = connectedBtDevices.size();
                        }
                        Diag::Log(L"airpods: bluetooth watcher ready, connected headsets=%zu", count);
                    } catch (...) {
                    }
                });

            btWatcher.Start();
        } catch (const winrt::hresult_error& error) {
            std::lock_guard lock(mutex);
            btWatcherFailed = true;
            lastError = L"BT watcher failed: " + std::wstring(error.message().c_str());
        } catch (...) {
            std::lock_guard lock(mutex);
            btWatcherFailed = true;
        }
    }

    void StopBtWatcher() {
        try {
            if (!btWatcher) return;
            btWatcher.Added(btAddedToken);
            btWatcher.Removed(btRemovedToken);
            btWatcher.EnumerationCompleted(btEnumToken);

            // Дожидаемся полной остановки: колбэки вотчера идут из пула
            // потоков, и удалять Impl, пока они могут выполняться, нельзя.
            auto stopped = std::make_shared<std::promise<void>>();
            const auto stoppedToken = btWatcher.Stopped(
                [stopped](const winrt::Windows::Devices::Enumeration::DeviceWatcher&,
                          const winrt::Windows::Foundation::IInspectable&) {
                    try {
                        stopped->set_value();
                    } catch (...) {
                    }
                });
            btWatcher.Stop();
            stopped->get_future().wait_for(std::chrono::milliseconds(1000));
            btWatcher.Stopped(stoppedToken);
            btWatcher = nullptr;
        } catch (...) {
        }
    }

    void Publish() {
        AirPodsTelemetry telemetry;
        {
            std::lock_guard lock(mutex);
            const std::uint64_t now = NowUnixMs();
            telemetry.deviceName = deviceName;
            telemetry.leftLevel = leftLevel;
            telemetry.rightLevel = rightLevel;
            telemetry.caseLevel = caseLevel;
            telemetry.leftCharging = leftCharging;
            telemetry.rightCharging = rightCharging;
            telemetry.caseCharging = caseCharging;
            telemetry.lastSeenUnixMs = lastAdvertUnixMs;
            telemetry.lastSuccessUnixMs = lastSuccessUnixMs;
            telemetry.hasData = lastSuccessUnixMs != 0;

            if (btWatcherReady) {
                // Видимость определяется только реальным состоянием
                // Bluetooth-подключения. Реклама обновляет заряд, но не может
                // ни спрятать, ни показать плитку — поэтому нет мерцания.
                telemetry.connected = deviceConnected;
            } else {
                // Вотчер не поднялся — старая стабильная схема по рекламе.
                telemetry.connected =
                    (lastAdvertUnixMs != 0 && now >= lastAdvertUnixMs &&
                     (now - lastAdvertUnixMs) <= kAdvertFallbackMs) ||
                    (lastGattOkMs != 0 && now >= lastGattOkMs &&
                     (now - lastGattOkMs) <= kAdvertFallbackMs);
            }
        }
        AppState::Instance().UpdateAirPods(telemetry);
    }

    /// Опрос выключен (плитку скрыли): данные не считаются «на связи», иначе
    /// после возобновления старый заряд выглядел бы как новое подключение.
    void PublishDisconnected() {
        AirPodsTelemetry telemetry;
        {
            std::lock_guard lock(mutex);
            telemetry.deviceName = deviceName;
            telemetry.leftLevel = leftLevel;
            telemetry.rightLevel = rightLevel;
            telemetry.caseLevel = caseLevel;
            telemetry.lastSeenUnixMs = lastAdvertUnixMs;
            telemetry.lastSuccessUnixMs = lastSuccessUnixMs;
            telemetry.hasData = lastSuccessUnixMs != 0;
            telemetry.connected = false;
        }
        AppState::Instance().UpdateAirPods(telemetry);
    }

    // Разбор Apple Continuity-сообщения 0x07 (AirPods). Раскладка:
    //   [0] тип 0x07, [1] длина, [2] режим, [3..4] модель, [5] статус,
    //   [6] заряд наушников, [7] кейс (старший полубайт) + флаги (младший),
    //   [8] крышка, [9] цвет, [10] состояние подключения.
    // Верхний полубайт [6] относится к «основному» наушнику: бит 5 статуса
    // говорит, левый он (1) или правый (0), поэтому при правом основном
    // полубайты меняются местами. Если прошивка пришлёт другой формат, в отчёте
    // останется hex — парсер можно будет поправить без догадок.
    void MergeAppleAdvert(const Advert& advert, std::uint64_t now) {
        const std::vector<std::uint8_t>& data = advert.data;
        if (data.size() < 8) return;
        if (data[0] != 0x07) return;  // не сообщение с зарядом

        const std::uint16_t model = static_cast<std::uint16_t>((data[3] << 8) | data[4]);
        deviceName = ModelName(model);

        const int status = data[5];
        const bool leftPrimary = (status & 0x20) != 0;

        const int upper = DecodeNibble((data[6] >> 4) & 0x0F);
        const int lower = DecodeNibble(data[6] & 0x0F);
        const int left = leftPrimary ? upper : lower;
        const int right = leftPrimary ? lower : upper;

        const int casePercent = DecodeNibble((data[7] >> 4) & 0x0F);
        const int flags = data[7] & 0x0F;
        bool leftCharging = (flags & 0x02) != 0;
        bool rightCharging = (flags & 0x01) != 0;
        const bool caseCharging = (flags & 0x04) != 0;
        if (!leftPrimary) std::swap(leftCharging, rightCharging);

        if (left >= 0) {
            leftLevel = left;
            leftSeenMs = now;
        }
        if (right >= 0) {
            rightLevel = right;
            rightSeenMs = now;
        }
        if (casePercent >= 0) {
            caseLevel = casePercent;
            caseSeenMs = now;
        }
        this->leftCharging = leftCharging;
        this->rightCharging = rightCharging;
        this->caseCharging = caseCharging;

        // Компонент, который давно не приходил (наушник в кейсе), не должен
        // висеть вечным: через 15 минут неизвестность честнее старого числа.
        constexpr std::uint64_t kComponentTtlMs = 15 * 60 * 1000;
        if (leftSeenMs != 0 && now - leftSeenMs > kComponentTtlMs) leftLevel = -1;
        if (rightSeenMs != 0 && now - rightSeenMs > kComponentTtlMs) rightLevel = -1;
        if (caseSeenMs != 0 && now - caseSeenMs > kComponentTtlMs) caseLevel = -1;

        lastAdvertUnixMs = now;
        if (left >= 0 || right >= 0 || casePercent >= 0) {
            if (lastSuccessUnixMs == 0) {
                Diag::Log(L"airpods: first battery from advertisement, name=%ls L=%d R=%d C=%d",
                          deviceName.c_str(), left, right, casePercent);
            }
            lastSuccessUnixMs = now;
        }
    }

    void ApplyGattLevels(int left, int right, int caseValue, const std::wstring& name,
                         std::uint64_t now) {
        bool any = false;
        if (left >= 0 && left <= 100) {
            leftLevel = left;
            leftSeenMs = now;
            any = true;
        }
        if (right >= 0 && right <= 100) {
            rightLevel = right;
            rightSeenMs = now;
            any = true;
        }
        if (caseValue >= 0 && caseValue <= 100) {
            caseLevel = caseValue;
            caseSeenMs = now;
            any = true;
        }
        if (!name.empty() && Lower(name).find(L"airpods") != std::wstring::npos) {
            deviceName = name;
        }
        if (!any) return;

        lastSuccessUnixMs = now;
        lastGattOkMs = now;
        // GATT-ответ — тоже доказательство, что устройство на связи.
        lastAdvertUnixMs = now;
    }

    /// Список сопряжённых BLE-устройств. Нужен, когда AirPods молчат в эфире:
    /// адрес для GATT можно взять прямо из Windows, не дожидаясь рекламы.
    void EnumeratePairedBle() {
        using namespace winrt::Windows::Devices::Bluetooth;
        using namespace winrt::Windows::Devices::Enumeration;

        std::vector<DeviceRecord> records;
        try {
            const DeviceInformationCollection devices =
                DeviceInformation::FindAllAsync(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)).get();
            for (const DeviceInformation& info : devices) {
                DeviceRecord record;
                try {
                    record.name = info.Name().c_str();
                    record.id = info.Id().c_str();
                    const auto properties = info.Properties();
                    const winrt::hstring key = L"System.Devices.Aep.DeviceAddress";
                    if (properties.HasKey(key)) {
                        const winrt::Windows::Foundation::IInspectable value = properties.Lookup(key);
                        if (value) {
                            record.address = ParseBluetoothAddress(
                                winrt::unbox_value<winrt::hstring>(value).c_str());
                        }
                    }
                    if (record.address == 0) {
                        const BluetoothLEDevice device =
                            BluetoothLEDevice::FromIdAsync(info.Id()).get();
                        if (device) {
                            record.address = device.BluetoothAddress();
                            device.Close();
                        }
                    }
                } catch (...) {
                }
                records.push_back(std::move(record));
            }
        } catch (const winrt::hresult_error& error) {
            std::lock_guard lock(mutex);
            lastError = L"BLE enumeration failed: " + std::wstring(error.message().c_str());
        } catch (...) {
        }

        std::lock_guard lock(mutex);
        deviceList = std::move(records);
        if (lastAppleAddress == 0) {
            for (const DeviceRecord& record : deviceList) {
                if (Lower(record.name).find(L"airpods") != std::wstring::npos) {
                    lastAppleAddress = record.address;
                    break;
                }
            }
        }
    }

    /// Прочитать Battery Service (0x180F) у BLE-устройства по адресу.
    /// У AirPods это три характеристики 0x2A19 — левый наушник, правый и кейс;
    /// к какой именно относится значение, видно по дескриптору 0x2901.
    void QueryGatt(std::uint64_t address) {
        using namespace winrt::Windows::Devices::Bluetooth;
        using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

        GattInfo info;
        info.address = address;
        info.queriedMs = NowUnixMs();

        auto store = [this, &info] {
            std::lock_guard lock(mutex);
            gatt = info;
        };

        if (address == 0) {
            info.error = L"no Apple address seen yet";
            store();
            return;
        }

        BluetoothLEDevice device{ nullptr };
        try {
            device = BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
        } catch (const winrt::hresult_error& error) {
            info.error = L"connect failed: " + std::wstring(error.message().c_str());
            store();
            return;
        }

        if (!device) {
            info.error = L"device not found (not paired, out of range or radio off)";
            store();
            return;
        }

        try {
            info.name = device.Name().c_str();
            info.access = AccessText(device.DeviceAccessInformation().CurrentStatus());

            const GattDeviceServicesResult services =
                device.GetGattServicesAsync(BluetoothCacheMode::Uncached).get();
            info.status = GattStatusText(services.Status());

            if (services.Status() != GattCommunicationStatus::Success) {
                info.error = L"battery services unavailable";
            } else {
                for (const GattDeviceService& service : services.Services()) {
                    const bool batteryService = service.Uuid() == kBatteryServiceUuid;
                    const GattCharacteristicsResult characteristics =
                        service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
                    if (characteristics.Status() != GattCommunicationStatus::Success) continue;

                    for (const GattCharacteristic& characteristic : characteristics.Characteristics()) {
                        GattEntry entry;
                        entry.service = UuidText(service.Uuid());
                        entry.characteristic = UuidText(characteristic.Uuid());
                        entry.status = L"not read";

                        const auto properties = characteristic.CharacteristicProperties();
                        auto addProperty = [&entry, properties](GattCharacteristicProperties flag,
                                                                const wchar_t* text) {
                            if ((properties & flag) == flag) {
                                if (!entry.properties.empty()) entry.properties += L",";
                                entry.properties += text;
                            }
                        };
                        addProperty(GattCharacteristicProperties::Read, L"read");
                        addProperty(GattCharacteristicProperties::Notify, L"notify");
                        addProperty(GattCharacteristicProperties::Indicate, L"indicate");
                        addProperty(GattCharacteristicProperties::Write, L"write");

                        // Читаем только заряд: остальные характеристики могут
                        // менять состояние наушников, а нам это не нужно.
                        const bool isBatteryLevel = characteristic.Uuid() == kBatteryLevelUuid;
                        if (isBatteryLevel &&
                            (properties & GattCharacteristicProperties::Read) ==
                                GattCharacteristicProperties::Read) {
                            const GattReadResult read =
                                characteristic.ReadValueAsync(BluetoothCacheMode::Uncached).get();
                            entry.status = GattStatusText(read.Status());
                            if (read.Status() == GattCommunicationStatus::Success) {
                                const std::vector<std::uint8_t> bytes = BufferBytes(read.Value());
                                entry.hex = HexBytes(bytes);
                                if (!bytes.empty() && bytes[0] <= 100) entry.level = bytes[0];
                            }

                            const GattDescriptorsResult descriptors =
                                characteristic.GetDescriptorsAsync(BluetoothCacheMode::Uncached).get();
                            if (descriptors.Status() == GattCommunicationStatus::Success) {
                                for (const GattDescriptor& descriptor : descriptors.Descriptors()) {
                                    if (descriptor.Uuid() != kUserDescriptionUuid) continue;
                                    const GattReadResult value =
                                        descriptor.ReadValueAsync(BluetoothCacheMode::Uncached).get();
                                    if (value.Status() == GattCommunicationStatus::Success) {
                                        entry.description = Utf8ToWide(BufferBytes(value.Value()));
                                    }
                                }
                            }
                        }

                        if (batteryService || isBatteryLevel) info.entries.push_back(std::move(entry));
                    }
                }
            }
        } catch (const winrt::hresult_error& error) {
            info.error = L"GATT error: " + std::wstring(error.message().c_str());
        } catch (...) {
            info.error = L"GATT error: unknown";
        }

        try {
            device.Close();
        } catch (...) {
        }

        // Мэппинг компонентов: сначала по описанию, затем по порядку.
        int left = -1;
        int right = -1;
        int caseValue = -1;
        std::vector<int> unspecified;
        for (const GattEntry& entry : info.entries) {
            if (entry.level < 0) continue;
            const std::wstring description = Lower(entry.description);
            if (description.find(L"left") != std::wstring::npos) {
                left = entry.level;
            } else if (description.find(L"right") != std::wstring::npos) {
                right = entry.level;
            } else if (description.find(L"case") != std::wstring::npos) {
                caseValue = entry.level;
            } else {
                unspecified.push_back(entry.level);
            }
        }
        for (const int value : unspecified) {
            if (left < 0) left = value;
            else if (right < 0) right = value;
            else if (caseValue < 0) caseValue = value;
        }

        const std::wstring lowerName = Lower(info.name);
        info.airPods = lowerName.find(L"airpods") != std::wstring::npos;
        if (!info.airPods && left >= 0 && right >= 0) {
            // Имени может не быть, но три уровня заряда у Apple-адреса — это AirPods.
            info.airPods = true;
        }

        if (info.airPods && (left >= 0 || right >= 0 || caseValue >= 0)) {
            const std::uint64_t now = NowUnixMs();
            std::lock_guard lock(mutex);
            ApplyGattLevels(left, right, caseValue, info.name, now);
            if (!gattLogged) {
                gattLogged = true;
                Diag::Log(L"airpods: GATT battery name=%ls left=%d right=%d case=%d",
                          info.name.empty() ? L"(unknown)" : info.name.c_str(), left, right, caseValue);
            }
        }
        store();
    }

    void OnAdvertisement(
        const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args) {
        using namespace winrt::Windows::Devices::Bluetooth::Advertisement;

        Advert advert;
        advert.address = args.BluetoothAddress();
        advert.rssi = args.RawSignalStrengthInDBm();

        const BluetoothLEAdvertisement advertisement = args.Advertisement();
        const winrt::hstring localName = advertisement.LocalName();
        if (!localName.empty()) advert.name = localName.c_str();

        const std::uint64_t now = NowUnixMs();
        const auto manufacturerData = advertisement.ManufacturerData();
        for (const auto& entry : manufacturerData) {
            const winrt::Windows::Storage::Streams::IBuffer buffer = entry.Data();
            const std::uint8_t* bytes = buffer.data();
            const std::uint32_t length = buffer.Length();

            Advert copy = advert;
            copy.company = entry.CompanyId();
            copy.data.assign(bytes, bytes + length);
            copy.seenUnixMs = now;
            copy.apple = (copy.company == 0x004C);
            if (copy.apple && !copy.data.empty()) copy.appleType = copy.data[0];

            std::lock_guard lock(mutex);
            ++advertCount;
            if (copy.apple) {
                if (copy.name.find(L"AirPods") != std::wstring::npos) deviceName = copy.name;
                // Для GATT-запасного пути нужен адрес именно наушников (0x07),
                // а не любого Apple-устройства поблизости.
                if (copy.appleType == 0x07) lastAppleAddress = copy.address;
                // «На связи» — это именно реклама наушников с зарядом, а не
                // случайный Apple-гаджет рядом: иначе плитка висела бы вечно.
                MergeAppleAdvert(copy, now);
            }

            auto it = adverts.find(copy.address);
            if (it == adverts.end() && adverts.size() >= kMaxAdvertRecords) {
                // Выкидываем самую старую запись, чтобы карта не росла в шумном эфире.
                auto oldest = adverts.begin();
                for (auto cur = adverts.begin(); cur != adverts.end(); ++cur) {
                    if (cur->second.seenUnixMs < oldest->second.seenUnixMs) oldest = cur;
                }
                adverts.erase(oldest);
            }
            Advert& stored = adverts[copy.address];
            const std::wstring keepName = stored.name;
            stored = std::move(copy);
            if (stored.name.empty()) stored.name = keepName;
        }
    }

    bool ScanOnce() {
        using namespace winrt::Windows::Devices::Bluetooth::Advertisement;

        try {
            BluetoothLEAdvertisementWatcher watcher;
            // Пассивный скан: заряд лежит в самом объявлении, а активный режим
            // ещё и шлёт запросы — лишний радиообмен ни к чему.
            watcher.ScanningMode(BluetoothLEScanningMode::Passive);

            const auto token = watcher.Received(
                [this](const BluetoothLEAdvertisementWatcher&,
                       const BluetoothLEAdvertisementReceivedEventArgs& args) {
                    // Исключение не должно уходить через ABI в потоковый пул WinRT:
                    // это уронило бы Explorer вместе с виджетом.
                    try {
                        OnAdvertisement(args);
                    } catch (...) {
                    }
                });

            {
                std::lock_guard lock(mutex);
                scanning = true;
                ++scanCount;
                lastError.clear();
            }

            watcher.Start();

            const ULONGLONG deadline = GetTickCount64() + kScanWindowMs;
            while (GetTickCount64() < deadline) {
                if (WaitForSingleObject(stopEvent, 100) != WAIT_TIMEOUT) break;
                // Новое устройство или ручное обновление — не ждём конца окна.
                if (AppState::Instance().CheckAndResetAirPodsScan()) break;
                if (!AppState::Instance().IsAirPodsEnabled()) break;
                if (AppState::Instance().IsFullscreenAppActive()) break;
            }

            watcher.Stop();
            watcher.Received(token);

            {
                std::lock_guard lock(mutex);
                scanning = false;
            }
            return true;
        } catch (const winrt::hresult_error& error) {
            std::lock_guard lock(mutex);
            scanning = false;
            lastError = error.message().c_str();
            return false;
        } catch (...) {
            std::lock_guard lock(mutex);
            scanning = false;
            lastError = L"unknown WinRT error";
            return false;
        }
    }

    DWORD Run() {
        bool apartment = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
        } catch (const winrt::hresult_error& error) {
            std::lock_guard lock(mutex);
            lastError = L"WinRT init failed: " + std::wstring(error.message().c_str());
        }

        if (apartment) {
            StartBtWatcher();
        }

        while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
            if (!apartment) {
                WaitForSingleObject(stopEvent, 1000);
                continue;
            }

            if (!AppState::Instance().IsAirPodsEnabled()) {
                // Плитку скрыли вручную — опрос и состояние сбрасываем.
                PublishDisconnected();
                WaitForSingleObject(stopEvent, 500);
                continue;
            }

            if (AppState::Instance().IsFullscreenAppActive()) {
                // В полноэкранном приложении только не обновляем данные.
                // Состояние подключения и последние показания сохраняем, чтобы
                // после выхода плитка вернулась сразу, как у мыши.
                WaitForSingleObject(stopEvent, 500);
                continue;
            }

            ScanOnce();
            // Основной источник — реклама: в ней есть заряд всех трёх компонентов.
            // GATT остаётся запасным вариантом для прошивок без сообщения 0x07.
            bool haveBattery = false;
            std::uint64_t address = 0;
            {
                std::lock_guard lock(mutex);
                haveBattery = lastSuccessUnixMs != 0;
                address = lastAppleAddress;
            }
            if (!haveBattery) {
                if (address == 0) {
                    // AirPods в ушах могут не рекламироваться — берём адрес из
                    // списка сопряжённых BLE-устройств Windows.
                    EnumeratePairedBle();
                    std::lock_guard lock(mutex);
                    address = lastAppleAddress;
                }
                QueryGatt(address);
            }
            Publish();

            // Пауза между окнами сканирования; просыпаемся на события.
            const ULONGLONG until = GetTickCount64() + kScanPeriodMs;
            while (GetTickCount64() < until) {
                if (WaitForSingleObject(stopEvent, 250) != WAIT_TIMEOUT) goto finish;
                if (AppState::Instance().CheckAndResetAirPodsScan()) break;
                if (!AppState::Instance().IsAirPodsEnabled()) break;
                if (AppState::Instance().IsFullscreenAppActive()) break;
                // «connected» истекает по времени, даже когда рекламы нет.
                Publish();
            }
        }

    finish:
        StopBtWatcher();
        if (apartment) {
            try {
                winrt::uninit_apartment();
            } catch (...) {
            }
        }
        return 0;
    }

    static DWORD WINAPI ThreadProc(LPVOID param) {
        return static_cast<Impl*>(param)->Run();
    }
};

AirPodsMonitor& AirPodsMonitor::Instance() {
    static AirPodsMonitor monitor;
    return monitor;
}

AirPodsMonitor::~AirPodsMonitor() { Stop(); }

bool AirPodsMonitor::Start() {
    if (m_impl && m_impl->thread) return true;

    if (!m_impl) m_impl = new Impl();
    if (!m_impl->stopEvent) m_impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_impl->stopEvent) return false;

    m_impl->thread = CreateThread(nullptr, 0, Impl::ThreadProc, m_impl, 0, nullptr);
    if (!m_impl->thread) {
        CloseHandle(m_impl->stopEvent);
        m_impl->stopEvent = nullptr;
        return false;
    }
    return true;
}

void AirPodsMonitor::Stop() {
    if (!m_impl) return;
    if (m_impl->stopEvent) SetEvent(m_impl->stopEvent);
    if (m_impl->thread) {
        // Ждём до конца, а не по таймауту: удалять Impl, пока поток ещё
        // пользуется WinRT, нельзя — это use-after-free внутри Explorer.
        WaitForSingleObject(m_impl->thread, INFINITE);
        CloseHandle(m_impl->thread);
        m_impl->thread = nullptr;
    }
    if (m_impl->stopEvent) {
        CloseHandle(m_impl->stopEvent);
        m_impl->stopEvent = nullptr;
    }
    delete m_impl;
    m_impl = nullptr;
}

void AirPodsMonitor::AppendDiagnostics(std::wstring& out) const {
    wchar_t buffer[256]{};
    out += L"\r\n=== AirPods (Bluetooth LE) ===\r\n";

    swprintf_s(buffer, L"  setting: %ls\r\n",
               AppState::Instance().IsAirPodsEnabled() ? L"enabled" : L"disabled");
    out += buffer;

    if (!m_impl) {
        out += L"  monitor: not started\r\n";
        return;
    }

    std::lock_guard lock(m_impl->mutex);
    swprintf_s(buffer, L"  scan: %ls, windows: %d, advertisements: %d\r\n",
               m_impl->scanning ? L"running" : L"idle", m_impl->scanCount, m_impl->advertCount);
    out += buffer;
    swprintf_s(buffer, L"  last error: %ls\r\n",
               m_impl->lastError.empty() ? L"none" : m_impl->lastError.c_str());
    out += buffer;

    const AirPodsTelemetry telemetry = AppState::Instance().GetAirPods();
    swprintf_s(buffer, L"  detected: %ls  connected: %ls  name: %ls\r\n",
               telemetry.hasData ? L"yes" : L"no",
               telemetry.connected ? L"yes" : L"no",
               telemetry.deviceName.empty() ? L"(unknown)" : telemetry.deviceName.c_str());
    out += buffer;
    swprintf_s(buffer, L"  last advertisement: %ls (%ls)\r\n",
               LocalTimeText(telemetry.lastSeenUnixMs).c_str(),
               AgeText(telemetry.lastSeenUnixMs).c_str());
    out += buffer;
    swprintf_s(buffer, L"  last successful parse: %ls (%ls)\r\n",
               LocalTimeText(telemetry.lastSuccessUnixMs).c_str(),
               AgeText(telemetry.lastSuccessUnixMs).c_str());
    out += buffer;

    swprintf_s(buffer, L"  bt watcher: %ls, device connected: %ls\r\n",
               m_impl->btWatcherReady ? L"ready" : (m_impl->btWatcherFailed ? L"failed" : L"starting"),
               m_impl->deviceConnected ? L"yes" : L"no");
    out += buffer;
    for (const auto& [id, name] : m_impl->connectedBtDevices) {
        out += L"    connected: " + name + L"  id=" + id + L"\r\n";
    }

    auto levelText = [](int level, bool charging) -> std::wstring {
        if (level < 0) return L"unknown";
        std::wstring text = std::to_wstring(level) + L"%";
        if (charging) text += L" (charging)";
        return text;
    };
    swprintf_s(buffer, L"  left: %ls\r\n", levelText(telemetry.leftLevel, telemetry.leftCharging).c_str());
    out += buffer;
    swprintf_s(buffer, L"  right: %ls\r\n", levelText(telemetry.rightLevel, telemetry.rightCharging).c_str());
    out += buffer;
    swprintf_s(buffer, L"  case: %ls\r\n", levelText(telemetry.caseLevel, telemetry.caseCharging).c_str());
    out += buffer;

    out += L"  paired BLE devices:\r\n";
    if (m_impl->deviceList.empty()) {
        out += L"    (none or enumeration not run yet)\r\n";
    }
    int shown = 0;
    for (const Impl::DeviceRecord& device : m_impl->deviceList) {
        swprintf_s(buffer, L"    %ls  name=\"%ls\"  id=%ls\r\n", AddressText(device.address).c_str(),
                   device.name.empty() ? L"" : device.name.c_str(), device.id.c_str());
        out += buffer;
        if (++shown >= 12) {
            out += L"    ... (truncated)\r\n";
            break;
        }
    }

    out += L"  GATT query (last attempt):\r\n";
    if (m_impl->gatt.queriedMs == 0) {
        out += L"    (not queried yet)\r\n";
    } else {
        swprintf_s(buffer, L"    address=%ls  name=\"%ls\"  access=%ls  status=%ls  queried=%ls\r\n",
                   AddressText(m_impl->gatt.address).c_str(),
                   m_impl->gatt.name.empty() ? L"" : m_impl->gatt.name.c_str(),
                   m_impl->gatt.access.empty() ? L"?" : m_impl->gatt.access.c_str(),
                   m_impl->gatt.status.empty() ? L"?" : m_impl->gatt.status.c_str(),
                   AgeText(m_impl->gatt.queriedMs).c_str());
        out += buffer;
        if (!m_impl->gatt.error.empty()) {
            out += L"    error: " + m_impl->gatt.error + L"\r\n";
        }
        int shown = 0;
        for (const Impl::GattEntry& entry : m_impl->gatt.entries) {
            swprintf_s(buffer, L"    service=%ls char=%ls props=%ls level=%d desc=\"%ls\" status=%ls hex=%ls\r\n",
                       entry.service.c_str(), entry.characteristic.c_str(),
                       entry.properties.empty() ? L"-" : entry.properties.c_str(), entry.level,
                       entry.description.c_str(), entry.status.c_str(),
                       entry.hex.empty() ? L"-" : entry.hex.c_str());
            out += buffer;
            if (++shown >= 16) {
                out += L"    ... (truncated)\r\n";
                break;
            }
        }
        if (m_impl->gatt.entries.empty()) {
            out += L"    (no Battery Service characteristics)\r\n";
        }
    }

    out += L"  apple advertisements:\r\n";
    int appleCount = 0;
    for (const auto& [address, advert] : m_impl->adverts) {
        if (!advert.apple) continue;
        ++appleCount;
        swprintf_s(buffer,
                   L"    %ls  rssi=%d  company=0x%04X  type=0x%02X  len=%zu  name=\"%ls\"  seen=%ls\r\n",
                   AddressText(address).c_str(), advert.rssi, advert.company, advert.appleType,
                   advert.data.size(), advert.name.empty() ? L"" : advert.name.c_str(),
                   AgeText(advert.seenUnixMs).c_str());
        out += buffer;
        out += L"      hex: " + HexBytes(advert.data) + L"\r\n";
        if (appleCount >= 12) {
            out += L"    ... (truncated)\r\n";
            break;
        }
    }
    if (appleCount == 0) {
        out += L"    (no Apple advertisements seen yet)\r\n";
    }

    out += L"  layout: message 0x07, model=[3..4], status=[5], pods=[6], case/flags=[7], lid=[8];\r\n";
    out += L"          nibble 0..9 -> 0..90%, A..E -> 100%, F -> unknown;\r\n";
    out += L"          upper pods nibble belongs to the primary pod (status bit 5 = left).\r\n";
}
