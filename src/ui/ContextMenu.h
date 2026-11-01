#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

// Пункт собственного меню. Оформление и поведение — в духе Windows 11:
// скруглённая панель, тень, подсветка под курсором, «выскакивание» при открытии.
struct ContextMenuItem {
    enum class Kind { Title, Info, Action, Footer };
    Kind kind = Kind::Action;
    std::wstring text;
    UINT id = 0;
    bool checked = false;
    bool enabled = true;
    // Непустой список превращает пункт в раскрывающееся подменю (стрелка справа).
    std::vector<ContextMenuItem> submenu;

    // Футер: «Версия: …» и две маленькие кнопки справа — проверить и обновить.
    UINT checkId = 0;
    UINT installId = 0;
    bool updateAvailable = false;  // синяя точка на кнопке обновления
    bool updateChecking = false;   // проверка уже идёт
    // Запускает тихую проверку, не закрывая меню. Если пусто — возвращается checkId.
    std::function<void()> onCheck;
};

// Показывает меню, растущее от прямоугольника виджета (anchorRect в экранных координатах).
// Возвращает id выбранного пункта или 0, если меню закрыли без выбора.
UINT ShowWin11Menu(HINSTANCE hInst, HWND owner, bool darkTheme, const RECT& anchorRect,
                   const std::vector<ContextMenuItem>& items);

/// Закрыть открытое меню (анимация закрытия), не разрушая окно владельца.
/// Нужно, когда WM_CLOSE приходит, пока меню ещё показано: окно нельзя
/// уничтожать изнутри его же вложенного цикла сообщений.
void CloseActiveMenu();
