#include "core/AppPaths.h"
#include <windows.h>
#include <mutex>

namespace {

std::once_flag g_once;
std::wstring g_directory;

std::wstring EnvironmentPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring buffer(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, buffer.data(), size);
    if (written == 0 || written >= size) return {};
    buffer.resize(written);
    return buffer;
}

void EnsureDirectory() {
    std::wstring base = EnvironmentPath(L"LOCALAPPDATA");
    if (base.empty()) {
        // Совсем крайний случай (нет профиля) — хотя бы TEMP.
        wchar_t temp[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (length > 0 && length < MAX_PATH) base.assign(temp, length);
    }
    if (base.empty()) return;
    if (base.back() == L'\\') base.pop_back();

    std::wstring directory = base + L"\\BatteryInTaskBar";
    CreateDirectoryW(directory.c_str(), nullptr);  // уже существует — это норма
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return;
    }
    g_directory = std::move(directory);
}

bool WriteExact(HANDLE file, const void* data, DWORD size) {
    DWORD written = 0;
    return WriteFile(file, data, size, &written, nullptr) != FALSE &&
           written == size;
}

}  // namespace

const std::wstring& AppPaths::DataDirectory() {
    std::call_once(g_once, EnsureDirectory);
    return g_directory;
}

std::wstring AppPaths::DataFile(const wchar_t* name) {
    const std::wstring& directory = DataDirectory();
    if (directory.empty()) return {};
    return directory + L"\\" + name;
}

bool AppPaths::WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    if (path.empty() || text.empty() ||
        text.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    const int length = static_cast<int>(text.size());

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    if (static_cast<unsigned int>(bytes) > MAXDWORD) return false;

    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, utf8.data(), bytes,
                            nullptr, nullptr) != bytes) {
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    const bool ok = WriteExact(file, bom, sizeof(bom)) &&
                    WriteExact(file, utf8.data(), static_cast<DWORD>(utf8.size()));
    CloseHandle(file);
    return ok;
}
