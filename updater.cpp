#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <winver.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "core/AppPaths.h"
#include "core/Localization.h"
#include "core/UpdateState.h"
#include "res/app_version.h"
#include "ui/WidgetIdentity.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winhttp.lib")

namespace {

// --- Общие константы -------------------------------------------------------

constexpr wchar_t kApiUrl[] =
    L"https://api.github.com/repos/alwanxiety/BatteryInTaskBar/releases/latest";
constexpr wchar_t kReleasePage[] =
    L"https://github.com/alwanxiety/BatteryInTaskBar/releases/latest";

constexpr wchar_t kUpdaterMutexName[] = L"Local\\BatteryInTaskBar.Updater";
constexpr wchar_t kWidgetMutexName[] = L"Local\\BatteryInTaskBar.Widget.Instance";
constexpr wchar_t kWatchdogMutexName[] = L"Local\\BatteryInTaskBar.Injector";
constexpr wchar_t kUserExitEventName[] = L"Local\\BatteryInTaskBar.Exit";

constexpr wchar_t kDllName[] = L"taskbar_mod.dll";
constexpr wchar_t kInjectorName[] = L"injector.exe";
constexpr wchar_t kChecksumsName[] = L"SHA256SUMS.txt";
constexpr wchar_t kBackupSuffix[] = L".bak";

constexpr UINT WM_APP_CHECK_DONE = WM_APP + 1;
constexpr UINT WM_APP_UPDATE_STATUS = WM_APP + 2;
constexpr UINT WM_APP_UPDATE_DONE = WM_APP + 3;

constexpr UINT_PTR TIMER_AUTOCLOSE = 1;
constexpr UINT_PTR TIMER_PROGRESS = 2;

constexpr int ID_STATUS = 100;
constexpr int ID_PROGRESS = 101;
constexpr int ID_UPDATE = 102;
constexpr int ID_LATER = 103;
constexpr int ID_CLOSE = 104;
constexpr int ID_OPEN_RELEASE = 105;

constexpr DWORD kWatchdogStopTimeoutMs = 12000;
constexpr DWORD kWidgetCloseTimeoutMs = 18000;
constexpr DWORD kDllUnloadTimeoutMs = 20000;
constexpr DWORD kWatcherStartTimeoutMs = 12000;

// --- Логирование -----------------------------------------------------------

CRITICAL_SECTION g_logLock;
bool g_logLockReady = false;

std::wstring FormatText(const wchar_t* format, ...) {
    if (!format) return {};
    va_list args;
    va_start(args, format);
    const int length = _vscwprintf(format, args);
    va_end(args);
    if (length < 0) return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    va_start(args, format);
    vswprintf_s(text.data(), text.size(), format, args);
    va_end(args);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), chars);
    return wide;
}

std::uint64_t NowUnixMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void LogLine(const wchar_t* format, ...) {
    if (!g_logLockReady || !format) return;

    va_list args;
    va_start(args, format);
    wchar_t message[2048]{};
    vswprintf_s(message, format, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const std::wstring line = FormatText(
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %ls\r\n", st.wYear, st.wMonth,
        st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);

    const std::string utf8 = WideToUtf8(line);
    if (utf8.empty()) return;

    EnterCriticalSection(&g_logLock);
    const std::wstring path = AppPaths::DataFile(L"updater.log");
    if (!path.empty()) {
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                      nullptr);
            CloseHandle(file);
        }
    }
    LeaveCriticalSection(&g_logLock);
}

std::wstring Win32ErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text;
    if (length && buffer) text.assign(buffer, length);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                             text.back() == L' ' || text.back() == L'\t')) {
        text.pop_back();
    }
    if (text.empty()) text = FormatText(L"error %lu", error);
    return text;
}

// --- Файлы -----------------------------------------------------------------

bool FileExists(const std::wstring& path) {
    return !path.empty() &&
           GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring FileName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ModuleDirectory() {
    std::vector<WCHAR> buffer(MAX_PATH);
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
    std::wstring path(buffer.data(), buffer.size());
    const auto slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    path.resize(slash + 1);
    return path;
}

/// Версия установленного виджета, а не самого updater.exe. Иначе старый
/// обновлятор после установки 1.0.7-beta2 продолжит видеть «доступно 1.0.7-beta2».
std::wstring InstalledAppVersion() {
    const std::wstring dll = ModuleDirectory() + kDllName;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(dll.c_str(), &ignored);
    if (size == 0) return APP_VERSION_WSTRING;

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(dll.c_str(), 0, size, data.data())) {
        return APP_VERSION_WSTRING;
    }

    VS_FIXEDFILEINFO* version = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&version),
                        &length) ||
        !version) {
        return APP_VERSION_WSTRING;
    }

    wchar_t text[32]{};
    swprintf_s(text, L"%u.%u.%u.%u",
               HIWORD(version->dwFileVersionMS),
               LOWORD(version->dwFileVersionMS),
               HIWORD(version->dwFileVersionLS),
               LOWORD(version->dwFileVersionLS));
    return text;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring current;
    for (std::size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if ((path[i] == L'\\' || i + 1 == path.size()) && current.size() > 3) {
            CreateDirectoryW(current.c_str(), nullptr);
        }
    }
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring SanitizeTag(const std::wstring& tag) {
    std::wstring result;
    for (const wchar_t c : tag) {
        if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
            (c >= L'A' && c <= L'Z') || c == L'.' || c == L'-' || c == L'_') {
            result.push_back(c);
        } else {
            result.push_back(L'_');
        }
        if (result.size() >= 64) break;
    }
    return result.empty() ? std::wstring(L"latest") : result;
}

