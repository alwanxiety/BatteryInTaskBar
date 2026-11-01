#include "tools/HidDump.h"
#include "core/AppPaths.h"
#include "core/Localization.h"
#include "hid/BekenStatusProtocol.h"
#include "hid/MouseProviders.h"
#include "res/app_version.h"
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <cstdarg>
#include <cstdint>
#include <cwchar>
#include <vector>

namespace {

constexpr DWORD kDeviceIdChars = 256;  // хватает на MAX_DEVICE_ID_LEN и запас

void Append(std::wstring& out, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int length = _vscwprintf(format, args);
    va_end(args);
    if (length < 0) return;

    // Буфер по фактическому размеру: длина путей и строк устройства заранее неизвестна.
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    va_start(args, format);
    vswprintf_s(buffer.data(), buffer.size(), format, args);
    va_end(args);

    buffer.resize(static_cast<std::size_t>(length));
    out += buffer;
}

template <typename Fn>
std::wstring QueryHidString(HANDLE dev, Fn fn) {
    wchar_t buffer[256]{};
    if (!fn(dev, buffer, static_cast<ULONG>(sizeof(buffer)))) return {};
    return buffer;
}

// SEH нельзя включать в функции с объектами, требующими раскрутки стека,
// поэтому вызов провайдера вынесен в отдельный кадр без локальных объектов.
bool SafeQuery(IMouseProvider& provider, HANDLE dev, const HIDP_CAPS& caps,
               DeviceTelemetry& data) noexcept {
    __try { return provider.Query(dev, caps, data); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring BoolText(bool value) {
    return value ? L"yes" : L"no";
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

bool ReadOneFrame(HANDLE dev, const HIDP_CAPS& caps, std::vector<std::uint8_t>& frame,
                  DWORD timeoutMs) {
    if (caps.InputReportByteLength == 0) return false;

    std::vector<std::uint8_t> buffer(caps.InputReportByteLength, 0);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    DWORD read = 0;
    bool ok = false;
    if (ReadFile(dev, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &ov)) {
        ok = read > 0;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(dev, &ov, &read, FALSE) != FALSE && read > 0;
        } else {
            CancelIoEx(dev, &ov);
            DWORD ignored = 0;
            GetOverlappedResult(dev, &ov, &ignored, TRUE);
        }
    }
    CloseHandle(ov.hEvent);
    if (ok) {
        buffer.resize(read < buffer.size() ? read : static_cast<DWORD>(buffer.size()));
        frame = std::move(buffer);
    }
    return ok;
}

void DumpHidDevices(std::wstring& out) {
    Append(out, L"\r\n=== HID interfaces ===\r\n");

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        Append(out, L"  enumeration failed, error %lu\r\n", GetLastError());
        return;
    }

    LogitechProvider logitech;
    RazerProvider razer;
    BekenStatusProvider beken;
    RedSquareProvider redSquare;
    CompxNordicProvider compx;
    GenericBatteryProvider generic;
    IMouseProvider* providers[] = { &logitech, &razer, &beken, &redSquare, &compx, &generic };

    int index = 0;
    int matchedTotal = 0;
    int answeredTotal = 0;

    SP_DEVICE_INTERFACE_DATA iface{ sizeof(SP_DEVICE_INTERFACE_DATA) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &iface); ++i) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) continue;

