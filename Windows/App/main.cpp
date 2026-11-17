// FriKey (Vietnamese Input by Friday, based on Vikey): tray icon (V / E, blue while translate mode is on), its menu,
// translate notices, and the message loop the hooks need.
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <algorithm>
#include <windowsx.h>
#include "Core.h"
#include "Prefs.h"
#include "Site.h"
#include "Settings.h"
#include "Emoji.h"
#include "Translate.h"
#include "TranslateText.h"
#include "Menu.h"
#include "Ui.h"
#include "../../Engine/Vietnamese.h"
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_VIKEY_SETLANG = WM_APP + 2;   // tests: wParam = language (0 English, 1 Vietnamese)
static const UINT WM_VIKEY_EMOJI = WM_APP + 5;     // the emoji hotkey, posted from the keyboard hook
static const UINT WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");   // Explorer (re)started: the tray is empty
static const UINT WM_VIKEY_SETTRANSLATE = WM_APP + 24;   // tests: wParam = translate mode on (1) or off (0)
static const UINT TIMER_FLUSH = 1;
enum { ID_VIET = 100, ID_ENG, ID_SETTINGS, ID_AUTOSWITCH, ID_ABOUT, ID_QUIT, ID_TR_TOGGLE, ID_TR_SWAP, ID_TR_SETTINGS,
       ID_TR_SOURCE = 300, ID_TR_TARGET = 400 };   // + language index (source: 0 = auto, then the list)

static HWND _hwnd; static NOTIFYICONDATAW _nid = {};
static bool _skipClickUp = false; static ULONGLONG _clickSwitchedAt = 0;   // tray clicks
static HICON _iconV, _iconE, _iconVt, _iconEt;   // ...t: translate mode on