bool ReadTextFileUtf8(const std::wstring& path, std::string& text,
                      std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = FormatText(L"%ls: %ls", path.c_str(), Win32ErrorText(GetLastError()).c_str());
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 20 * 1024 * 1024) {
        CloseHandle(file);
        error = FormatText(L"%ls: bad file size", path.c_str());
        return false;
    }

    text.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    DWORD offset = 0;
    while (offset < text.size()) {
        if (!ReadFile(file, text.data() + offset,
                      static_cast<DWORD>(std::min<std::size_t>(
                          text.size() - offset, 64 * 1024)),
                      &read, nullptr)) {
            error = FormatText(L"%ls: %ls", path.c_str(),
                               Win32ErrorText(GetLastError()).c_str());
            CloseHandle(file);
            return false;
        }
        if (read == 0) break;
        offset += read;
    }
    CloseHandle(file);
    text.resize(offset);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return true;
}

// --- HTTP ------------------------------------------------------------------

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool CrackUrl(const std::wstring& url, UrlParts& parts, std::wstring& error) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0,
                         &components)) {
        error = L"invalid URL";
        return false;
    }

    parts.host.assign(host, components.dwHostNameLength);
    parts.path.assign(path, components.dwUrlPathLength);
    if (parts.path.empty()) parts.path = L"/";
    parts.port = components.nPort;
    parts.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    if (parts.host.empty()) {
        error = L"URL has no host";
        return false;
    }
    return true;
}

using ChunkSink = std::function<bool(const char*, DWORD, std::wstring&)>;

bool HttpGet(const std::wstring& url, const std::wstring& extraHeaders,
             const ChunkSink& sink, std::wstring& error) {
    UrlParts parts;
    if (!CrackUrl(url, parts, error)) return false;

    HINTERNET session = WinHttpOpen(
        L"BatteryInTaskBar-Updater/" APP_VERSION_WSTRING,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = FormatText(L"WinHttpOpen: %ls",
                           Win32ErrorText(GetLastError()).c_str());
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                     sizeof(protocols));

    HINTERNET connect = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!connect) {
        error = FormatText(L"WinHttpConnect: %ls",
                           Win32ErrorText(GetLastError()).c_str());
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        error = FormatText(L"WinHttpOpenRequest: %ls",
                           Win32ErrorText(GetLastError()).c_str());
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect,
                     sizeof(redirect));

    std::wstring headers = L"User-Agent: BatteryInTaskBar-Updater/" APP_VERSION_WSTRING L"\r\n";
    headers += extraHeaders;

    bool ok = WinHttpSendRequest(
                  request, headers.c_str(), static_cast<DWORD>(-1L),
                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE &&
              WinHttpReceiveResponse(request, nullptr) != FALSE;
    if (!ok) {
        error = FormatText(L"HTTP request: %ls",
                           Win32ErrorText(GetLastError()).c_str());
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok && (!WinHttpQueryHeaders(
                   request,
                   WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                   WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                   WINHTTP_NO_HEADER_INDEX) ||
               status != 200)) {
        error = FormatText(L"HTTP status %lu", status);
        ok = false;
    }

    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = FormatText(L"WinHttpQueryDataAvailable: %ls",
                               Win32ErrorText(GetLastError()).c_str());
            ok = false;
            break;
        }
        if (available == 0) break;

        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            error = FormatText(L"WinHttpReadData: %ls",
                               Win32ErrorText(GetLastError()).c_str());
            ok = false;
            break;
        }
        if (read == 0) break;
        chunk.resize(read);
        if (!sink(chunk.data(), read, error)) {
            ok = false;
            break;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

bool HttpGetText(const std::wstring& url, std::string& text,
                 std::wstring& error) {
    text.clear();
    return HttpGet(
        url, L"Accept: application/vnd.github+json\r\n",
        [&](const char* data, DWORD size, std::wstring&) {
            if (text.size() + size > 20 * 1024 * 1024) {
                error = L"response is too large";
                return false;
            }
            text.append(data, size);
            return true;
        },
        error);
}

bool HttpGetFile(const std::wstring& url, const std::wstring& path,
                 std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = FormatText(L"%ls: %ls", FileName(path).c_str(),
                           Win32ErrorText(GetLastError()).c_str());
        return false;
    }

    bool ok = HttpGet(
        url, L"",
        [&](const char* data, DWORD size, std::wstring&) {
            DWORD written = 0;
            if (!WriteFile(file, data, size, &written, nullptr) ||
                written != size) {
                error = FormatText(L"%ls: %ls", FileName(path).c_str(),
                                   Win32ErrorText(GetLastError()).c_str());
                return false;
            }
            return true;
        },
        error);
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// --- JSON и версии ---------------------------------------------------------

std::string FindJsonString(const std::string& json, std::size_t from,
                           const char* key) {
    const std::size_t keyPos = json.find(key, from);
    if (keyPos == std::string::npos) return {};
    std::size_t pos = keyPos + std::strlen(key);
    while (pos < json.size() &&
           std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != ':') return {};
    ++pos;
    while (pos < json.size() &&
           std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;

    std::string value;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') break;
        if (c == '\\' && pos < json.size()) {
            const char escaped = json[pos++];
            switch (escaped) {
            case '"': value += '"'; break;
            case '\\': value += '\\'; break;
            case '/': value += '/'; break;
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            default: value += escaped; break;
            }
        } else {
            value += c;
        }
    }
    return value;
}

struct Asset {
    std::wstring name;
    std::wstring url;
};

struct ReleaseInfo {
    std::wstring tag;
    std::wstring version;
    std::vector<Asset> assets;
};

bool ParseAssets(const std::string& json, std::vector<Asset>& assets) {
    const std::size_t assetsPos = json.find("\"assets\"");
    if (assetsPos == std::string::npos) return false;
    std::size_t pos = json.find('[', assetsPos);
    if (pos == std::string::npos) return false;
    ++pos;

    while (pos < json.size()) {
        const std::size_t objectStart = json.find('{', pos);
        if (objectStart == std::string::npos) break;

        int depth = 0;
        bool inString = false;
        bool escaped = false;
        std::size_t objectEnd = std::string::npos;
        for (std::size_t i = objectStart; i < json.size(); ++i) {
            const char c = json[i];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
            } else if (c == '"') {
                inString = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    objectEnd = i;
                    break;
                }
            }
        }
        if (objectEnd == std::string::npos) break;

        const std::string object = json.substr(objectStart, objectEnd - objectStart + 1);
        const std::string name = FindJsonString(object, 0, "\"name\"");
        const std::string url =
            FindJsonString(object, 0, "\"browser_download_url\"");
        if (!name.empty() && !url.empty()) {
            assets.push_back({ Utf8ToWide(name), Utf8ToWide(url) });
        }

        pos = objectEnd + 1;
        while (pos < json.size() &&
               std::isspace(static_cast<unsigned char>(json[pos]))) {
            ++pos;
        }
        if (pos < json.size() && json[pos] == ']') break;
    }
    return !assets.empty();
}