        std::vector<std::uintptr_t> buffer(
            (requiredSize + sizeof(std::uintptr_t) - 1) / sizeof(std::uintptr_t));
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfoData{ sizeof(SP_DEVINFO_DATA) };
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, requiredSize, nullptr, &devInfoData)) continue;

        // Сначала пробуем с полным доступом (он нужен провайдерам), затем только
        // чтение (часть коллекций мыши система держит read-only), затем без прав
        // — для атрибутов и дескриптора.
        HANDLE dev = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL, nullptr);
        bool readable = dev != INVALID_HANDLE_VALUE;
        if (!readable) {
            dev = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            readable = dev != INVALID_HANDLE_VALUE;
        }
        if (!readable) {
            dev = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (dev == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr{ sizeof(HIDD_ATTRIBUTES) };
        if (!HidD_GetAttributes(dev, &attr)) {
            CloseHandle(dev);
            continue;
        }

        HIDP_CAPS caps{};
        bool haveCaps = false;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetPreparsedData(dev, &preparsed)) {
            haveCaps = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
            HidD_FreePreparsedData(preparsed);
        }

        ++index;
        Append(out, L"\r\n[%d] VID_%04X&PID_%04X rev %04X  access=%ls\r\n", index,
               static_cast<unsigned>(attr.VendorID), static_cast<unsigned>(attr.ProductID),
               static_cast<unsigned>(attr.VersionNumber), readable ? L"rw" : L"read-only");
        Append(out, L"  path: %ls\r\n", detail->DevicePath);

        wchar_t instanceId[kDeviceIdChars]{};
        if (SetupDiGetDeviceInstanceIdW(set, &devInfoData, instanceId, kDeviceIdChars, nullptr)) {
            Append(out, L"  instance: %ls\r\n", instanceId);
        }

        const std::wstring product = QueryHidString(dev, HidD_GetProductString);
        const std::wstring manufacturer = QueryHidString(dev, HidD_GetManufacturerString);
        const std::wstring serial = QueryHidString(dev, HidD_GetSerialNumberString);
        Append(out, L"  product: %ls\r\n", product.empty() ? L"(none)" : product.c_str());
        Append(out, L"  manufacturer: %ls\r\n", manufacturer.empty() ? L"(none)" : manufacturer.c_str());
        if (!serial.empty()) Append(out, L"  serial: %ls\r\n", serial.c_str());

        if (haveCaps) {
            Append(out, L"  usage: page=0x%04X usage=0x%04X\r\n", static_cast<unsigned>(caps.UsagePage),
                   static_cast<unsigned>(caps.Usage));
            Append(out, L"  reports: input=%u output=%u feature=%u\r\n",
                   static_cast<unsigned>(caps.InputReportByteLength),
                   static_cast<unsigned>(caps.OutputReportByteLength),
                   static_cast<unsigned>(caps.FeatureReportByteLength));
        } else {
            Append(out, L"  reports: unknown (preparsed data unavailable)\r\n");
        }

        // Сырые отчёты: без них нельзя добавить поддержку незнакомого
        // устройства (например, Attack Shark X6 с 262-байтовым feature-отчётом).
        if (haveCaps && readable) {
            if (caps.InputReportByteLength > 0 && caps.InputReportByteLength <= 4096) {
                std::vector<std::uint8_t> input(caps.InputReportByteLength, 0);
                if (HidD_GetInputReport(dev, input.data(), static_cast<ULONG>(input.size()))) {
                    Append(out, L"  raw input: %ls\r\n", HexBytes(input).c_str());
                }
            }
            if (caps.FeatureReportByteLength > 0 && caps.FeatureReportByteLength <= 4096) {
                for (int id = 0; id <= 0x0F; ++id) {
                    std::vector<std::uint8_t> feature(caps.FeatureReportByteLength, 0);
                    feature[0] = static_cast<std::uint8_t>(id);
                    if (HidD_GetFeature(dev, feature.data(), static_cast<ULONG>(feature.size()))) {
                        Append(out, L"  raw feature 0x%02X: %ls\r\n", id, HexBytes(feature).c_str());
                    }
                }
            }
        }

        // Beken/Inphic присылает заряд сам в interrupt-IN на 0x000A:0x0000.
        // Слушаем поток 3 с — это единственный способ увидеть кадры на мыши,
        // которая не отвечает на feature-запросы.
        if (haveCaps && caps.UsagePage == 0x000A && caps.Usage == 0x0000 &&
            caps.InputReportByteLength >= 5 && caps.InputReportByteLength <= 64) {
            HANDLE listenDev = CreateFileW(
                detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL, nullptr);
            if (listenDev == INVALID_HANDLE_VALUE) {
                listenDev = CreateFileW(detail->DevicePath, GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (listenDev == INVALID_HANDLE_VALUE) {
                listenDev = CreateFileW(detail->DevicePath, 0,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (listenDev != INVALID_HANDLE_VALUE) {
                Append(out, L"  listening for status frames (3 s)...\r\n");
                std::vector<std::uint8_t> previous;
                int seen = 0;
                const ULONGLONG deadline = GetTickCount64() + 3000;
                while (GetTickCount64() < deadline && seen < 8) {
                    std::vector<std::uint8_t> frame;
                    if (!ReadOneFrame(listenDev, caps, frame, 300)) continue;
                    if (frame == previous) continue;  // поток повторяет один и тот же кадр
                    previous = frame;
                    BekenStatusProtocol::Reading reading;
                    const bool isBattery =
                        BekenStatusProtocol::Parse(attr.ProductID, frame, reading);
                    std::wstring note;
                    if (isBattery) {
                        wchar_t noteBuffer[64]{};
                        if (reading.level >= 0) {
                            swprintf_s(noteBuffer, L"  -> %d%%%ls", reading.level,
                                       reading.charging ? L", charging" : L"");
                        } else {
                            swprintf_s(noteBuffer, L"  -> charging (level from previous frame)");
                        }
                        note = noteBuffer;
                    }
                    Append(out, L"  frame: %ls%ls\r\n", HexBytes(frame).c_str(), note.c_str());
                    ++seen;
                }
                if (seen == 0) Append(out, L"  no frames in the window\r\n");
                CloseHandle(listenDev);
            }
        }

        bool answered = false;
        for (IMouseProvider* provider : providers) {
            if (!provider->Match(attr.VendorID, attr.ProductID)) continue;
            // Generic-провайдер цепляется по описанию: без подходящего поля
            // Battery Strength он не должен светиться на каждом интерфейсе.
            if (haveCaps && !provider->MatchDevice(dev, caps)) continue;
            ++matchedTotal;
            Append(out, L"  provider matched: %ls\r\n", provider->Name());
            if (!readable || !haveCaps) {
                Append(out, L"    query: skipped (%ls)\r\n",
                       readable ? L"no caps" : L"device opened read-only");
                continue;
            }
            DeviceTelemetry data;
            const bool ok = SafeQuery(*provider, dev, caps, data);
            if (!ok) {
                Append(out, L"    query: no answer\r\n");
                continue;
            }
            answered = true;
            Append(out, L"    query: OK level=%d charging=%ls voltage=%d name=\"%ls\"\r\n",
                   data.batteryLevel, BoolText(data.isCharging).c_str(), data.voltageMv,
                   data.deviceName.empty() ? L"(unknown)" : data.deviceName.c_str());
        }
        if (answered) ++answeredTotal;

        CloseHandle(dev);
    }

    SetupDiDestroyDeviceInfoList(set);

    Append(out, L"\r\n=== HID summary ===\r\n");
    Append(out, L"  HID interfaces: %d\r\n", index);
    Append(out, L"  provider matches: %d\r\n", matchedTotal);
    Append(out, L"  interfaces that answered: %d\r\n", answeredTotal);
}

void DumpBluetoothBattery(std::wstring& out) {
    Append(out, L"\r\n=== PnP battery fallback (HID/BT) ===\r\n");
    DeviceTelemetry data;
    if (!PnpBatteryProvider::Query(data)) {
        Append(out, L"  no device with battery level found\r\n");
        return;
    }
    Append(out, L"  name: %ls\r\n", data.deviceName.empty() ? L"(unknown)" : data.deviceName.c_str());
    Append(out, L"  level: %d%%  charging=%ls  voltage=%d\r\n", data.batteryLevel,
           BoolText(data.isCharging).c_str(), data.voltageMv);
}

}  // namespace

void HidDump::AppendTo(std::wstring& out) {
    DumpHidDevices(out);
    DumpBluetoothBattery(out);
}

std::wstring HidDump::WriteReport() {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    std::wstring text;
    Append(text, L"BatteryInTaskBar - HID diagnostics\r\n");
    Append(text, L"Date: %04u-%02u-%02u %02u:%02u:%02u\r\n",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Append(text, L"Language: %ls\r\n", L10n::LanguageCode());
    Append(text, L"Version: %ls\r\n", APP_VERSION_WSTRING);
    Append(text, L"Send this file to the developer to add support for a new device.\r\n");

    AppendTo(text);

    wchar_t fileName[64]{};
    swprintf_s(fileName, L"hid-dump-%04u%02u%02u-%02u%02u%02u.txt",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const std::wstring path = AppPaths::DataFile(fileName);
    if (path.empty() || !AppPaths::WriteUtf8File(path, text)) return {};
    return path;
}
