// A Windows 11 style popup menu (tray menu, drop-down lists): roomy rows, rounded corners, check marks, shortcut hints,
// sub-menus, keyboard and mouse. Drawn with Direct2D at the monitor's DPI. Modal like TrackPopupMenu.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace vikey { namespace menu {

struct Item {
    int id = 0;
    std::wstring text, hint;          // hint: a shortcut or a value, right-aligned and dimmed
    bool checked = false, enabled = true, separator = false, bold = false;
    wchar_t icon = 0;                 // a Fluent icon glyph on the left (instead of a check mark)
    std::vector<Item> sub;
    static Item sep() { Item i; i.separator = true; return i; }
};

enum Align { BelowPoint = 0, AbovePoint = 1 };
/// Shows the menu at `at` (screen pixels) and returns the chosen item's id, or 0 when dismissed.
/// minWidth in DIPs (a drop-down list as wide as its button).
int show(HWND owner, POINT at, const std::vector<Item>& items, Align align = BelowPoint, float minWidth = 0);
bool isOpen();

}}