bool ParseRelease(const std::string& json, ReleaseInfo& info,
                  std::wstring& error) {
    const std::string tag = FindJsonString(json, 0, "\"tag_name\"");
    if (tag.empty()) {
        error = L"release JSON has no tag_name";
        return false;
    }
    info.tag = Utf8ToWide(tag);
    info.version = info.tag;
    if (!info.version.empty() &&
        (info.version.front() == L'v' || info.version.front() == L'V')) {
        info.version.erase(0, 1);
    }
    if (!ParseAssets(json, info.assets)) {
        error = L"release JSON has no assets";
        return false;
    }
    return true;
}

std::vector<int> ParseVersion(const std::wstring& version) {
    std::vector<int> parts;
    int value = 0;
    bool haveValue = false;
    for (const wchar_t c : version) {
        if (c >= L'0' && c <= L'9') {
            value = value * 10 + static_cast<int>(c - L'0');
            haveValue = true;
        } else if (c == L'.') {
            parts.push_back(haveValue ? value : 0);
            value = 0;
            haveValue = false;
            if (parts.size() >= 4) break;
        } else if (haveValue) {
            break;
        }
    }
    if (haveValue && parts.size() < 4) parts.push_back(value);
    while (parts.size() < 3) parts.push_back(0);

    // У первой опубликованной 1.0.7 beta в ресурсах стояла версия 1.0.7.0.
    // Чтобы 1.0.7-beta2 считалась новее, номер после «beta» кладём в четвёртый
    // компонент: beta2 -> 1.0.7.2. Старый updater из первой беты суффикс не
    // понимает, поэтому ей всё равно нужна установка ZIP вручную.
    const std::size_t beta = version.find(L"beta");
    if (beta != std::wstring::npos) {
        std::size_t pos = beta + 4;
        int number = 0;
        bool haveNumber = false;
        while (pos < version.size() && version[pos] >= L'0' && version[pos] <= L'9') {
            number = number * 10 + static_cast<int>(version[pos] - L'0');
            haveNumber = true;
            ++pos;
        }
        if (parts.size() < 4) {
            parts.push_back(haveNumber ? number : 0);
        } else if (!haveNumber) {
            parts[3] = 0;
        }
    }
    while (parts.size() < 4) parts.push_back(0);
    return parts;
}

