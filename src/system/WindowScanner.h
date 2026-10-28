#pragma once
#include <windows.h>

class WindowScanner {
public:
    static HWND FindTaskbarWindow();
    static HWND FindTrayNotifyWindow();
};
