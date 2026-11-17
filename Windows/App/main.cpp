// Vikey for Windows: tray icon (V / E), menu, and the message loop the hooks need.
// Fork: translate mode (blue icon while on), its menu and its notices.
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <windowsx.h>
#include "Core.h"
#include "Prefs.h"
#include "Site.h"
#include "Settings.h"
#include "Emoji.h"
#include "Translate.h"
#include "TranslateText.h"
#include "TranslateUi.h"
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
enum { ID_VIET = 100, ID_ENG, ID_SETTINGS, ID_AUTOSWITCH, ID_ABOUT, ID_QUIT, ID_GUIDE, ID_TR_TOGGLE, ID_TR_SWAP, ID_TR_SETTINGS,
       ID_TR_SOURCE = 300, ID_TR_TARGET = 400 };   // + language index (source: 0 = auto, then the list)

static HWND _hwnd; static NOTIFYICONDATAW _nid = {};
static HICON _iconV, _iconE, _iconVt, _iconEt;   // ...t: translate mode on

/// Draws the menu-bar glyph: a rounded square with a V or E cut out, like the macOS icon.
static HICON makeIcon(wchar_t letter, bool dark, bool translate = false) {
    const int S = 32;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = S; bi.bmiHeader.biHeight = -S; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, color);
    DWORD* px = (DWORD*)bits; for (int i = 0; i < S * S; i++) px[i] = 0;
    COLORREF fg = translate ? RGB(0, 120, 215) : dark ? RGB(255, 255, 255) : RGB(0, 0, 0);   // blue: translate mode on
    HBRUSH b = CreateSolidBrush(fg); HPEN p = CreatePen(PS_NULL, 0, 0);
    SelectObject(dc, b); SelectObject(dc, p);
    RoundRect(dc, 2, 3, S - 2, S - 3, 10, 10);
    HFONT f = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(1, 2, 3));
    RECT r = {0, 1, S, S}; DrawTextW(dc, &letter, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // punch the letter out: pixels drawn in the marker color become transparent; the rest opaque
    for (int i = 0; i < S * S; i++) { DWORD c = px[i] & 0xFFFFFF; if (c == 0x030201) px[i] = 0; else if (c != 0) px[i] |= 0xFF000000; else px[i] = 0; }
    SelectObject(dc, old); DeleteObject(f); DeleteObject(b); DeleteObject(p); DeleteDC(dc);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, color}; HICON h = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(color); return h;
}
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
static void makeIcons(bool dark) {
    _iconV = makeIcon(L'V', dark); _iconE = makeIcon(L'E', dark); _iconVt = makeIcon(L'V', dark, true); _iconEt = makeIcon(L'E', dark, true);
}
static void updateTray(int lang) {
    bool tr = vikey::core::translateEnabled();
    _nid.hIcon = tr ? (lang ? _iconVt : _iconEt) : (lang ? _iconV : _iconE);
    std::wstring tip = lang ? L"Vikey – Tiếng Việt" : L"Vikey – English";
    if (tr) tip += (vikey::core::translateBusy() ? L"\nĐang dịch… " : L"\nDịch: ") + pairLabel();
    wcsncpy_s(_nid.szTip, tip.c_str(), _TRUNCATE);
    _nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &_nid);
}
/// A short notice from translate mode (an error, auto-off): a balloon from the tray icon. Never contains typed text.
static void notifyUser(const std::wstring& message) {
    NOTIFYICONDATAW n = _nid; n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, L"Vikey dịch", _TRUNCATE); wcsncpy_s(n.szInfo, message.c_str(), _TRUNCATE);
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
static void showMenu() {
    using namespace vikey;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (core::language() ? MF_CHECKED : 0), ID_VIET, (L"Tiếng Việt\t" + hotkeyLabel()).c_str());
    AppendMenuW(m, MF_STRING | (prefs::getInt(L"AutoSwitchByTyping", 1) ? MF_CHECKED : 0), ID_AUTOSWITCH, L"Tự chuyển Anh/Việt");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Mở Vikey…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    {   // translate mode
        bool on = core::translateEnabled();
        std::wstring src = prefs::getString(L"TranslateSource", L"vi"), dst = prefs::getString(L"TranslateTarget", L"en");
        AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), ID_TR_TOGGLE, L"Chế độ dịch\tAlt+Shift+T");
        int n; const translate::text::Language* list = translate::text::languages(&n);
        HMENU from = CreatePopupMenu(), to = CreatePopupMenu();
        AppendMenuW(from, MF_STRING | (src == L"auto" ? MF_CHECKED : 0), ID_TR_SOURCE, L"Tự nhận");
        for (int i = 0; i < n; i++) {
            AppendMenuW(from, MF_STRING | (src == list[i].code ? MF_CHECKED : 0), ID_TR_SOURCE + 1 + i, list[i].label);
            AppendMenuW(to, MF_STRING | (dst == list[i].code ? MF_CHECKED : 0), ID_TR_TARGET + i, list[i].label);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)from, (L"Dịch từ: " + languageLabel(src)).c_str());
        AppendMenuW(m, MF_POPUP, (UINT_PTR)to, (L"Dịch sang: " + languageLabel(dst)).c_str());
        AppendMenuW(m, MF_STRING | (src == L"auto" ? MF_GRAYED : 0), ID_TR_SWAP, (L"Đảo chiều (" + languageLabel(dst) + L" → " + languageLabel(src) + L")").c_str());
        AppendMenuW(m, MF_STRING, ID_TR_SETTINGS, translate::configured() ? L"Cài đặt dịch…" : L"Cài đặt dịch… (chưa có API)");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_GUIDE, L"Hướng dẫn…");
    AppendMenuW(m, MF_STRING, ID_ABOUT, L"Giới thiệu Vikey");
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Thoát Vikey");
    POINT pt; GetCursorPos(&pt); SetForegroundWindow(_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, _hwnd, nullptr);
    PostMessageW(_hwnd, WM_NULL, 0, 0);   // or the next menu vanishes at once (TrackPopupMenu remarks)
    DestroyMenu(m);
}
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    using namespace vikey;
    if (WM_TASKBARCREATED && msg == WM_TASKBARCREATED) { Shell_NotifyIconW(NIM_ADD, &_nid); return 0; }   // put the icon back (_nid has the current one)
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ui::showSettings(GetModuleHandleW(nullptr), false);
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_VIET: core::setLanguageByUser(!core::language()); break;
        case ID_AUTOSWITCH: toggle(L"AutoSwitchByTyping", 1); break;
        case ID_SETTINGS: ui::showSettings(GetModuleHandleW(nullptr), false); break;
        case ID_ABOUT: ui::showAbout(GetModuleHandleW(nullptr)); break;
        case ID_GUIDE: ui::showOnboarding(GetModuleHandleW(nullptr)); break;
        case ID_QUIT: DestroyWindow(h); break;
        case ID_TR_TOGGLE: core::setTranslateEnabled(!core::translateEnabled()); break;
        case ID_TR_SETTINGS: ui::showTranslateSettings(GetModuleHandleW(nullptr)); break;
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
    case WM_APP + 3: ui::showSettings(GetModuleHandleW(nullptr), false); return 0;
    case WM_VIKEY_SETLANG: core::setLanguageQuiet((int)wp); return 1;
    case WM_VIKEY_EMOJI: emoji::toggle(); return 0;
    case WM_TIMER: if (wp == TIMER_FLUSH) { core::flushTypingLog(); core::translateTick(); } return 0;
    case core::WM_TR_TOGGLE: case core::WM_TR_FIELD: case translate::WM_TRANSLATED: core::onTranslateMessage(msg, wp); return 0;
    case WM_VIKEY_SETTRANSLATE: core::setTranslateEnabled(wp != 0); return 1;
    case site::WM_SITE: { HWND w; std::wstring host = site::lastHost(&w); if (w == GetForegroundWindow()) core::onSiteChanged(host); return 0; }
    case WM_SETTINGCHANGE: {
        if (!lp || wcscmp((LPCWSTR)lp, L"ImmersiveColorSet") != 0) return 0;   // a theme change: every other setting broadcast is ignored
        bool dark = darkTaskbar(); DestroyIcon(_iconV); DestroyIcon(_iconE); DestroyIcon(_iconVt); DestroyIcon(_iconEt); makeIcons(dark); updateTray(core::language()); return 0; }
    case WM_DESTROY: core::stop(); Shell_NotifyIconW(NIM_DELETE, &_nid); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\VikeyDichRunning");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {   // already running: opening the app again shows its window, like on macOS
        HWND t = FindWindowW(L"VikeyDichTray", nullptr);
        if (t) { DWORD pid = 0; GetWindowThreadProcessId(t, &pid); AllowSetForegroundWindow(pid); PostMessageW(t, WM_APP + 3, 0, 0); }   // our foreground right goes to the running Vikey: its window comes to the front
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    vikey::prefs::migrateFromOpenKey();
    WNDCLASSW wc = {}; wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"VikeyDichTray";
    RegisterClassW(&wc);
    // hidden top-level, not HWND_MESSAGE: a message-only window gets no broadcasts (TaskbarCreated, WM_SETTINGCHANGE);
    // WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt+Tab
    _hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"VikeyDichTray", L"Vikey", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    bool dark = darkTaskbar(); makeIcons(dark);
    _nid.cbSize = sizeof(_nid); _nid.hWnd = _hwnd; _nid.uID = 1; _nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; _nid.uCallbackMessage = WM_TRAY; _nid.hIcon = _iconV;
    wcscpy_s(_nid.szTip, L"Vikey");
    Shell_NotifyIconW(NIM_ADD, &_nid);
    vikey::core::setLanguageObserver([](int lang) { updateTray(lang); vikey::ui::languageChanged(lang); });
    vikey::translate::start(_hwnd);
    vikey::core::setTranslateWindow(_hwnd);
    vikey::core::setTranslateObserver([](int event, const std::wstring& message) {
        if (event == 0) updateTray(vikey::core::language()); else notifyUser(message);
    });
    if (!vikey::core::start()) { MessageBoxW(nullptr, L"Không cài được bộ bắt phím (SetWindowsHookEx).", L"Vikey", MB_ICONERROR); return 1; }
    updateTray(vikey::core::language());
    vikey::emoji::install(inst);
    vikey::core::setEmojiHotkeyHandler([] { PostMessageW(_hwnd, WM_VIKEY_EMOJI, 0, 0); });   // not inside the hook: opening can take a moment
    if (!vikey::prefs::has(L"FirstRunDone")) vikey::ui::showSettings(inst, true);
    SetTimer(_hwnd, TIMER_FLUSH, 10000, nullptr);
    vikey::site::start(_hwnd);   // per-website memory and password fields, read off the hook thread
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    ReleaseMutex(single); CloseHandle(single);
    return 0;
}