int CompareVersions(const std::wstring& left, const std::wstring& right) {
    const std::vector<int> a = ParseVersion(left);
    const std::vector<int> b = ParseVersion(right);
    for (std::size_t i = 0; i < 4; ++i) {
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

const Asset* FindAsset(const ReleaseInfo& info, const wchar_t* name) {
    for (const Asset& asset : info.assets) {
        if (_wcsicmp(asset.name.c_str(), name) == 0) return &asset;
    }
    return nullptr;
}

// --- SHA-256 ---------------------------------------------------------------

bool Sha256File(const std::wstring& path, std::wstring& hex,
                std::wstring& error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool ok = false;

    NTSTATUS status =
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = FormatText(L"BCryptOpenAlgorithmProvider: 0x%08lX",
                           static_cast<unsigned long>(status));
        return false;
    }

    DWORD hashLength = 0;
    DWORD resultLength = 0;
    status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashLength),
                               sizeof(hashLength), &resultLength, 0);
    if (!BCRYPT_SUCCESS(status) || hashLength == 0) {
        error = L"BCryptGetProperty failed";
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error = L"BCryptCreateHash failed";
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = FormatText(L"%ls: %ls", FileName(path).c_str(),
                           Win32ErrorText(GetLastError()).c_str());
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    std::vector<BYTE> buffer(64 * 1024);
    DWORD read = 0;
    for (;;) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
            error = FormatText(L"%ls: %ls", FileName(path).c_str(),
                               Win32ErrorText(GetLastError()).c_str());
            break;
        }
        if (read == 0) {
            std::vector<BYTE> digest(hashLength);
            status = BCryptFinishHash(hash, digest.data(),
                                      static_cast<ULONG>(digest.size()), 0);
            if (!BCRYPT_SUCCESS(status)) {
                error = L"BCryptFinishHash failed";
                break;
            }
            static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
            hex.clear();
            hex.reserve(digest.size() * 2);
            for (const BYTE byte : digest) {
                hex.push_back(kHex[byte >> 4]);
                hex.push_back(kHex[byte & 0x0F]);
            }
            ok = true;
            break;
        }
        status = BCryptHashData(hash, buffer.data(), read, 0);
        if (!BCRYPT_SUCCESS(status)) {
            error = L"BCryptHashData failed";
            break;
        }
    }

    CloseHandle(file);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool ParseChecksums(const std::wstring& path,
                    std::map<std::wstring, std::wstring>& sums,
                    std::wstring& error) {
    std::string text;
    if (!ReadTextFileUtf8(path, text, error)) return false;

    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        const std::size_t space = line.find_first_of(" \t");
        if (space == std::string::npos) continue;
        std::string hash = line.substr(0, space);
        std::string name = line.substr(space);
        while (!name.empty() &&
               (name.front() == ' ' || name.front() == '\t')) {
            name.erase(name.begin());
        }
        if (!name.empty() && name.front() == '*') name.erase(name.begin());
        if (hash.empty() || name.empty()) continue;
        std::transform(hash.begin(), hash.end(), hash.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        sums[Utf8ToWide(name)] = Utf8ToWide(hash);
    }
    if (sums.empty()) {
        error = L"checksum file is empty";
        return false;
    }
    return true;
}

// --- Проверка обновления ---------------------------------------------------

std::wstring TestReleaseDirectory() {
    wchar_t buffer[32768]{};
    const DWORD length =
        GetEnvironmentVariableW(L"BIT_UPDATE_TEST_DIR", buffer,
                                static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    std::wstring path(buffer, length);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

enum class CheckResult { UpToDate, Available, Error };

CheckResult CheckLatestRelease(ReleaseInfo& info, std::wstring& error) {
    std::string json;
    const std::wstring testDir = TestReleaseDirectory();
    if (!testDir.empty()) {
        if (!ReadTextFileUtf8(testDir + L"\\release.json", json, error)) {
            return CheckResult::Error;
        }
    } else if (!HttpGetText(kApiUrl, json, error)) {
        return CheckResult::Error;
    }

    if (!ParseRelease(json, info, error)) return CheckResult::Error;

    if (CompareVersions(info.version, InstalledAppVersion()) <= 0) {
        return CheckResult::UpToDate;
    }

    for (const wchar_t* name : { kDllName, kInjectorName, kChecksumsName }) {
        if (!FindAsset(info, name)) {
            error = L10n::Get(Str::UpdateNoAssets);
            return CheckResult::Error;
        }
    }
    return CheckResult::Available;
}

bool DownloadAsset(const std::wstring& testDir, const ReleaseInfo& info,
                   const wchar_t* name, const std::wstring& destination,
                   std::wstring& error) {
    const Asset* asset = FindAsset(info, name);
    if (!asset) {
        error = L10n::Get(Str::UpdateNoAssets);
        return false;
    }
    if (!testDir.empty()) {
        const std::wstring source = testDir + L"\\" + name;
        if (!CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
            error = FormatText(L"%ls: %ls", name,
                               Win32ErrorText(GetLastError()).c_str());
            return false;
        }
        return true;
    }
    return HttpGetFile(asset->url, destination, error);
}

// --- Остановка работающего приложения -------------------------------------

DWORD GetExplorerPid() {
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{ sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (lstrcmpiW(entry.szExeFile, L"explorer.exe") != 0) continue;
            DWORD session = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session) ||
                session != mySession) {
                continue;
            }
            pid = entry.th32ProcessID;
            break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

bool IsModuleLoaded(DWORD pid, const wchar_t* moduleName) {
    if (pid == 0 || !moduleName) return false;
    HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    bool loaded = false;
    MODULEENTRY32W entry{ sizeof(MODULEENTRY32W) };
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                loaded = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return loaded;
}

bool IsNamedObjectPresent(const wchar_t* name) {
    HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (!handle) return false;
    CloseHandle(handle);
    return true;
}

bool WaitForMutexGone(const wchar_t* name, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (!IsNamedObjectPresent(name)) return true;
        Sleep(150);
    }
    return !IsNamedObjectPresent(name);
}

bool WaitForWidgetGone(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        HWND window = FindWindowW(nullptr, kWidgetWindowTitle);
        if (!window) window = FindWindowW(kLegacyWidgetClassName, nullptr);
        if (!window && !IsNamedObjectPresent(kWidgetMutexName)) return true;
        Sleep(150);
    }
    HWND window = FindWindowW(nullptr, kWidgetWindowTitle);
    if (!window) window = FindWindowW(kLegacyWidgetClassName, nullptr);
    return !window && !IsNamedObjectPresent(kWidgetMutexName);
}

bool WaitForModuleGone(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        const DWORD pid = GetExplorerPid();
        if (pid == 0 || !IsModuleLoaded(pid, kDllName)) return true;
        Sleep(200);
    }
    const DWORD pid = GetExplorerPid();
    return pid == 0 || !IsModuleLoaded(pid, kDllName);
}

