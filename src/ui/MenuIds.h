#pragma once
#include <windows.h>

// Общие id команд для меню мыши и наушников.
constexpr UINT IDM_REFRESH = 1000;
constexpr UINT IDM_AUTORUN = 1001;
constexpr UINT IDM_EXIT = 1002;
constexpr UINT IDM_THEME_GLASS = 1003;
constexpr UINT IDM_THEME_DARK = 1004;
constexpr UINT IDM_THEME_LIGHT = 1005;
// Скрытая команда: пять кликов по названию устройства в меню мыши.
constexpr UINT IDM_REPORT = 1006;
// Меню наушников: обновить только Bluetooth-данные и скрыть вторую плитку.
constexpr UINT IDM_HEADSET_REFRESH = 1008;
constexpr UINT IDM_HEADSET_HIDE = 1009;