static bool darkTaskbar() {
    HKEY h; DWORD v = 1, s = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &h) == ERROR_SUCCESS) { RegQueryValueExW(h, L"SystemUsesLightTheme", nullptr, nullptr, (LPBYTE)&v, &s); RegCloseKey(h); }
    return v == 0;
}
static std::wstring languageLabel(const std::wstring& code) {
    if (code == L"auto") return L"Tự nhận";
    const vikey::translate::text::Language* l = vikey::translate::text::languageByCode(code);
    return l ? l->label : code;
}
static std::wstring pairLabel() {
    return languageLabel(vikey::prefs::getString(L"TranslateSource", L"vi")) + L" → " + languageLabel(vikey::prefs::getString(L"TranslateTarget", L"en"));
}
/// The tray icons: V (Tiếng Việt, orange) and E (English, blue), with a green dot while translate mode is on. Loaded at
/// the tray's own size for the screen's DPI, so they are sharp and fill their slot like the other tray icons.
static void makeIcons(bool) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    int cx = std::max(16, GetSystemMetrics(SM_CXSMICON)), cy = std::max(16, GetSystemMetrics(SM_CYSMICON));
    auto load = [&](int id) { return (HICON)LoadImageW(inst, MAKEINTRESOURCEW(id), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR); };
    _iconV = load(IDI_TRAY_V); _iconE = load(IDI_TRAY_E); _iconVt = load(IDI_TRAY_V_TRANSLATE); _iconEt = load(IDI_TRAY_E_TRANSLATE);
}
static void updateTray(int lang) {
    bool tr = vikey::core::translateEnabled();
    _nid.hIcon = tr ? (lang ? _iconVt : _iconEt) : (lang ? _iconV : _iconE);
    std::wstring tip = lang ? L"FriKey – Tiếng Việt" : L"FriKey – English";
    if (tr) tip += (vikey::core::translateBusy() ? L"\nĐang dịch… " : L"\nDịch: ") + pairLabel();
    wcsncpy_s(_nid.szTip, tip.c_str(), _TRUNCATE);
    _nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &_nid);
}
/// A short notice from translate mode (an error, auto-off): a balloon from the tray icon. Never contains typed text.
static void notifyUser(const std::wstring& message) {
    NOTIFYICONDATAW n = _nid; n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, L"FriKey", _TRUNCATE); wcsncpy_s(n.szInfo, message.c_str(), _TRUNCATE);
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &n);
    MessageBeep(MB_ICONASTERISK);
}
static void setPair(const std::wstring& source, const std::wstring& target) {
    vikey::prefs::setString(L"TranslateSource", source); vikey::prefs::setString(L"TranslateTarget", target);
    vikey::core::translatePairChanged();
}
static void toggle(const wchar_t* key, int fallback) { vikey::prefs::setInt(key, !vikey::prefs::getInt(key, fallback)); vikey::core::reloadSettings(); }
static std::wstring hotkeyLabel() {
    int hk = vikey::prefs::getInt(L"SwitchKeyStatus", 0); if (!hk) hk = 0x7A000206;
    std::wstring t; if (hk & 0x100) t += L"Ctrl+"; if (hk & 0x200) t += L"Alt+"; if (hk & 0x800) t += L"Shift+"; if (hk & 0x400) t += L"Win+";
    int k = hk & 0xFF; if (k != 0xFE) { unsigned short ch = keyCodeToCharacter((unsigned int)(k | 0x10000)); if (k == 49) t += L"Space"; else if (ch) t += (wchar_t)ch; } else if (!t.empty()) t.pop_back();
    return t;
}
static std::wstring shortcutLabel(const wchar_t* pref, int fallback) {
    int hk = vikey::prefs::getInt(pref, fallback); if (!hk) return L"";
    std::wstring t; if (hk & 0x100) t += L"Ctrl+"; if (hk & 0x200) t += L"Alt+"; if (hk & 0x800) t += L"Shift+"; if (hk & 0x400) t += L"Win+";
    int k = hk & 0xFF; if (k != 0xFE) { unsigned short ch = keyCodeToCharacter((unsigned int)(k | 0x10000)); if (k == 49) t += L"Space"; else if (ch) t += (wchar_t)ch; } else if (!t.empty()) t.pop_back();
    return t;
}
/// The tray menu: FriKey's own Windows 11 style menu (Menu.cpp), roomy rows grouped by separators.
static void showMenu() {
    using namespace vikey;
    using menu::Item;
    auto item = [](int id, const std::wstring& text, const std::wstring& hint = L"", bool checked = false, bool enabled = true) {
        Item i; i.id = id; i.text = text; i.hint = hint; i.checked = checked; i.enabled = enabled; return i; };
    std::vector<Item> m;
    m.push_back(item(ID_VIET, L"Gõ tiếng Việt", hotkeyLabel(), core::language() != 0));
    m.push_back(item(ID_AUTOSWITCH, L"Tự chuyển Anh/Việt", L"", prefs::getInt(L"AutoSwitchByTyping", 1) != 0));
    m.push_back(Item::sep());
    bool on = core::translateEnabled();
    std::wstring src = prefs::getString(L"TranslateSource", L"vi"), dst = prefs::getString(L"TranslateTarget", L"en");
    m.push_back(item(ID_TR_TOGGLE, L"Chế độ dịch", shortcutLabel(L"TranslateToggleHotkey", 0x11 | 0x200 | 0x800), on));
    int n; const translate::text::Language* list = translate::text::languages(&n);
    Item from = item(0, L"Dịch từ", languageLabel(src)), to = item(0, L"Dịch sang", languageLabel(dst));
    from.sub.push_back(item(ID_TR_SOURCE, L"Tự nhận", L"", src == L"auto"));
    for (int i = 0; i < n; i++) {
        from.sub.push_back(item(ID_TR_SOURCE + 1 + i, list[i].label, L"", src == list[i].code));
        to.sub.push_back(item(ID_TR_TARGET + i, list[i].label, L"", dst == list[i].code));
    }
    m.push_back(from); m.push_back(to);
    Item swap = item(ID_TR_SWAP, L"Đảo chiều", languageLabel(dst) + L" → " + languageLabel(src), false, src != L"auto"); swap.icon = look::IC_SWAP;
    m.push_back(swap);
    if (!translate::configured()) m.push_back(item(ID_TR_SETTINGS, L"Cài đặt dịch…", L"chưa có API"));
    m.push_back(Item::sep());
    Item settings = item(ID_SETTINGS, L"Cài đặt FriKey…"); settings.icon = look::IC_SETTINGS; m.push_back(settings);
    Item about = item(ID_ABOUT, L"Giới thiệu"); about.icon = look::IC_INFO; m.push_back(about);
    m.push_back(Item::sep());
    m.push_back(item(ID_QUIT, L"Thoát FriKey"));
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(_hwnd);
    int id = menu::show(_hwnd, pt, m, menu::AbovePoint);
    if (id) PostMessageW(_hwnd, WM_COMMAND, id, 0);
}
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    using namespace vikey;
    if (WM_TASKBARCREATED && msg == WM_TASKBARCREATED) { Shell_NotifyIconW(NIM_ADD, &_nid); return 0; }   // put the icon back (_nid has the current one)
    switch (msg) {
    case WM_TRAY:
        // one click switches Tiếng Việt / English at once (like Unikey); a double click opens the settings. The first click
        // of a double click has already switched, so the double click switches back: the language ends where it was
        if (LOWORD(lp) == WM_LBUTTONUP) {
            if (_skipClickUp) _skipClickUp = false;
            else { core::setLanguageByUser(!core::language()); _clickSwitchedAt = GetTickCount64(); }
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            if (GetTickCount64() - _clickSwitchedAt <= GetDoubleClickTime() + 100) core::setLanguageByUser(!core::language());
            _skipClickUp = true;   // the double click's own button-up
            ui::showSettings(GetModuleHandleW(nullptr), ui::PageTyping);
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_VIET: core::setLanguageByUser(!core::language()); break;
        case ID_AUTOSWITCH: toggle(L"AutoSwitchByTyping", 1); break;
        case ID_SETTINGS: ui::showSettings(GetModuleHandleW(nullptr), ui::PageTyping); break;
        case ID_ABOUT: ui::showSettings(GetModuleHandleW(nullptr), ui::PageAbout); break;
        case ID_QUIT: DestroyWindow(h); break;
        case ID_TR_TOGGLE: core::setTranslateEnabled(!core::translateEnabled()); break;
        case ID_TR_SETTINGS: ui::showSettings(GetModuleHandleW(nullptr), ui::PageTranslate); break;
        case ID_TR_SWAP: setPair(prefs::getString(L"TranslateTarget", L"en"), prefs::getString(L"TranslateSource", L"vi")); break;
        default: {
            int n; const translate::text::Language* list = translate::text::languages(&n);
            int id = LOWORD(wp);
            std::wstring src = prefs::getString(L"TranslateSource", L"vi"), dst = prefs::getString(L"TranslateTarget", L"en");
            if (id >= ID_TR_SOURCE && id <= ID_TR_SOURCE + n) {
                std::wstring pick = id == ID_TR_SOURCE ? L"auto" : list[id - ID_TR_SOURCE - 1].code;
                setPair(pick, pick == dst ? src : dst);   // the same language on both sides: swap instead
            } else if (id >= ID_TR_TARGET && id < ID_TR_TARGET + n) {
                std::wstring pick = list[id - ID_TR_TARGET].code;
                setPair(pick == src ? (dst == L"auto" ? L"vi" : dst) : src, pick);
            }
        } }
        return 0;
    case WM_APP + 3: ui::showSettings(GetModuleHandleW(nullptr), ui::PageTyping); return 0;
    case WM_VIKEY_SETLANG: core::setLanguageQuiet((int)wp); return 1;
    case WM_VIKEY_EMOJI: emoji::toggle(); return 0;
    case WM_TIMER: if (wp == TIMER_FLUSH) { core::flushTypingLog(); core::translateTick(); } return 0;
    case core::WM_TR_TOGGLE: case core::WM_TR_FIELD: case translate::WM_TRANSLATED: core::onTranslateMessage(msg, wp); return 0;
    case WM_VIKEY_SETTRANSLATE: core::setTranslateEnabled(wp != 0); return 1;
    case site::WM_SITE: { HWND w; std::wstring host = site::lastHost(&w); if (w == GetForegroundWindow()) core::onSiteChanged(host); return 0; }
    case WM_SETTINGCHANGE: {
        if (!lp || wcscmp((LPCWSTR)lp, L"ImmersiveColorSet") != 0) return 0;   // a theme change: every other setting broadcast is ignored
        look::refreshTheme();
        bool dark = darkTaskbar(); DestroyIcon(_iconV); DestroyIcon(_iconE); DestroyIcon(_iconVt); DestroyIcon(_iconEt); makeIcons(dark); updateTray(core::language()); return 0; }
    case WM_DESTROY: core::stop(); Shell_NotifyIconW(NIM_DELETE, &_nid); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\FriKeyRunning");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {   // already running: opening the app again shows its window, like on macOS
        HWND t = FindWindowW(L"FriKeyTray", nullptr);
        if (t) { DWORD pid = 0; GetWindowThreadProcessId(t, &pid); AllowSetForegroundWindow(pid); PostMessageW(t, WM_APP + 3, 0, 0); }   // our foreground right goes to the running Vikey: its window comes to the front
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    vikey::prefs::migrateFromOpenKey();
    WNDCLASSW wc = {}; wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"FriKeyTray";
    RegisterClassW(&wc);
    // hidden top-level, not HWND_MESSAGE: a message-only window gets no broadcasts (TaskbarCreated, WM_SETTINGCHANGE);
    // WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt+Tab
    _hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"FriKeyTray", L"FriKey", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    bool dark = darkTaskbar(); makeIcons(dark);
    _nid.cbSize = sizeof(_nid); _nid.hWnd = _hwnd; _nid.uID = 1; _nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; _nid.uCallbackMessage = WM_TRAY; _nid.hIcon = _iconV;
    wcscpy_s(_nid.szTip, L"FriKey");
    Shell_NotifyIconW(NIM_ADD, &_nid);
    vikey::core::setLanguageObserver([](int lang) { updateTray(lang); vikey::ui::languageChanged(lang); });
    vikey::translate::start(_hwnd);
    vikey::core::setTranslateWindow(_hwnd);
    vikey::core::setTranslateObserver([](int event, const std::wstring& message) {
        if (event == 0) { updateTray(vikey::core::language()); vikey::ui::refreshSettings(); } else notifyUser(message);
    });
    if (!vikey::core::start()) { MessageBoxW(nullptr, L"Không cài được bộ bắt phím (SetWindowsHookEx).", L"FriKey", MB_ICONERROR); return 1; }
    updateTray(vikey::core::language());
    vikey::emoji::install(inst);
    vikey::core::setEmojiHotkeyHandler([] { PostMessageW(_hwnd, WM_VIKEY_EMOJI, 0, 0); });   // not inside the hook: opening can take a moment
    if (!vikey::prefs::has(L"FirstRunDone")) { vikey::prefs::setInt(L"FirstRunDone", 1); vikey::ui::showSettings(inst, vikey::ui::PageTyping); }
    SetTimer(_hwnd, TIMER_FLUSH, 10000, nullptr);
    vikey::site::start(_hwnd);   // per-website memory and password fields, read off the hook thread
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    ReleaseMutex(single); CloseHandle(single);
    return 0;
}