bool StopRunningApp(std::wstring& error) {
    HANDLE exitEvent =
        OpenEventW(EVENT_MODIFY_STATE, FALSE, kUserExitEventName);
    if (exitEvent) {
        SetEvent(exitEvent);
        CloseHandle(exitEvent);
    }

    if (!WaitForMutexGone(kWatchdogMutexName, kWatchdogStopTimeoutMs)) {
        error = L10n::Get(Str::UpdateWidgetLoaded);
        return false;
    }

    HWND widget = FindWindowW(nullptr, kWidgetWindowTitle);
    if (!widget) widget = FindWindowW(kLegacyWidgetClassName, nullptr);
    if (widget) PostMessageW(widget, WM_CLOSE, 0, 0);

    if (!WaitForWidgetGone(kWidgetCloseTimeoutMs)) {
        error = L10n::Get(Str::UpdateWidgetLoaded);
        return false;
    }
    if (!WaitForModuleGone(kDllUnloadTimeoutMs)) {
        error = L10n::Get(Str::UpdateWidgetLoaded);
        return false;
    }
    return true;
}

// --- Замена файлов ---------------------------------------------------------

bool CanWriteInstallDirectory(const std::wstring& directory,
                              std::wstring& error) {
    const std::wstring probe = directory + L".updater-write-test";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L10n::Get(Str::UpdateNoWriteAccess);
        return false;
    }
    CloseHandle(file);
    DeleteFileW(probe.c_str());
    return true;
}

