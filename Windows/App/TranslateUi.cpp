// "Cài đặt dịch…": where the translate requests go (an OpenAI-style chat-completions endpoint), the API key (kept in
// Windows Credential Manager, never shown again once saved) and the auto-off delay.
#include "TranslateUi.h"
#include "Translate.h"
#include "Prefs.h"
#include "resource.h"
#include <commctrl.h>
#include <algorithm>
#include <string>

namespace vikey { namespace ui {

enum { T_PRESET = 300, T_HOST, T_PATH, T_MODEL, T_KEY, T_AUTOOFF, T_SAVE, T_CLEARKEY, T_CLOSE };
static HWND _tw = nullptr;
static HFONT _font = nullptr, _bold = nullptr;
static int _dpi = 96;
static int S(int v) { return MulDiv(v, _dpi, 96); }

struct Preset { const wchar_t* name; const wchar_t* host; const wchar_t* path; const wchar_t* hint; };
static const Preset PRESETS[] = {
    {L"OpenAI", L"api.openai.com", L"/v1/chat/completions", L"Model: tên model trên platform.openai.com (ví dụ gpt-… mini)"},
    {L"Google Gemini", L"generativelanguage.googleapis.com", L"/v1beta/openai/chat/completions", L"Model: tên model Gemini trên ai.google.dev (ví dụ gemini-…-flash)"},
    {L"Tuỳ chỉnh (API kiểu OpenAI)", L"", L"", L"Bất kỳ dịch vụ nào có endpoint chat/completions kiểu OpenAI"},
};

static std::wstring text(int id) {
    HWND c = GetDlgItem(_tw, id); int n = GetWindowTextLengthW(c);
    std::wstring s(n, 0); if (n) GetWindowTextW(c, &s[0], n + 1);
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
static void refreshKeyCue() {
    SendMessageW(GetDlgItem(_tw, T_KEY), EM_SETCUEBANNER, TRUE,
                 (LPARAM)(translate::hasKey() ? L"(đã lưu — để trống nếu không đổi)" : L"Dán API key vào đây"));
}
static void selectPresetFor(const std::wstring& host) {
    int sel = 2;
    for (int i = 0; i < 2; i++) if (host == PRESETS[i].host) sel = i;
    SendMessageW(GetDlgItem(_tw, T_PRESET), CB_SETCURSEL, sel, 0);
    SetWindowTextW(GetDlgItem(_tw, T_PRESET + 100), PRESETS[sel].hint);
}

static void save() {
    std::wstring host = text(T_HOST), path = text(T_PATH), model = text(T_MODEL);
    if (host.find(L"://") != std::wstring::npos) host = host.substr(host.find(L"://") + 3);   // "https://api.x.com" → "api.x.com"
    if (!host.empty() && host.back() == L'/') host.pop_back();
    if (!path.empty() && path[0] != L'/') path = L"/" + path;
    if (host.find_first_of(L"/ ?#") != std::wstring::npos) { MessageBoxW(_tw, L"Host chỉ là tên máy chủ, ví dụ api.openai.com.", L"Cài đặt dịch", MB_ICONWARNING); return; }
    prefs::setString(L"TranslateHost", host); prefs::setString(L"TranslatePath", path); prefs::setString(L"TranslateModel", model);
    std::wstring minutes = text(T_AUTOOFF);
    prefs::setInt(L"TranslateAutoOffMinutes", minutes.empty() ? 0 : std::max(0, std::min(1440, _wtoi(minutes.c_str()))));
    HWND k = GetDlgItem(_tw, T_KEY); int n = GetWindowTextLengthW(k);
    if (n > 0) {
        std::wstring key(n, 0); GetWindowTextW(k, &key[0], n + 1);
        size_t a = key.find_first_not_of(L" \t\r\n"), b = key.find_last_not_of(L" \t\r\n");
        std::wstring trimmed = a == std::wstring::npos ? L"" : key.substr(a, b - a + 1);
        bool ok = translate::setKey(trimmed);
        SecureZeroMemory(&key[0], key.size() * sizeof(wchar_t)); if (!trimmed.empty()) SecureZeroMemory(&trimmed[0], trimmed.size() * sizeof(wchar_t));
        SetWindowTextW(k, L"");
        if (!ok) { MessageBoxW(_tw, L"Không lưu được API key vào Windows Credential Manager.", L"Cài đặt dịch", MB_ICONERROR); return; }
    }
    refreshKeyCue();
    MessageBoxW(_tw, translate::configured() ? L"Đã lưu. Bật chế độ dịch bằng Alt+Shift+T, gõ xong bấm Alt+T để dịch."
                                             : L"Đã lưu, nhưng còn thiếu host, đường dẫn, model hoặc API key.", L"Cài đặt dịch", MB_ICONINFORMATION);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case T_PRESET:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < 2) { SetWindowTextW(GetDlgItem(h, T_HOST), PRESETS[i].host); SetWindowTextW(GetDlgItem(h, T_PATH), PRESETS[i].path); }
                if (i >= 0 && i < 3) SetWindowTextW(GetDlgItem(h, T_PRESET + 100), PRESETS[i].hint);
            }
            return 0;
        case T_SAVE: save(); return 0;
        case T_CLEARKEY:
            if (MessageBoxW(h, L"Xoá API key đã lưu khỏi máy này?", L"Cài đặt dịch", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) { translate::setKey(L""); refreshKeyCue(); }
            return 0;
        case T_CLOSE: DestroyWindow(h); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: SetBkMode((HDC)wp, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY: _tw = nullptr; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void showTranslateSettings(HINSTANCE inst) {
    if (_tw) { ShowWindow(_tw, SW_SHOWNORMAL); SetForegroundWindow(_tw); return; }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"VikeyDichTranslateSettings";
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
        RegisterClassW(&wc); registered = true;
    }
    POINT pt; GetCursorPos(&pt); HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) { auto f = (HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*))GetProcAddress(shcore, "GetDpiForMonitor"); if (f) f(mon, 0, &dx, &dy); }
    _dpi = (int)dx;
    RECT wr = {0, 0, S(520), S(410)}; AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    int w = wr.right - wr.left, hgt = wr.bottom - wr.top;
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - hgt) / 2;
    _tw = CreateWindowExW(0, L"VikeyDichTranslateSettings", L"Cài đặt dịch", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, w, hgt, nullptr, nullptr, inst, nullptr);
    if (_font) DeleteObject(_font); if (_bold) DeleteObject(_bold);
    _font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    _bold = CreateFontW(-S(14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    auto add = [&](const wchar_t* cls, const wchar_t* t, DWORD style, int id, int x0, int y0, int cw, int ch, bool bold = false) {
        HWND c = CreateWindowExW(wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, t, WS_CHILD | WS_VISIBLE | style, S(x0), S(y0), S(cw), S(ch), _tw, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)(bold ? _bold : _font), TRUE); return c;
    };
    int lx = 20, fx = 150, fw = 350, y0 = 18;
    add(L"STATIC", L"Bản dịch do một dịch vụ AI làm. Chỉ đoạn anh bấm Alt+T mới được gửi đi, qua HTTPS, tới đúng máy chủ dưới đây.", SS_LEFT, -1, lx, y0, 480, 40);
    y0 += 52;
    add(L"STATIC", L"Dịch vụ", SS_LEFT, -1, lx, y0 + 4, 120, 20, true);
    HWND preset = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, T_PRESET, fx, y0, fw, 200);
    for (auto& p : PRESETS) SendMessageW(preset, CB_ADDSTRING, 0, (LPARAM)p.name);
    y0 += 32; add(L"STATIC", L"", SS_LEFT, T_PRESET + 100, fx, y0, fw, 36);
    y0 += 40; add(L"STATIC", L"Host", SS_LEFT, -1, lx, y0 + 4, 120, 20, true);
    add(L"EDIT", prefs::getString(L"TranslateHost", L"").c_str(), ES_AUTOHSCROLL | WS_TABSTOP, T_HOST, fx, y0, fw, 26);
    y0 += 36; add(L"STATIC", L"Đường dẫn", SS_LEFT, -1, lx, y0 + 4, 120, 20, true);
    add(L"EDIT", prefs::getString(L"TranslatePath", L"").c_str(), ES_AUTOHSCROLL | WS_TABSTOP, T_PATH, fx, y0, fw, 26);
    y0 += 36; add(L"STATIC", L"Model", SS_LEFT, -1, lx, y0 + 4, 120, 20, true);
    add(L"EDIT", prefs::getString(L"TranslateModel", L"").c_str(), ES_AUTOHSCROLL | WS_TABSTOP, T_MODEL, fx, y0, fw, 26);
    y0 += 36; add(L"STATIC", L"API key", SS_LEFT, -1, lx, y0 + 4, 120, 20, true);
    add(L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, T_KEY, fx, y0, fw - 90, 26);
    add(L"BUTTON", L"Xoá key", BS_PUSHBUTTON | WS_TABSTOP, T_CLEARKEY, fx + fw - 82, y0, 82, 26);
    y0 += 36; add(L"STATIC", L"Tự tắt sau (phút)", SS_LEFT, -1, lx, y0 + 4, 130, 20, true);
    add(L"EDIT", std::to_wstring(prefs::getInt(L"TranslateAutoOffMinutes", 10)).c_str(), ES_NUMBER | WS_TABSTOP, T_AUTOOFF, fx, y0, 60, 26);
    add(L"STATIC", L"0 = không tự tắt", SS_LEFT, -1, fx + 70, y0 + 4, 200, 20);
    y0 += 40;
    add(L"STATIC", L"Alt+Shift+T bật/tắt chế độ dịch · Alt+T dịch đoạn vừa gõ (bấm lại: cách dịch khác) · Backspace ngay sau: lấy lại câu gốc · Esc: huỷ khi đang chờ",
        SS_LEFT, -1, lx, y0, 480, 40);
    y0 += 50;
    add(L"BUTTON", L"Lưu", BS_DEFPUSHBUTTON | WS_TABSTOP, T_SAVE, 320, y0, 90, 30);
    add(L"BUTTON", L"Đóng", BS_PUSHBUTTON | WS_TABSTOP, T_CLOSE, 420, y0, 80, 30);
    selectPresetFor(prefs::getString(L"TranslateHost", L""));
    refreshKeyCue();
    ShowWindow(_tw, SW_SHOWNORMAL); SetForegroundWindow(_tw);
}

}}
