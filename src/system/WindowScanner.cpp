#include "system/WindowScanner.h"

HWND WindowScanner::FindTaskbarWindow() {
    return FindWindowW(L"Shell_TrayWnd", nullptr);
}

HWND WindowScanner::FindTrayNotifyWindow() {
    HWND hTray = FindTaskbarWindow();
    if (!hTray) return nullptr;
    return FindWindowExW(hTray, nullptr, L"TrayNotifyWnd", nullptr);
}