bool ReplaceWithBackup(const std::wstring& directory,
                       const std::wstring& staging, const wchar_t* name,
                       std::wstring& error) {
    const std::wstring target = directory + name;
    const std::wstring staged = staging + L"\\" + name;
    const std::wstring backup = target + kBackupSuffix;

    if (!FileExists(staged)) {
        error = FormatText(L"%ls: staged file not found", name);
        return false;
    }

    DeleteFileW(backup.c_str());
    if (FileExists(target) &&
        !CopyFileW(target.c_str(), backup.c_str(), FALSE)) {
        error = FormatText(L"%ls: backup: %ls", name,
                           Win32ErrorText(GetLastError()).c_str());
        return false;
    }

    for (int attempt = 0; attempt < 12; ++attempt) {
        if (MoveFileExW(staged.c_str(), target.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        Sleep(250);
    }

    error = FormatText(L10n::Get(Str::UpdateFileBusy), name);
    if (FileExists(backup)) {
        MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return false;
}

void RestoreBackup(const std::wstring& directory, const wchar_t* name) {
    const std::wstring target = directory + name;
    const std::wstring backup = target + kBackupSuffix;
    if (FileExists(backup)) {
        MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

bool StartInjector(const std::wstring& directory, std::wstring& error) {
    const std::wstring injector = directory + kInjectorName;
    if (!FileExists(injector)) {
        error = L"injector.exe is missing";
        return false;
    }
    std::wstring command = L"\"" + injector + L"\" --watch";
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(injector.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, directory.c_str(),
                        &startup, &process)) {
        error = FormatText(L"injector.exe: %ls",
                           Win32ErrorText(GetLastError()).c_str());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool WaitForWidgetStart(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        HWND window = FindWindowW(nullptr, kWidgetWindowTitle);
        if (!window) window = FindWindowW(kLegacyWidgetClassName, nullptr);
        if (window || IsNamedObjectPresent(kWidgetMutexName)) return true;
        Sleep(200);
    }
    return false;
}

// --- Установка -------------------------------------------------------------

using ProgressFn = std::function<void(const std::wstring&)>;

void Report(ProgressFn& progress, const wchar_t* format, ...) {
    if (!progress) return;
    va_list args;
    va_start(args, format);
    const int length = _vscwprintf(format, args);
    va_end(args);
    if (length < 0) return;
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    va_start(args, format);
    vswprintf_s(text.data(), text.size(), format, args);
    va_end(args);
    text.resize(static_cast<std::size_t>(length));
    progress(text);
}

bool ApplyUpdate(const std::wstring& installDir, const ReleaseInfo& release,
                 bool noStop, bool noStart, ProgressFn progress,
                 std::wstring& error) {
    const std::wstring testDir = TestReleaseDirectory();
    if (!CanWriteInstallDirectory(installDir, error)) return false;

    const std::wstring dataDir = AppPaths::DataDirectory();
    if (dataDir.empty()) {
        error = L"data directory is unavailable";
        return false;
    }
    const std::wstring staging =
        dataDir + L"\\update\\" + SanitizeTag(release.version);
    if (!EnsureDirectory(staging)) {
        error = L"could not create the update directory";
        return false;
    }

    for (const wchar_t* name : { kDllName, kInjectorName, kChecksumsName }) {
        DeleteFileW((staging + L"\\" + name).c_str());
    }

    Report(progress, L10n::Get(Str::UpdateDownloading), kChecksumsName);
    if (!DownloadAsset(testDir, release, kChecksumsName,
                       staging + L"\\" + kChecksumsName, error)) {
        return false;
    }

    std::map<std::wstring, std::wstring> sums;
    if (!ParseChecksums(staging + L"\\" + kChecksumsName, sums, error)) {
        return false;
    }

    for (const wchar_t* name : { kDllName, kInjectorName }) {
        Report(progress, L10n::Get(Str::UpdateDownloading), name);
        if (!DownloadAsset(testDir, release, name, staging + L"\\" + name,
                           error)) {
            return false;
        }
    }

    Report(progress, L"%ls", L10n::Get(Str::UpdateVerifying));
    for (const wchar_t* name : { kDllName, kInjectorName }) {
        const auto found = sums.find(name);
        if (found == sums.end()) {
            error = FormatText(L10n::Get(Str::UpdateChecksumMismatch), name);
            return false;
        }
        std::wstring actual;
        if (!Sha256File(staging + L"\\" + name, actual, error)) return false;
        if (_wcsicmp(actual.c_str(), found->second.c_str()) != 0) {
            error = FormatText(L10n::Get(Str::UpdateChecksumMismatch), name);
            return false;
        }
        LogLine(L"checksum ok: %ls %ls", name, actual.c_str());
    }

    if (!noStop) {
        Report(progress, L"%ls", L10n::Get(Str::UpdateInstalling));
        if (!StopRunningApp(error)) return false;
    }

    Report(progress, L"%ls", L10n::Get(Str::UpdateInstalling));
    if (!ReplaceWithBackup(installDir, staging, kDllName, error)) {
        RestoreBackup(installDir, kDllName);
        RestoreBackup(installDir, kInjectorName);
        return false;
    }
    if (!ReplaceWithBackup(installDir, staging, kInjectorName, error)) {
        RestoreBackup(installDir, kDllName);
        RestoreBackup(installDir, kInjectorName);
        return false;
    }

    if (!noStart) {
        Report(progress, L"%ls", L10n::Get(Str::UpdateRestarting));
        if (!StartInjector(installDir, error)) {
            RestoreBackup(installDir, kDllName);
            RestoreBackup(installDir, kInjectorName);
            StartInjector(installDir, error);
            return false;
        }
        const bool started = WaitForWidgetStart(kWatcherStartTimeoutMs);
        LogLine(L"widget started after update: %ls", started ? L"yes" : L"no");
    }

    LogLine(L"update installed: %ls", release.version.c_str());
    return true;
}

// --- Тихие режимы (виджет не показывает интерфейс обновлятора) -------------

int RunSilentCheck() {
    // Отдельный процесс: сеть не занимает поток интерфейса виджета. Не
    // блокируем повторные проверки мьютексом — ручная кнопка должна всегда
    // получить свежий CheckedAt, даже если фоновая проверка ещё идёт.
    ReleaseInfo release;
    std::wstring error;
    const CheckResult result = CheckLatestRelease(release, error);

    UpdateState::Info state;
    state.checkedAtUnixMs = NowUnixMs();
    if (result == CheckResult::Available) {
        state.available = true;
        state.latest = release.version;
        LogLine(L"silent check: update available %ls", release.version.c_str());
    } else if (result == CheckResult::UpToDate) {
        state.latest = InstalledAppVersion();
        LogLine(L"silent check: up to date %ls", state.latest.c_str());
    } else {
        state.error = error;
        LogLine(L"silent check failed: %ls", error.c_str());
    }
    UpdateState::Write(state);

    return result == CheckResult::Available ? 1
         : (result == CheckResult::Error ? 2 : 0);
}

int RunSilentInstall() {
    HANDLE installMutex = CreateMutexW(nullptr, FALSE, kUpdaterMutexName);
    if (!installMutex) return 2;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(installMutex);
        return 0;
    }

    ReleaseInfo release;
    std::wstring error;
    const CheckResult result = CheckLatestRelease(release, error);
    if (result != CheckResult::Available) {
        UpdateState::Info state;
        state.checkedAtUnixMs = NowUnixMs();
        state.error = (result == CheckResult::Error) ? error : std::wstring{};
        state.latest = (result == CheckResult::UpToDate) ? InstalledAppVersion()
                                                         : std::wstring{};
        UpdateState::Write(state);
        LogLine(L"silent install skipped: %ls",
                result == CheckResult::Error ? error.c_str() : L"up to date");
        CloseHandle(installMutex);
        return result == CheckResult::Error ? 2 : 0;
    }

    const bool ok = ApplyUpdate(
        ModuleDirectory(), release, false, false,
        [](const std::wstring&) {}, error);

    UpdateState::Info state;
    state.checkedAtUnixMs = NowUnixMs();
    state.latest = release.version;
    if (ok) {
        state.available = false;
        LogLine(L"silent install: installed %ls", release.version.c_str());
    } else {
        state.available = true;
        state.error = error;
        LogLine(L"silent install failed: %ls", error.c_str());
    }
    UpdateState::Write(state);

    CloseHandle(installMutex);
    return ok ? 0 : 2;
}

// --- Окно ------------------------------------------------------------------

HWND g_window = nullptr;
HWND g_status = nullptr;
HWND g_progress = nullptr;
HWND g_updateButton = nullptr;
HWND g_laterButton = nullptr;
HWND g_closeButton = nullptr;
HWND g_openButton = nullptr;

bool g_auto = false;
bool g_noStop = false;
bool g_noStart = false;
bool g_updating = false;
std::atomic<bool> g_checkRunning{ false };
ULONGLONG g_autoCloseAt = 0;

std::mutex g_releaseMutex;
ReleaseInfo g_release;

void SetStatus(const std::wstring& text) {
    if (g_status) SetWindowTextW(g_status, text.c_str());
}

void ShowButtons(bool update, bool later, bool close, bool open) {
    if (g_updateButton) ShowWindow(g_updateButton, update ? SW_SHOW : SW_HIDE);
    if (g_laterButton) ShowWindow(g_laterButton, later ? SW_SHOW : SW_HIDE);
    if (g_closeButton) ShowWindow(g_closeButton, close ? SW_SHOW : SW_HIDE);
    if (g_openButton) ShowWindow(g_openButton, open ? SW_SHOW : SW_HIDE);
}

void SetBusy(bool busy) {
    if (g_progress) {
        ShowWindow(g_progress, busy ? SW_SHOW : SW_HIDE);
        SendMessageW(g_progress, PBM_SETMARQUEE, busy ? TRUE : FALSE, 30);
    }
}

void ScheduleAutoClose(UINT seconds) {
    g_autoCloseAt = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
}

DWORD WINAPI CheckThreadProc(LPVOID) {
    ReleaseInfo info;
    std::wstring error;
    const CheckResult result = CheckLatestRelease(info, error);
    if (result == CheckResult::Available) {
        LogLine(L"update available: %ls -> %ls", InstalledAppVersion().c_str(),
                info.version.c_str());
        {
            std::lock_guard lock(g_releaseMutex);
            g_release = info;
        }
        PostMessageW(g_window, WM_APP_CHECK_DONE, 1,
                     reinterpret_cast<LPARAM>(new std::wstring(info.version)));
    } else if (result == CheckResult::UpToDate) {
        LogLine(L"already up to date: %ls", InstalledAppVersion().c_str());
        PostMessageW(g_window, WM_APP_CHECK_DONE, 0,
                     reinterpret_cast<LPARAM>(
                         new std::wstring(InstalledAppVersion())));
    } else {
        LogLine(L"check failed: %ls", error.c_str());
        PostMessageW(g_window, WM_APP_CHECK_DONE, 2,
                     reinterpret_cast<LPARAM>(new std::wstring(error)));
    }
    g_checkRunning.store(false);
    return 0;
}

void StartApply(HWND hwnd) {
    if (g_updating) return;
    g_updating = true;
    g_autoCloseAt = 0;
    SetBusy(true);
    ShowButtons(false, false, false, false);
    SetStatus(L10n::Get(Str::UpdateInstalling));

    struct ApplyContext {
        HWND hwnd;
        ReleaseInfo release;
    };
    ApplyContext* context = nullptr;
    {
        std::lock_guard lock(g_releaseMutex);
        context = new ApplyContext{ hwnd, g_release };
    }

    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
            std::unique_ptr<ApplyContext> context(
                static_cast<ApplyContext*>(param));
            const std::wstring installDir = ModuleDirectory();
            std::wstring error;
            const bool ok = ApplyUpdate(
                installDir, context->release, g_noStop, g_noStart,
                [](const std::wstring& text) {
                    PostMessageW(g_window, WM_APP_UPDATE_STATUS, 0,
                                 reinterpret_cast<LPARAM>(
                                     new std::wstring(text)));
                },
                error);
            if (!ok) LogLine(L"update failed: %ls", error.c_str());
            PostMessageW(g_window, WM_APP_UPDATE_DONE, ok ? 1 : 0,
                         reinterpret_cast<LPARAM>(new std::wstring(error)));
            return 0;
        },
        context, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete context;
        g_updating = false;
        SetBusy(false);
        SetStatus(FormatText(L10n::Get(Str::UpdateFailed),
                             Win32ErrorText(GetLastError()).c_str()));
        ShowButtons(false, false, true, true);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam,
                         LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_window = hwnd;
        g_status = CreateWindowExW(
            0, L"STATIC", L10n::Get(Str::UpdateChecking),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 16, 14, 448, 72,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)),
            nullptr, nullptr);
        g_progress = CreateWindowExW(
            0, PROGRESS_CLASSW, nullptr, WS_CHILD | PBS_MARQUEE, 16, 92, 448,
            18, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROGRESS)),
            nullptr, nullptr);
        g_updateButton = CreateWindowExW(
            0, L"BUTTON", L10n::Get(Str::UpdateButton),
            WS_CHILD | BS_PUSHBUTTON, 238, 130, 100, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_UPDATE)), nullptr,
            nullptr);
        g_laterButton = CreateWindowExW(
            0, L"BUTTON", L10n::Get(Str::UpdateLater),
            WS_CHILD | BS_PUSHBUTTON, 348, 130, 116, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LATER)), nullptr,
            nullptr);
        g_closeButton = CreateWindowExW(
            0, L"BUTTON", L10n::Get(Str::UpdateClose),
            WS_CHILD | BS_PUSHBUTTON, 348, 130, 116, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLOSE)), nullptr,
            nullptr);
        g_openButton = CreateWindowExW(
            0, L"BUTTON", L10n::Get(Str::UpdateOpenRelease),
            WS_CHILD | BS_PUSHBUTTON, 190, 130, 148, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OPEN_RELEASE)),
            nullptr, nullptr);

        for (HWND control : { g_status, g_progress, g_updateButton, g_laterButton,
                              g_closeButton, g_openButton }) {
            if (control) {
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                             TRUE);
            }
        }
        ShowButtons(false, false, false, false);
        SetBusy(true);
        SetTimer(hwnd, TIMER_PROGRESS, 100, nullptr);

        g_checkRunning.store(true);
        HANDLE thread = CreateThread(nullptr, 0, CheckThreadProc, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        return 0;
    }

    case WM_APP_CHECK_DONE: {
        std::unique_ptr<std::wstring> payload(
            reinterpret_cast<std::wstring*>(lParam));
        const std::wstring value = payload ? *payload : std::wstring{};
        SetBusy(false);
        KillTimer(hwnd, TIMER_PROGRESS);

        if (wParam == 1) {
            SetStatus(FormatText(L10n::Get(Str::UpdateAvailable),
                                 value.c_str(), InstalledAppVersion().c_str()));
            ShowButtons(true, true, false, false);
            if (g_auto) {
                StartApply(hwnd);
            }
        } else if (wParam == 0) {
            SetStatus(FormatText(L10n::Get(Str::UpdateUpToDate),
                                 value.c_str()));
            ShowButtons(false, false, true, false);
            ScheduleAutoClose(6);
            SetTimer(hwnd, TIMER_AUTOCLOSE, 500, nullptr);
        } else {
            SetStatus(FormatText(L10n::Get(Str::UpdateFailed), value.c_str()));
            ShowButtons(false, false, true, true);
            if (g_auto) ScheduleAutoClose(3);
        }
        if (g_autoCloseAt != 0) {
            SetTimer(hwnd, TIMER_AUTOCLOSE, 500, nullptr);
        }
        return 0;
    }

    case WM_APP_UPDATE_STATUS: {
        std::unique_ptr<std::wstring> payload(
            reinterpret_cast<std::wstring*>(lParam));
        if (payload) SetStatus(*payload);
        return 0;
    }

    case WM_APP_UPDATE_DONE: {
        std::unique_ptr<std::wstring> payload(
            reinterpret_cast<std::wstring*>(lParam));
        const std::wstring error = payload ? *payload : std::wstring{};
        g_updating = false;
        SetBusy(false);
        KillTimer(hwnd, TIMER_PROGRESS);
        if (wParam == 1) {
            SetStatus(L10n::Get(Str::UpdateDone));
            ShowButtons(false, false, true, false);
            ScheduleAutoClose(6);
        } else {
            SetStatus(FormatText(L10n::Get(Str::UpdateFailed), error.c_str()));
            ShowButtons(false, false, true, true);
            if (g_auto) ScheduleAutoClose(3);
        }
        SetTimer(hwnd, TIMER_AUTOCLOSE, 500, nullptr);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_UPDATE: StartApply(hwnd); break;
        case ID_LATER:
        case ID_CLOSE: DestroyWindow(hwnd); break;
        case ID_OPEN_RELEASE:
            ShellExecuteW(hwnd, L"open", kReleasePage, nullptr, nullptr,
                          SW_SHOWNORMAL);
            break;
        default: break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_AUTOCLOSE && g_autoCloseAt != 0 &&
            GetTickCount64() >= g_autoCloseAt) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        if (g_updating) return 0;  // не закрывать окно посреди установки
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

