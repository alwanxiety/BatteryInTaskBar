#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Пункт собственного меню. Оформление и поведение — в духе Windows 11:
// скруглённая панель, тень, подсветка под курсором, «выскакивание» при открытии.
struct ContextMenuItem {
    enum class Kind { Title, Info, Action };
    Kind kind = Kind::Action;
    std::wstring text;
    UINT id = 0;
    bool checked = false;
    bool enabled = true;
    // Непустой список превращает пункт в раскрывающееся подменю (стрелка справа).
    std::vector<ContextMenuItem> submenu;
};

// Показывает меню, растущее от прямоугольника виджета (anchorRect в экранных координатах).
// Возвращает id выбранного пункта или 0, если меню закрыли без выбора.
UINT ShowWin11Menu(HINSTANCE hInst, HWND owner, bool darkTheme, const RECT& anchorRect,
                   const std::vector<ContextMenuItem>& items);