std::vector<std::wstring> CommandLineArgs() {
    std::vector<std::wstring> args;
    int count = 0;
    LPWSTR* items = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!items) return args;
    for (int i = 1; i < count; ++i) args.emplace_back(items[i]);
    LocalFree(items);
    return args;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    InitializeCriticalSection(&g_logLock);
    g_logLockReady = true;
    L10n::Init();

    bool silentCheck = false;
    bool silentInstall = false;
    for (const std::wstring& arg : CommandLineArgs()) {
        if (_wcsicmp(arg.c_str(), L"--auto") == 0) g_auto = true;
        else if (_wcsicmp(arg.c_str(), L"--no-stop") == 0) g_noStop = true;
        else if (_wcsicmp(arg.c_str(), L"--no-start") == 0) g_noStart = true;
        else if (_wcsicmp(arg.c_str(), L"--check-silent") == 0) silentCheck = true;
        else if (_wcsicmp(arg.c_str(), L"--install-silent") == 0) silentInstall = true;
    }
    LogLine(L"updater start, version=%ls, appVersion=%ls, install=%ls, test=%ls, auto=%d noStop=%d noStart=%d silentCheck=%d silentInstall=%d",
            APP_VERSION_WSTRING, InstalledAppVersion().c_str(), ModuleDirectory().c_str(),
            TestReleaseDirectory().c_str(), g_auto ? 1 : 0, g_noStop ? 1 : 0,
            g_noStart ? 1 : 0, silentCheck ? 1 : 0, silentInstall ? 1 : 0);

    if (silentCheck) return RunSilentCheck();
    if (silentInstall) return RunSilentInstall();

    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, kUpdaterMutexName);
    if (!instanceMutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L10n::Get(Str::UpdateAlreadyRunning),
                    L10n::Get(Str::UpdateTitle), MB_ICONINFORMATION | MB_OK);
        CloseHandle(instanceMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = L"BatteryInTaskBarUpdaterWindow";
    if (!RegisterClassExW(&windowClass)) {
        CloseHandle(instanceMutex);
        return 1;
    }

    RECT rect{ 0, 0, 480, 182 };
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND window = CreateWindowExW(
        0, windowClass.lpszClassName, L10n::Get(Str::UpdateTitle),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height, nullptr,
        nullptr, instance, nullptr);
    if (!window) {
        CloseHandle(instanceMutex);
        return 1;
    }

    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    SetForegroundWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CloseHandle(instanceMutex);

    // Проверочный поток мог остаться; даём ему короткое время завершиться.
    // Критическую секцию не удаляем: процесс всё равно завершается, а гонка
    // с потоком, который ещё пишет в лог, была бы хуже утечки при выходе.
    for (int i = 0; i < 40 && g_checkRunning.load(); ++i) Sleep(50);
    return 0;
}
