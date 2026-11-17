// FriKey's settings window, in the style of Windows 11 Settings (WinUI 3): a navigation pane on the left (Gõ tiếng Việt,
// Dịch, Giới thiệu) and pages of setting cards on the right — toggle switches, drop-downs, shortcut recorders, text
// boxes. Drawn with Direct2D in DIPs at the window's DPI (sharp on 4K, follows WM_DPICHANGED), Plus Jakarta Sans,
// light and dark theme and the accent colour from Windows. Text boxes are native Edit controls inside drawn frames.
#include "Settings.h"
#include "Ui.h"
#include "Menu.h"
#include "Core.h"
#include "Prefs.h"
#include "Emoji.h"
#include "KeyMap.h"
#include "Translate.h"
#include "TranslateText.h"
#include "resource.h"
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <functional>
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>

#ifndef FRIKEY_VERSION
#define FRIKEY_VERSION "0.2.0"
#endif
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

namespace vikey { namespace ui {

using namespace look;

// ---- model -----------------------------------------------------------------------------------
enum Kind { K_TITLE, K_HEADER, K_CARD, K_HERO };
enum Ctl { C_NONE, C_TOGGLE, C_COMBO, C_HOTKEY, C_BUTTON, C_ACCENT, C_EDIT, C_KEY, C_SEG, C_LINK };
struct El {
    Kind kind = K_CARD; Ctl ctl = C_NONE; wchar_t icon = 0;
    std::wstring title, desc, button;
    bool on = false, enabled = true, modsOnly = false, password = false;
    int index = 0, hotkey = 0;
    std::vector<std::wstring> options;
    std::wstring pref, cue, url;
    HWND edit = nullptr;
    std::function<void(El&)> change;
    D2D1_RECT_F rect = {}, ctlRect = {}, btnRect = {};
    float textTop = 0, textRight = 0;   // where the title starts, and where title and description wrap
};

static HWND _w; static HINSTANCE _inst; static ID2D1HwndRenderTarget* _rt; static float _s = 1;
static int _page = 0; static float _scroll = 0, _contentH = 0;
static std::vector<El> _els;
static int _hover = -1, _hoverNav = -1, _pressed = -1, _pressedNav = -1; static bool _hoverBtn = false;
static int _recording = -1;
static HFONT _editFont; static HBRUSH _editBrush;
static unsigned _testId = 0x80000000u; static bool _testing = false;
static const UINT WM_REFRESH = WM_APP + 40;

static const float NAV_W = 232, PAD_X = 36, MAX_W = 1000, CARD_GAP = 4, CARD_PAD = 18, MIN_CARD = 70, CTL_H = 32;

// ---- shortcuts -------------------------------------------------------------------------------
static std::vector<std::wstring> keysOf(int hk) {
    std::vector<std::wstring> k;
    if (!hk) return k;
    if (HAS_CONTROL(hk)) k.push_back(L"Ctrl"); if (HAS_OPTION(hk)) k.push_back(L"Alt"); if (HAS_SHIFT(hk)) k.push_back(L"Shift"); if (HAS_COMMAND(hk)) k.push_back(L"Win");
    int c = GET_SWITCH_KEY(hk);
    if (c != 0xFE) {
        Uint16 ch = keyCodeToCharacter(c | CAPS_MASK);
        if (c == KEY_SPACE) k.push_back(L"Space"); else if (c == KEY_RETURN) k.push_back(L"Enter"); else if (c == KEY_TAB) k.push_back(L"Tab");
        else if (ch) k.push_back(std::wstring(1, (wchar_t)ch)); else k.push_back(L"?");
    }
    return k;
}
static std::wstring hotkeyText(int hk) { std::wstring s; for (auto& k : keysOf(hk)) { if (!s.empty()) s += L"+"; s += k; } return s.empty() ? L"Chưa đặt" : s; }
static int switchKey() { int hk = prefs::getInt(L"SwitchKeyStatus", 0); return hk ? hk : 0x7A000206; }
static int translateKey() { return prefs::getInt(L"TranslateHotkey", KEY_T | 0x200); }
static int toggleKey() { return prefs::getInt(L"TranslateToggleHotkey", KEY_T | 0x200 | 0x800); }
/// The shortcuts in use, without the beep bit: a new one may not take another's place.
static bool clashes(const std::wstring& pref, int value) {
    struct { const wchar_t* p; int v; } all[] = {{L"SwitchKeyStatus", switchKey()}, {L"EmojiHotkey", prefs::getInt(L"EmojiPicker", 0) ? prefs::getInt(L"EmojiHotkey", 0) : 0},
                                                 {L"TranslateHotkey", translateKey()}, {L"TranslateToggleHotkey", toggleKey()}};
    for (auto& a : all) if (pref != a.p && a.v && (a.v & 0xFFFF & ~0x8000) == (value & 0xFFFF & ~0x8000)) return true;
    return false;
}

// ---- pages -----------------------------------------------------------------------------------
static El& add(Kind k, const std::wstring& title, const std::wstring& desc = L"") { _els.push_back(El()); El& e = _els.back(); e.kind = k; e.title = title; e.desc = desc; return e; }
static El& header(const std::wstring& t) { return add(K_HEADER, t); }
static El& toggle(wchar_t icon, const std::wstring& t, const std::wstring& d, bool on, std::function<void(El&)> f) {
    El& e = add(K_CARD, t, d); e.icon = icon; e.ctl = C_TOGGLE; e.on = on; e.change = f; return e;
}
static El& combo(wchar_t icon, const std::wstring& t, const std::wstring& d, std::vector<std::wstring> opts, int index, std::function<void(El&)> f) {
    El& e = add(K_CARD, t, d); e.icon = icon; e.ctl = C_COMBO; e.options = opts; e.index = index; e.change = f; return e;
}
static El& hotkey(wchar_t icon, const std::wstring& t, const std::wstring& d, const wchar_t* pref, int value, bool modsOnly) {
    El& e = add(K_CARD, t, d); e.icon = icon; e.ctl = C_HOTKEY; e.pref = pref; e.hotkey = value; e.modsOnly = modsOnly; return e;
}
static El& button(wchar_t icon, const std::wstring& t, const std::wstring& d, const std::wstring& label, bool accent, std::function<void(El&)> f) {
    El& e = add(K_CARD, t, d); e.icon = icon; e.ctl = accent ? C_ACCENT : C_BUTTON; e.button = label; e.change = f; return e;
}
static El& field(wchar_t icon, const std::wstring& t, const std::wstring& d, const wchar_t* pref, const std::wstring& cue) {
    El& e = add(K_CARD, t, d); e.icon = icon; e.ctl = C_EDIT; e.pref = pref; e.cue = cue; return e;
}
static El* find(const std::wstring& title) { for (auto& e : _els) if (e.title == title) return &e; return nullptr; }

static void refreshEnabled();
static void rebuild();
static void drawDialog();

static int langIndex(const std::wstring& code, bool withAuto) {
    int n; const translate::text::Language* l = translate::text::languages(&n);
    if (withAuto && code == L"auto") return 0;
    for (int i = 0; i < n; i++) if (code == l[i].code) return i + (withAuto ? 1 : 0);
    return 0;
}
static std::vector<std::wstring> langOptions(bool withAuto) {
    int n; const translate::text::Language* l = translate::text::languages(&n);
    std::vector<std::wstring> o; if (withAuto) o.push_back(L"Tự nhận");
    for (int i = 0; i < n; i++) o.push_back(l[i].label);
    return o;
}
static std::wstring langCode(int index, bool withAuto) {
    int n; const translate::text::Language* l = translate::text::languages(&n);
    if (withAuto) { if (index == 0) return L"auto"; index--; }
    return index >= 0 && index < n ? l[index].code : L"en";
}

struct Preset { const wchar_t* name; const wchar_t* host; const wchar_t* path; const wchar_t* model; };
static const Preset PRESETS[] = {
    {L"Google Gemini", L"generativelanguage.googleapis.com", L"/v1beta/openai/chat/completions", L"ví dụ gemini-3.5-flash-lite"},
    {L"OpenAI", L"api.openai.com", L"/v1/chat/completions", L"tên model trên platform.openai.com"},
    {L"Tuỳ chỉnh (API kiểu OpenAI)", L"", L"", L"tên model của dịch vụ"},
};
static int presetIndex() { std::wstring h = prefs::getString(L"TranslateHost", L""); for (int i = 0; i < 2; i++) if (h == PRESETS[i].host) return i; return h.empty() ? 0 : 2; }

static void pageTyping() {
    int hk = switchKey();
    add(K_TITLE, L"Gõ tiếng Việt");
    El& seg = add(K_CARD, L"Đang gõ", L"Chuyển nhanh bằng phím chuyển Anh/Việt bên dưới");
    seg.icon = IC_KEYBOARD; seg.ctl = C_SEG; seg.options = {L"Tiếng Việt", L"English"}; seg.index = core::language() ? 0 : 1;
    seg.change = [](El& e) { core::setLanguageByUser(e.index == 0 ? 1 : 0); };
    hotkey(0, L"Phím chuyển Anh/Việt", L"Một phím kèm Ctrl, Alt, Shift hoặc Win, hoặc hai phím bổ trợ cùng lúc (ví dụ Ctrl+Shift)", L"SwitchKeyStatus", hk, true);
    toggle(0, L"Âm báo khi chuyển", L"Lên giọng khi sang tiếng Việt, xuống giọng khi sang tiếng Anh", HAS_BEEP(hk) != 0, [](El& e) {
        prefs::setInt(L"SwitchKeyStatus", (switchKey() & ~0x8000) | (e.on ? 0x8000 : 0)); core::reloadSettings(); });

    header(L"Tự chuyển Anh/Việt");
    toggle(0, L"Tự chuyển khi gõ", L"Gõ một từ tiếng Anh khi đang ở tiếng Việt (hoặc ngược lại), FriKey tự sửa từ đó và chuyển chế độ. Bấm Xoá ngay sau đó để hoàn tác",
           prefs::getInt(L"AutoSwitchByTyping", 1) != 0, [](El& e) { prefs::setInt(L"AutoSwitchByTyping", e.on); core::reloadSettings(); refreshEnabled(); });
    toggle(0, L"Tạm dừng 5 giây", L"Sau khi bạn tự bấm chuyển, FriKey không tự chuyển trong 5 giây", prefs::getInt(L"PauseAutoAfterManual", 1) != 0,
           [](El& e) { prefs::setInt(L"PauseAutoAfterManual", e.on); core::reloadSettings(); });
    toggle(0, L"Nhớ theo ứng dụng", L"Nhớ bạn gõ tiếng Anh hay tiếng Việt ở mỗi ứng dụng và tự chuyển lại khi quay về", prefs::getInt(L"UseSmartSwitchKey", 1) != 0,
           [](El& e) { prefs::setInt(L"UseSmartSwitchKey", e.on); core::reloadSettings(); refreshEnabled(); });
    toggle(0, L"Nhớ theo trang web", L"Chrome, Edge, Brave, Vivaldi, Opera, Firefox: nhớ cho từng trang web. Chỉ đọc địa chỉ tab đang mở, ngay trên máy",
           prefs::getInt(L"SmartSwitchPerSite", 1) != 0, [](El& e) { prefs::setInt(L"SmartSwitchPerSite", e.on); core::reloadSettings(); });

    header(L"Kiểu gõ");
    combo(0, L"Kiểu gõ", L"Telex đơn giản: [ và ] là dấu ngoặc. Cả ba kiểu đều gõ uw, ow, aw ra ư, ơ, ă", {L"Telex", L"Telex đơn giản 1", L"Telex đơn giản 2"},
          std::min(2, std::max(0, prefs::getInt(L"TelexStyle", 0))), [](El& e) { prefs::setInt(L"TelexStyle", e.index); core::reloadSettings(); });
    combo(0, L"Đặt dấu", L"Vị trí dấu thanh với oa, oe, uy", {L"hoà, thuý (kiểu mới)", L"hòa, thúy (kiểu cũ)"}, prefs::getInt(L"ModernOrthography", 1) ? 0 : 1,
          [](El& e) { prefs::setInt(L"ModernOrthography", e.index == 0 ? 1 : 0); core::reloadSettings(); });
    toggle(0, L"Kiểm tra chính tả", L"Chỉ bỏ dấu khi từ đang gõ đúng quy tắc chính tả tiếng Việt", prefs::getInt(L"Spelling", 1) != 0,
           [](El& e) { prefs::setInt(L"Spelling", e.on); core::reloadSettings(); refreshEnabled(); });
    toggle(0, L"Trả lại phím khi sai", L"Gõ xong một từ sai chính tả, FriKey trả lại đúng các phím bạn đã gõ", prefs::getInt(L"RestoreIfInvalidWord", 1) != 0,
           [](El& e) { prefs::setInt(L"RestoreIfInvalidWord", e.on); core::reloadSettings(); });

    header(L"Khác");
    toggle(0, L"Bảng emoji", L"Tìm emoji bằng tiếng Việt hoặc tiếng Anh, gõ Telex vẫn tìm được. Enter để chèn, Esc để đóng", prefs::getInt(L"EmojiPicker", 0) != 0,
           [](El& e) { prefs::setInt(L"EmojiPicker", e.on); core::reloadSettings(); emoji::settingsChanged(); refreshEnabled(); });
    hotkey(0, L"Phím mở bảng emoji", L"Một phím kèm Ctrl, Alt hoặc Win, ví dụ Ctrl+Alt+E", L"EmojiHotkey", prefs::getInt(L"EmojiHotkey", 0), false);
    toggle(IC_POWER, L"Mở FriKey khi đăng nhập", L"Tự chạy mỗi khi bạn đăng nhập Windows", prefs::launchAtLogin(),
           [](El& e) { prefs::setLaunchAtLogin(e.on); e.on = prefs::launchAtLogin(); });
}

static void runTest();
static void pageTranslate() {
    add(K_TITLE, L"Dịch");
    toggle(IC_GLOBE, L"Chế độ dịch", L"Tắt mỗi khi mở FriKey. Khi bật, icon FriKey ở khay chuyển màu xanh", core::translateEnabled(),
           [](El& e) { core::setTranslateEnabled(e.on); });
    hotkey(0, L"Phím bật/tắt chế độ dịch", L"Một phím kèm Ctrl, Alt hoặc Win", L"TranslateToggleHotkey", toggleKey(), false);
    hotkey(0, L"Phím dịch", L"Dịch đoạn vừa gõ ngay tại chỗ. Bấm lại để có cách dịch khác, Backspace để lấy lại câu gốc, Esc để huỷ khi đang chờ",
           L"TranslateHotkey", translateKey(), false);
    static const int offs[] = {0, 5, 10, 15, 30, 60};
    int cur = prefs::getInt(L"TranslateAutoOffMinutes", 10), ci = 2; for (int i = 0; i < 6; i++) if (offs[i] == cur) ci = i;
    combo(0, L"Tự tắt khi không dùng", L"Chế độ dịch tự tắt sau một lúc không dịch", {L"Không tự tắt", L"Sau 5 phút", L"Sau 10 phút", L"Sau 15 phút", L"Sau 30 phút", L"Sau 60 phút"},
          ci, [](El& e) { prefs::setInt(L"TranslateAutoOffMinutes", offs[e.index]); });

    header(L"Ngôn ngữ");
    combo(0, L"Dịch từ", L"Ngôn ngữ bạn gõ", langOptions(true), langIndex(prefs::getString(L"TranslateSource", L"vi"), true), [](El& e) {
        std::wstring src = langCode(e.index, true), dst = prefs::getString(L"TranslateTarget", L"en");
        if (src == dst) prefs::setString(L"TranslateTarget", prefs::getString(L"TranslateSource", L"vi") == L"auto" ? L"vi" : prefs::getString(L"TranslateSource", L"vi"));
        prefs::setString(L"TranslateSource", src); core::translatePairChanged(); PostMessageW(_w, WM_REFRESH, 0, 0); });
    combo(0, L"Dịch sang", L"Ngôn ngữ của người nhận", langOptions(false), langIndex(prefs::getString(L"TranslateTarget", L"en"), false), [](El& e) {
        std::wstring dst = langCode(e.index, false), src = prefs::getString(L"TranslateSource", L"vi");
        if (src == dst) prefs::setString(L"TranslateSource", prefs::getString(L"TranslateTarget", L"en"));
        prefs::setString(L"TranslateTarget", dst); core::translatePairChanged(); PostMessageW(_w, WM_REFRESH, 0, 0); });
    button(IC_SWAP, L"Đảo chiều", L"Đổi chỗ ngôn ngữ nguồn và đích, khi trả lời người kia bằng ngôn ngữ của họ", L"Đảo chiều", false, [](El&) {
        std::wstring src = prefs::getString(L"TranslateSource", L"vi"), dst = prefs::getString(L"TranslateTarget", L"en");
        if (src == L"auto") return;
        prefs::setString(L"TranslateSource", dst); prefs::setString(L"TranslateTarget", src); core::translatePairChanged(); PostMessageW(_w, WM_REFRESH, 0, 0); });

    header(L"Dịch vụ AI");
    std::vector<std::wstring> names; for (auto& p : PRESETS) names.push_back(p.name);
    combo(0, L"Dịch vụ", L"Bất kỳ dịch vụ nào có API chat/completions kiểu OpenAI", names, presetIndex(), [](El& e) {
        if (e.index < 2) { prefs::setString(L"TranslateHost", PRESETS[e.index].host); prefs::setString(L"TranslatePath", PRESETS[e.index].path); }
        else { prefs::setString(L"TranslateHost", L""); prefs::setString(L"TranslatePath", L""); }
        PostMessageW(_w, WM_REFRESH, 0, 0); });
    int pi = presetIndex();
    field(0, L"Host", L"Tên máy chủ, không có https://", L"TranslateHost", L"api.example.com");
    field(0, L"Đường dẫn", L"Đường dẫn tới chat/completions", L"TranslatePath", L"/v1/chat/completions");
    field(0, L"Model", pi == 0 ? L"Lấy tên trong cột Endpoint ở ai.google.dev/gemini-api/docs/models" : L"Tên model của dịch vụ", L"TranslateModel", PRESETS[pi].model);
    El& key = add(K_CARD, L"API key", translate::hasKey() ? L"Đã lưu trong Windows Credential Manager. Dán key mới để thay" : L"Lưu trong Windows Credential Manager, không hiện lại sau khi lưu");
    key.icon = IC_SHIELD; key.ctl = C_KEY; key.password = true; key.button = L"Lưu"; key.cue = translate::hasKey() ? L"••••••••  (đã lưu)" : L"Dán API key vào đây";
    key.change = [](El& e) {
        int n = GetWindowTextLengthW(e.edit); if (n <= 0) return;
        std::wstring k(n, 0); GetWindowTextW(e.edit, &k[0], n + 1);
        size_t a = k.find_first_not_of(L" \t\r\n"), b = k.find_last_not_of(L" \t\r\n");
        std::wstring t = a == std::wstring::npos ? L"" : k.substr(a, b - a + 1);
        bool ok = !t.empty() && translate::setKey(t);
        SecureZeroMemory(&k[0], k.size() * sizeof(wchar_t)); if (!t.empty()) SecureZeroMemory(&t[0], t.size() * sizeof(wchar_t));
        SetWindowTextW(e.edit, L"");
        e.desc = ok ? L"Đã lưu. Bấm Kiểm tra để thử" : L"Không lưu được key vào Windows Credential Manager";
        PostMessageW(_w, WM_REFRESH, 1, 0); };
    if (translate::hasKey())
        button(0, L"Xoá API key", L"Xoá key đã lưu khỏi máy này", L"Xoá", false, [](El&) {
            if (MessageBoxW(_w, L"Xoá API key đã lưu khỏi máy này?", L"FriKey", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) { translate::setKey(L""); PostMessageW(_w, WM_REFRESH, 0, 0); } });
    button(0, L"Kiểm tra", _testing ? L"Đang gửi “Xin chào, hẹn gặp bạn tối nay nhé!”…" : L"Dịch thử một câu mẫu với cài đặt ở trên", L"Kiểm tra", true, [](El&) { runTest(); });

    header(L"Bảo mật");
    El& n = add(K_CARD, L"Chỉ gửi khi bạn bấm phím dịch",
                L"Chỉ đoạn cần dịch được gửi, qua HTTPS, tới đúng máy chủ ở trên. Không gửi khi chế độ dịch tắt, không dịch trong ô mật khẩu, trình quản lý mật khẩu hay terminal, không ghi nội dung ra đĩa.");
    n.icon = IC_SHIELD;
}

static void pageAbout() {
    add(K_TITLE, L"Giới thiệu");
    add(K_HERO, L"FriKey", L"Vietnamese Input by Friday");
    auto link = [](wchar_t icon, const std::wstring& t, const std::wstring& d, const std::wstring& url) { El& e = add(K_CARD, t, d); e.icon = icon; e.ctl = C_LINK; e.url = url; e.button = L"Mở"; };
    header(L"Mã nguồn và giấy phép");
    link(IC_INFO, L"Mã nguồn mở, giấy phép GPL-3.0", L"FriKey phát triển từ Vikey và OpenKey, phát hành theo GNU General Public License v3.0", L"https://www.gnu.org/licenses/gpl-3.0.html");
    link(0, L"Vikey", L"Bộ gõ gốc của Tony Dinh (Đinh Quang Trung): engine Telex, tự chuyển Anh/Việt, bảng emoji", L"https://github.com/tonyminibot/Vikey");
    link(0, L"OpenKey", L"Bộ gõ của Tuyen Mai, nền tảng của Vikey", L"https://github.com/tuyenvm/OpenKey");
    link(0, L"Dữ liệu của Vikey", L"Từ điển, dữ liệu gõ và emoji (Emojibase, Unicode CLDR) mà Vikey dùng", L"https://vikey.app/acknowledgements");
    link(0, L"Plus Jakarta Sans", L"Font chữ của Tokotype, giấy phép SIL Open Font License 1.1", L"https://github.com/tokotype/PlusJakartaSans");
}

static void build() {
    for (auto& e : _els) if (e.edit) DestroyWindow(e.edit);
    _els.clear(); _hover = -1; _pressed = -1;
    if (_page == 0) pageTyping(); else if (_page == 1) pageTranslate(); else pageAbout();
    for (auto& e : _els) {
        if (e.ctl != C_EDIT && e.ctl != C_KEY) continue;
        e.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP | (e.password ? ES_PASSWORD : 0), 0, 0, 10, 10, _w, nullptr, _inst, nullptr);
        SendMessageW(e.edit, WM_SETFONT, (WPARAM)_editFont, FALSE);
        SendMessageW(e.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        if (!e.pref.empty()) SetWindowTextW(e.edit, prefs::getString(e.pref.c_str(), L"").c_str());
        SendMessageW(e.edit, EM_SETCUEBANNER, TRUE, (LPARAM)e.cue.c_str());
        SetWindowLongPtrW(e.edit, GWLP_USERDATA, 1);
    }
    refreshEnabled();
}

static void refreshEnabled() {
    if (_page != 0) return;
    for (auto& e : _els) {
        if (e.title == L"Tạm dừng 5 giây") e.enabled = prefs::getInt(L"AutoSwitchByTyping", 1) != 0;
        if (e.title == L"Nhớ theo trang web") e.enabled = prefs::getInt(L"UseSmartSwitchKey", 1) != 0;
        if (e.title == L"Trả lại phím khi sai") e.enabled = prefs::getInt(L"Spelling", 1) != 0;
        if (e.title == L"Phím mở bảng emoji") e.enabled = prefs::getInt(L"EmojiPicker", 0) != 0;
    }
    if (_w) InvalidateRect(_w, nullptr, FALSE);
}

// ---- layout ----------------------------------------------------------------------------------
static float ctlWidth(const El& e) {
    switch (e.ctl) {
    case C_TOGGLE: return 40 + 12 + std::max(measureWidth(L"Bật", font(14)), measureWidth(L"Tắt", font(14)));
    case C_COMBO: { float w = 0; for (auto& o : e.options) w = std::max(w, measureWidth(o, font(14))); return std::max(200.0f, w + 11 + 36); }
    case C_HOTKEY: return 220;
    case C_BUTTON: case C_ACCENT: case C_LINK: return std::max(96.0f, measureWidth(e.button, font(14)) + 24 + (e.ctl == C_LINK ? 22 : 0));
    case C_EDIT: return 320;
    case C_KEY: return 280 + 8 + 80;
    case C_SEG: { float w = 0; for (auto& o : e.options) w += measureWidth(o, font(14)) + 32; return w + 4; }
    default: return 0;
    }
}

static void layout() {
    if (!_w) return;
    RECT rc; GetClientRect(_w, &rc);
    float W = rc.right / _s;
    float x0 = NAV_W + PAD_X, cw = std::min(MAX_W, W - x0 - PAD_X);
    if (cw < 300) cw = 300;
    float y = 0;
    for (auto& e : _els) {
        switch (e.kind) {
        case K_TITLE: e.rect = R(x0, 28, cw, 40); y = 28 + 40 + 20; break;
        case K_HEADER: y += 14; e.rect = R(x0, y, cw, 22); y += 22 + 8; break;
        case K_HERO: e.rect = R(x0, y, cw, 112); y += 112 + 8; break;
        case K_CARD: {
            float cwid = ctlWidth(e), left = CARD_PAD + (e.icon ? 36 : 0), textW = cw - left - CARD_PAD - (cwid ? cwid + 24 : 0);
            // too narrow for the control beside the text (a text box in a small window): the control goes under it
            bool stacked = cwid > 0 && textW < 200;
            if (stacked) textW = cw - left - CARD_PAD;
            float th = measureHeight(e.title, font(14), textW), dh = e.desc.empty() ? 0 : measureHeight(e.desc, font(12), textW) + 2;
            float h = std::max(MIN_CARD, th + dh + 2 * 15) + (stacked ? CTL_H + 12 : 0);
            e.rect = R(x0, y, cw, h);
            e.textRight = x0 + left + textW;
            e.textTop = stacked ? y + 15 : y + (h - th - dh) / 2;
            float cx = x0 + cw - CARD_PAD - cwid, cy = y + (h - CTL_H) / 2;
            if (stacked) { cy = y + 15 + th + dh + 12; if (e.ctl == C_EDIT || e.ctl == C_KEY) { cx = x0 + left; cwid = cw - left - CARD_PAD; } }
            e.ctlRect = R(cx, cy, cwid, CTL_H);
            if (e.ctl == C_KEY) { float ew = cwid - 88; e.ctlRect = R(cx, cy, ew, CTL_H); e.btnRect = R(cx + ew + 8, cy, 80, CTL_H); }
            y += h + CARD_GAP; break; }
        }
    }
    _contentH = y + 40;
    float view = rc.bottom / _s;
    _scroll = std::max(0.0f, std::min(_scroll, _contentH - view));
    for (auto& e : _els) {
        if (!e.edit) continue;
        D2D1_RECT_F r = e.ctlRect; float top = r.top - _scroll;
        bool visible = top > 0 && top + CTL_H < view && _recording < 0;   // hidden under the shortcut dialog
        float lineH = 20;
        MoveWindow(e.edit, (int)std::lround((r.left + 11) * _s), (int)std::lround((top + (CTL_H - lineH) / 2) * _s),
                   (int)std::lround((r.right - r.left - 22) * _s), (int)std::lround(lineH * _s), TRUE);
        ShowWindow(e.edit, visible ? SW_SHOWNA : SW_HIDE);
        if (visible) InvalidateRect(e.edit, nullptr, TRUE);
        EnableWindow(e.edit, e.enabled);
    }
}

// ---- drawing ---------------------------------------------------------------------------------
static D2D1_RECT_F off(D2D1_RECT_F r) { r.top -= _scroll; r.bottom -= _scroll; return r; }
static bool inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

static void drawControlFrame(const D2D1_RECT_F& r, bool hover, bool enabled, bool focus) {
    const Theme& t = theme();
    fill(_rt, r, 4, enabled ? (hover ? t.controlHover : t.control) : t.subtle);
    stroke(_rt, r, 4, t.controlStroke);
    if (focus) { fill(_rt, D2D1::RectF(r.left + 1, r.bottom - 2, r.right - 1, r.bottom), 1, t.accent); }
    else line(_rt, r.left + 4, r.bottom - 0.5f, r.right - 4, r.bottom - 0.5f, t.controlBottom);
}
static void drawButton(const D2D1_RECT_F& r, const std::wstring& label, bool accent, bool hover, bool enabled, wchar_t icon = 0) {
    const Theme& t = theme();
    if (accent && enabled) { fill(_rt, r, 4, hover ? t.accentHover : t.accent); }
    else drawControlFrame(r, hover, enabled, false);
    D2D1_COLOR_F ink = accent && enabled ? t.accentText : enabled ? t.text : t.text3;
    if (icon) {
        float tw = measureWidth(label, font(14)); float x = (r.left + r.right - tw - 22) / 2;
        text(_rt, label, font(14), D2D1::RectF(x, r.top, x + tw + 1, r.bottom), ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        text(_rt, std::wstring(1, icon), icons(12), R(x + tw + 6, r.top, 16, r.bottom - r.top), ink);
    } else text(_rt, label, font(14), r, ink, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}
static void drawToggle(const D2D1_RECT_F& r, bool on, bool hover, bool enabled) {
    const Theme& t = theme();
    float sx = r.right - 40, sy = (r.top + r.bottom) / 2 - 10;
    D2D1_RECT_F track = R(sx, sy, 40, 20);
    text(_rt, on ? L"Bật" : L"Tắt", font(14), D2D1::RectF(r.left, r.top, sx - 12, r.bottom), enabled ? t.text : t.text3, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (on) {
        fill(_rt, track, 10, enabled ? (hover ? t.accentHover : t.accent) : t.text3);
        float k = hover ? 7 : 6; fill(_rt, R(sx + 30 - k, sy + 10 - k, 2 * k, 2 * k), k, enabled ? t.accentText : t.card);
    } else {
        fill(_rt, track, 10, hover && enabled ? t.controlHover : t.control);
        stroke(_rt, track, 10, enabled ? t.toggleOffStroke : t.text3);
        float k = hover ? 7 : 6; fill(_rt, R(sx + 10 - k, sy + 10 - k, 2 * k, 2 * k), k, enabled ? t.text2 : t.text3);
    }
}
/// Key caps ending at `right` (centred on it with center=true); returns their total width.
static float drawCaps(const std::vector<std::wstring>& keys, float right, float top, float h, float size, bool accent, bool enabled, bool center = false) {
    const Theme& t = theme();
    float gap = h * 0.18f, total = 0; std::vector<float> ws;
    for (auto& k : keys) { float w = std::max(h * 1.15f, measureWidth(k, font(size, 600)) + h * 0.7f); ws.push_back(w); total += w; }
    total += gap * (keys.empty() ? 0 : keys.size() - 1);
    float x = center ? right - total / 2 : right - total;
    for (size_t i = 0; i < keys.size(); i++) {
        D2D1_RECT_F k = R(x, top, ws[i], h);
        if (accent && enabled) fill(_rt, k, h * 0.16f, t.accent);
        else { fill(_rt, k, h * 0.16f, enabled ? t.control : t.subtle); stroke(_rt, k, h * 0.16f, t.controlStroke); line(_rt, k.left + 4, k.bottom - 0.5f, k.right - 4, k.bottom - 0.5f, t.controlBottom); }
        text(_rt, keys[i], font(size, 600), k, accent && enabled ? t.accentText : enabled ? t.text : t.text3, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        x += ws[i] + gap;
    }
    return total;
}
static void drawKeys(const D2D1_RECT_F& r, int hk, bool, bool hover, bool enabled) {
    const Theme& t = theme();
    D2D1_RECT_F pen = R(r.right - 32, r.top, 32, r.bottom - r.top);   // the edit button
    if (hover && enabled) fill(_rt, pen, 4, t.subtleHover);
    text(_rt, std::wstring(1, (wchar_t)0xE70F), icons(14), pen, enabled ? t.text : t.text3);
    std::vector<std::wstring> keys = keysOf(hk);
    if (keys.empty()) { text(_rt, L"Chưa đặt", font(14), D2D1::RectF(r.left, r.top, pen.left - 10, r.bottom), t.text3, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER); return; }
    drawCaps(keys, pen.left - 10, r.top, r.bottom - r.top, 13, true, enabled);
}
static void drawSeg(const El& e, const D2D1_RECT_F& r, bool enabled) {
    const Theme& t = theme();
    fill(_rt, r, 6, t.control); stroke(_rt, r, 6, t.controlStroke);
    float x = r.left + 2;
    for (int i = 0; i < (int)e.options.size(); i++) {
        float w = measureWidth(e.options[i], font(14)) + 32;
        D2D1_RECT_F s = R(x, r.top + 2, w, r.bottom - r.top - 4);
        if (i == e.index) fill(_rt, s, 4, enabled ? t.accent : t.text3);
        text(_rt, e.options[i], font(14, i == e.index ? 600 : 400), s, i == e.index ? t.accentText : t.text, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        x += w;
    }
}

static void paint() {
    if (!_rt) {
        RECT rc; GetClientRect(_w, &rc);
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96 * _s, 96 * _s);
        d2d()->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(_w, D2D1::SizeU(rc.right, rc.bottom)), &_rt);
        if (!_rt) return;
    }
    const Theme& t = theme();
    RECT rc; GetClientRect(_w, &rc); float W = rc.right / _s, H = rc.bottom / _s;
    _rt->BeginDraw(); _rt->Clear(t.bg);
    _rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    // navigation
    logo(_rt, 18, 18, 32);
    text(_rt, L"FriKey", font(16, 700), R(60, 16, NAV_W - 64, 22), t.text);
    text(_rt, L"Vietnamese Input by Friday", font(11), R(60, 37, NAV_W - 64, 16), t.text2);
    static const struct { wchar_t icon; const wchar_t* label; } NAV[] = {{IC_KEYBOARD, L"Gõ tiếng Việt"}, {IC_GLOBE, L"Dịch"}, {IC_INFO, L"Giới thiệu"}};
    for (int i = 0; i < 3; i++) {
        D2D1_RECT_F r = R(8, 72 + i * 40.0f, NAV_W - 16, 36);
        if (i == _page) fill(_rt, r, 4, t.subtleHover); else if (i == _hoverNav) fill(_rt, r, 4, t.subtle);
        if (i == _page) fill(_rt, R(r.left, r.top + 10, 3, 16), 1.5f, t.accent);
        text(_rt, std::wstring(1, NAV[i].icon), icons(16), R(r.left + 12, r.top, 24, 36), t.text);
        text(_rt, NAV[i].label, font(14, i == _page ? 600 : 400), D2D1::RectF(r.left + 48, r.top, r.right, r.bottom), t.text, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    std::wstring ver = L"Phiên bản " WIDEN(FRIKEY_VERSION);
    text(_rt, ver, font(11.5f), R(20, H - 30, NAV_W - 32, 16), t.text3);
    // content
    _rt->PushAxisAlignedClip(D2D1::RectF(NAV_W, 0, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < (int)_els.size(); i++) {
        El& e = _els[i]; D2D1_RECT_F r = off(e.rect);
        if (r.bottom < 0 || r.top > H) continue;
        bool hover = i == _hover && e.enabled;
        switch (e.kind) {
        case K_TITLE: text(_rt, e.title, font(28, 600), r, t.text, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER); break;
        case K_HEADER: text(_rt, e.title, font(14, 600), r, t.text); break;
        case K_HERO:
            fill(_rt, r, 8, t.card); stroke(_rt, r, 8, t.cardStroke);
            logo(_rt, r.left + 24, r.top + 24, 64);
            text(_rt, e.title, font(26, 700), R(r.left + 108, r.top + 22, 400, 34), t.text);
            text(_rt, e.desc, font(14), R(r.left + 108, r.top + 56, 500, 20), t.text2);
            text(_rt, L"Phiên bản " WIDEN(FRIKEY_VERSION) L" · dựa trên Vikey 1.0.33", font(12), R(r.left + 108, r.top + 78, 500, 18), t.text3);
            break;
        case K_CARD: {
            bool clickable = e.ctl != C_NONE && e.ctl != C_EDIT && e.ctl != C_KEY;
            fill(_rt, r, 6, hover && clickable ? t.cardHover : t.card); stroke(_rt, r, 6, t.cardStroke);
            float left = r.left + CARD_PAD;
            if (e.icon) { text(_rt, std::wstring(1, e.icon), icons(18), R(left, r.top, 20, r.bottom - r.top), e.enabled ? t.text : t.text3); left += 36; }
            D2D1_RECT_F cr = off(e.ctlRect);
            float textRight = e.textRight;
            float th = measureHeight(e.title, font(14), textRight - left), dh = e.desc.empty() ? 0 : measureHeight(e.desc, font(12), textRight - left) + 2;
            float ty = e.textTop - _scroll;
            text(_rt, e.title, font(14), D2D1::RectF(left, ty, textRight, ty + th + 2), e.enabled ? t.text : t.text3);
            if (!e.desc.empty()) text(_rt, e.desc, font(12), D2D1::RectF(left, ty + th + 2, textRight, ty + th + dh + 4), e.enabled ? t.text2 : t.text3);
            bool focus = e.edit && GetFocus() == e.edit;
            switch (e.ctl) {
            case C_TOGGLE: drawToggle(cr, e.on, hover, e.enabled); break;
            case C_COMBO:
                drawControlFrame(cr, hover, e.enabled, false);
                text(_rt, e.index >= 0 && e.index < (int)e.options.size() ? e.options[e.index] : L"", font(14), D2D1::RectF(cr.left + 11, cr.top, cr.right - 32, cr.bottom), e.enabled ? t.text : t.text3, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                text(_rt, std::wstring(1, IC_CHEVRON_D), icons(12), R(cr.right - 32, cr.top, 24, CTL_H), t.text2);
                break;
            case C_HOTKEY: drawKeys(cr, e.hotkey, _recording == i, hover, e.enabled); break;
            case C_BUTTON: case C_ACCENT: drawButton(cr, e.button, e.ctl == C_ACCENT, hover, e.enabled); break;
            case C_LINK: drawButton(cr, e.button, false, hover, e.enabled, IC_LINK); break;
            case C_EDIT: drawControlFrame(cr, false, e.enabled, focus); break;
            case C_KEY: drawControlFrame(cr, false, e.enabled, focus); drawButton(off(e.btnRect), e.button, true, hover && _hoverBtn, e.enabled); break;
            case C_SEG: drawSeg(e, cr, e.enabled); break;
            default: break;
            }
            break; }
        }
    }
    // scroll bar
    if (_contentH > H) {
        float th = std::max(40.0f, H * H / _contentH), ty = (_scroll / (_contentH - H)) * (H - th);
        fill(_rt, R(W - 7, ty + 2, 3, th - 4), 1.5f, t.text3);
    }
    _rt->PopAxisAlignedClip();
    drawDialog();
    if (_rt->EndDraw() == D2DERR_RECREATE_TARGET) { _rt->Release(); _rt = nullptr; }
}

// ---- the shortcut dialog (like PowerToys: press the keys, see them, Lưu) -------------------------------
static int _dlgValue = 0, _dlgHeld = 0, _dlgHover = 0; static bool _dlgValid = false, _dlgKeyInChord = false; static std::wstring _dlgMsg;
enum { DB_NONE, DB_SAVE, DB_CANCEL, DB_DEFAULT };
static int defaultHotkey(const std::wstring& pref) {
    if (pref == L"SwitchKeyStatus") return 0x7A000206 & 0xFFFF;
    if (pref == L"TranslateHotkey") return KEY_T | 0x200;
    if (pref == L"TranslateToggleHotkey") return KEY_T | 0x200 | 0x800;
    return 0;
}
static std::wstring ruleOf(const El& e) {
    return e.modsOnly ? L"Một phím kèm Ctrl, Alt, Shift hoặc Win, hoặc chỉ hai phím bổ trợ (ví dụ Ctrl+Shift)" : L"Một phím kèm Ctrl, Alt hoặc Win (có thể thêm Shift)";
}
static D2D1_RECT_F dlgRect() {
    RECT rc; GetClientRect(_w, &rc); float W = rc.right / _s, H = rc.bottom / _s, dw = 480, dh = 300;
    return R((W - dw) / 2, (H - dh) / 2, dw, dh);
}
static D2D1_RECT_F dlgButton(int which) {
    D2D1_RECT_F d = dlgRect(); float y = d.bottom - 24 - 32, bw = 120;
    if (which == DB_SAVE) return R(d.right - 24 - 2 * bw - 8, y, bw, 32);
    if (which == DB_CANCEL) return R(d.right - 24 - bw, y, bw, 32);
    return R(d.left + 24, y, bw, 32);
}
static int dlgHit(float x, float y) { for (int b : {DB_SAVE, DB_CANCEL, DB_DEFAULT}) if (inside(dlgButton(b), x, y)) return b; return DB_NONE; }
static void stopRecording() {
    if (_recording < 0) return;
    _recording = -1; core::setKeysPaused(false); layout(); InvalidateRect(_w, nullptr, FALSE);
}
static void openDialog(int i) {
    stopRecording();
    _recording = i; _dlgValue = _els[i].hotkey; _dlgHeld = 0; _dlgValid = false; _dlgKeyInChord = false; _dlgHover = DB_NONE; _dlgMsg = ruleOf(_els[i]);
    core::setKeysPaused(true); SetFocus(_w); layout(); InvalidateRect(_w, nullptr, FALSE);
}
/// A combination was pressed: can it be used here?
static void tryCombination(int value) {
    El& e = _els[_recording];
    _dlgValue = value; _dlgValid = false;
    if (!e.modsOnly && !(value & 0x700)) { _dlgMsg = L"Cần có Ctrl, Alt hoặc Win: Shift với một phím là chữ hoa"; return; }
    if (clashes(e.pref, value)) { _dlgMsg = L"Tổ hợp này đã dùng cho việc khác trong FriKey"; return; }
    _dlgValid = true; _dlgMsg = L"Bấm Lưu (hoặc Enter) để dùng tổ hợp này";
}
static void drawDialog() {
    if (_recording < 0) return;
    const Theme& t = theme(); El& e = _els[_recording];
    RECT rc; GetClientRect(_w, &rc);
    fill(_rt, D2D1::RectF(0, 0, rc.right / _s, rc.bottom / _s), 0, D2D1::ColorF(0, 0, 0, t.dark ? 0.45f : 0.3f));
    D2D1_RECT_F d = dlgRect();
    fill(_rt, d, 8, t.dark ? D2D1::ColorF(0x2B2B2B) : D2D1::ColorF(0xFFFFFF));
    fill(_rt, D2D1::RectF(d.left, d.bottom - 80, d.right, d.bottom), 8, t.bg);   // the button bar
    fill(_rt, D2D1::RectF(d.left, d.bottom - 80, d.right, d.bottom - 70), 0, t.bg);
    line(_rt, d.left, d.bottom - 80, d.right, d.bottom - 80, t.cardStroke);
    stroke(_rt, d, 8, t.cardStroke);
    text(_rt, L"Đặt phím tắt", font(20, 600), R(d.left + 24, d.top + 22, d.right - d.left - 48, 28), t.text);
    text(_rt, e.title, font(13), R(d.left + 24, d.top + 52, d.right - d.left - 48, 18), t.text2);
    // the keys: the modifiers held right now, else the combination pressed, else the current one
    std::vector<std::wstring> keys; bool accent = false;
    if (_dlgHeld) keys = keysOf(0xFE | _dlgHeld);
    else { keys = keysOf(_dlgValue); accent = _dlgValid || _dlgValue == e.hotkey; }
    D2D1_RECT_F stage = R(d.left + 24, d.top + 84, d.right - d.left - 48, 84);
    fill(_rt, stage, 6, t.subtle);
    if (keys.empty()) text(_rt, L"Bấm tổ hợp phím…", font(15), stage, t.text3, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    else drawCaps(keys, (stage.left + stage.right) / 2, stage.top + 18, 48, 18, accent, true, true);
    bool bad = !_dlgValid && !_dlgHeld && _dlgMsg != ruleOf(e);
    text(_rt, std::wstring(1, bad ? (wchar_t)0xE783 : (wchar_t)0xE946), icons(13), R(d.left + 24, d.top + 178, 18, 20), bad ? t.danger : t.text2);
    text(_rt, _dlgMsg, font(12.5f), R(d.left + 48, d.top + 179, d.right - d.left - 72, 36), bad ? t.danger : t.text2);
    drawButton(dlgButton(DB_SAVE), L"Lưu", true, _dlgHover == DB_SAVE, _dlgValid);
    drawButton(dlgButton(DB_CANCEL), L"Huỷ", false, _dlgHover == DB_CANCEL, true);
    drawButton(dlgButton(DB_DEFAULT), L"Mặc định", false, _dlgHover == DB_DEFAULT, true);
}
static void dialogKey(UINT msg, DWORD vk) {
    El& e = _els[_recording];
    bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0, shift = GetKeyState(VK_SHIFT) < 0, win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    bool isMod = vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                 vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT;
    int mods = (ctrl ? 0x100 : 0) | (alt ? 0x200 : 0) | (win ? 0x400 : 0) | (shift ? 0x800 : 0);
    bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (down) {
        if (!mods && vk == VK_ESCAPE) { stopRecording(); return; }
        if (!mods && vk == VK_RETURN) { if (_dlgValid) PostMessageW(_w, WM_COMMAND, 0xF00D, 0); return; }
        if (isMod) { if (!_dlgHeld) _dlgKeyInChord = false; _dlgHeld = mods; }
        else {
            int key = (int)engineKey(vk);
            if (key != KEY_EMPTY && mods) { _dlgKeyInChord = true; _dlgHeld = 0; tryCombination(key | mods); }
            else if (key != KEY_EMPTY) { _dlgValue = 0; _dlgValid = false; _dlgMsg = ruleOf(e); }
        }
    } else if (isMod) {
        // modifiers let go with no key in between: the modifiers alone (the switch shortcut only, two or more)
        int was = _dlgHeld;
        if (was && !_dlgKeyInChord) {
            int n = !!(was & 0x100) + !!(was & 0x200) + !!(was & 0x400) + !!(was & 0x800);
            if (e.modsOnly && n >= 2) tryCombination(0xFE | was);
        }
        _dlgHeld = 0;
        if (!mods) _dlgKeyInChord = false;
    }
    InvalidateRect(_w, nullptr, FALSE);
}
static void commitHotkey(int value) {
    if (_recording < 0) return;
    El& e = _els[_recording];
    if (e.pref == L"SwitchKeyStatus") value |= switchKey() & 0x8000;
    prefs::setInt(e.pref.c_str(), value); e.hotkey = value;
    core::reloadSettings(); if (e.pref == L"EmojiHotkey") emoji::settingsChanged();
    stopRecording();
    PostMessageW(_w, WM_REFRESH, 0, 0);
}

static void runTest() {
    if (_testing) return;
    if (!translate::configured()) { if (El* e = find(L"Kiểm tra")) { e->desc = L"Còn thiếu host, đường dẫn, model hoặc API key"; InvalidateRect(_w, nullptr, FALSE); } return; }
    _testing = true;
    translate::Request q; q.text = L"Xin chào, hẹn gặp bạn tối nay nhé!"; q.source = L"vi"; q.target = prefs::getString(L"TranslateTarget", L"en");
    if (q.target == L"vi") q.target = L"en";
    q.id = ++_testId | 0x80000000u; q.reply = _w;
    translate::request(q);
    if (El* e = find(L"Kiểm tra")) { e->desc = L"Đang gửi “Xin chào, hẹn gặp bạn tối nay nhé!”…"; InvalidateRect(_w, nullptr, FALSE); }
}

static void clickElement(int i, float x, float y) {
    El& e = _els[i];
    if (!e.enabled) return;
    switch (e.ctl) {
    case C_TOGGLE: e.on = !e.on; if (e.change) e.change(e); break;
    case C_COMBO: {
        std::vector<menu::Item> items;
        for (int k = 0; k < (int)e.options.size(); k++) { menu::Item it; it.id = k + 1; it.text = e.options[k]; it.checked = k == e.index; items.push_back(it); }
        D2D1_RECT_F cr = off(e.ctlRect);
        POINT p = {(LONG)std::lround(cr.left * _s), (LONG)std::lround(cr.bottom * _s) + 2}; ClientToScreen(_w, &p);
        int id = menu::show(_w, p, items, menu::BelowPoint, cr.right - cr.left);
        if (id > 0 && id - 1 != e.index) { e.index = id - 1; if (e.change) e.change(e); }
        break; }
    case C_HOTKEY:
        openDialog(i); break;
    case C_BUTTON: case C_ACCENT: if (e.change) e.change(e); break;
    case C_LINK: ShellExecuteW(_w, L"open", e.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case C_SEG: {
        D2D1_RECT_F cr = off(e.ctlRect); float sx = cr.left + 2;
        for (int k = 0; k < (int)e.options.size(); k++) { float w = measureWidth(e.options[k], font(14)) + 32; if (x >= sx && x < sx + w && k != e.index) { e.index = k; if (e.change) e.change(e); } sx += w; }
        break; }
    case C_KEY: if (inside(off(e.btnRect), x, y) && e.change) e.change(e); break;
    default: break;
    }
    (void)y;
    InvalidateRect(_w, nullptr, FALSE);
}

static int hitEl(float x, float y, bool* onBtn = nullptr) {
    if (onBtn) *onBtn = false;
    for (int i = 0; i < (int)_els.size(); i++) {
        El& e = _els[i]; if (e.kind != K_CARD || e.ctl == C_NONE || e.ctl == C_EDIT) continue;
        if (e.ctl == C_KEY) { if (inside(off(e.btnRect), x, y)) { if (onBtn) *onBtn = true; return i; } continue; }
        D2D1_RECT_F r = e.ctl == C_TOGGLE || e.ctl == C_BUTTON || e.ctl == C_ACCENT || e.ctl == C_LINK || e.ctl == C_COMBO || e.ctl == C_HOTKEY ? off(e.rect) : off(e.ctlRect);
        if (e.ctl == C_SEG) r = off(e.ctlRect);
        if (inside(r, x, y)) return i;
    }
    return -1;
}
static int hitNav(float x, float y) { for (int i = 0; i < 3; i++) if (inside(R(8, 72 + i * 40.0f, NAV_W - 16, 36), x, y)) return i; return -1; }

static void setPage(int p) { if (p == _page) return; stopRecording(); _page = p; _scroll = 0; build(); layout(); InvalidateRect(_w, nullptr, FALSE); }

static void makeEditResources() {
    if (_editFont) DeleteObject(_editFont);
    _editFont = CreateFontW(-(int)std::lround(14 * _s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, gdiFace());
    for (auto& e : _els) if (e.edit) SendMessageW(e.edit, WM_SETFONT, (WPARAM)_editFont, TRUE);
    if (_editBrush) DeleteObject(_editBrush);
    _editBrush = CreateSolidBrush(gdi(theme().control, theme().card));
}

static void saveField(El& e) {
    if (e.pref.empty() || !e.edit) return;
    int n = GetWindowTextLengthW(e.edit); std::wstring v(n, 0); if (n) GetWindowTextW(e.edit, &v[0], n + 1);
    size_t a = v.find_first_not_of(L" \t"), b = v.find_last_not_of(L" \t"); v = a == std::wstring::npos ? L"" : v.substr(a, b - a + 1);
    if (e.pref == L"TranslateHost") { size_t p = v.find(L"://"); if (p != std::wstring::npos) v = v.substr(p + 3); size_t s = v.find(L'/'); if (s != std::wstring::npos) v = v.substr(0, s); }
    if (e.pref == L"TranslatePath" && !v.empty() && v[0] != L'/') v = L"/" + v;
    prefs::setString(e.pref.c_str(), v);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM lp) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); paint(); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: if (_rt) _rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp))); layout(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = (LONG)(820 * _s); mm->ptMinTrackSize.y = (LONG)(560 * _s); return 0; }
    case WM_DPICHANGED: {
        _s = HIWORD(w) / 96.0f; RECT* r = (RECT*)lp;
        if (_rt) { _rt->Release(); _rt = nullptr; }
        makeEditResources();
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout(); InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_MOUSEWHEEL: {
        RECT rc; GetClientRect(h, &rc); float view = rc.bottom / _s;
        float ns = std::max(0.0f, std::min(_scroll - GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 60, std::max(0.0f, _contentH - view)));
        if (ns != _scroll) { _scroll = ns; layout(); InvalidateRect(h, nullptr, FALSE); }
        return 0; }
    case WM_MOUSEMOVE: {
        if (_recording >= 0) { int b = dlgHit(GET_X_LPARAM(lp) / _s, GET_Y_LPARAM(lp) / _s); if (b != _dlgHover) { _dlgHover = b; InvalidateRect(h, nullptr, FALSE); } return 0; }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        float x = GET_X_LPARAM(lp) / _s, y = GET_Y_LPARAM(lp) / _s; bool onBtn;
        int i = hitEl(x, y, &onBtn), n = hitNav(x, y);
        if (i != _hover || n != _hoverNav || onBtn != _hoverBtn) { _hover = i; _hoverNav = n; _hoverBtn = onBtn; InvalidateRect(h, nullptr, FALSE); }
        return 0; }
    case WM_MOUSELEAVE: _hover = -1; _hoverNav = -1; _hoverBtn = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (_hover >= 0 || _hoverNav >= 0)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(lp) / _s, y = GET_Y_LPARAM(lp) / _s;
        if (_recording >= 0) { _pressed = dlgHit(x, y); return 0; }
        _pressed = hitEl(x, y); _pressedNav = hitNav(x, y);
        SetFocus(h); SetCapture(h); return 0; }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        float x = GET_X_LPARAM(lp) / _s, y = GET_Y_LPARAM(lp) / _s;
        if (_recording >= 0) {
            int b = dlgHit(x, y);
            if (b != DB_NONE && b == _pressed) {
                if (b == DB_SAVE && _dlgValid) commitHotkey(_dlgValue);
                else if (b == DB_CANCEL) stopRecording();
                else if (b == DB_DEFAULT) {
                    int d = defaultHotkey(_els[_recording].pref);
                    if (d) tryCombination(d); else { _dlgValue = 0; _dlgValid = true; _dlgMsg = L"Bấm Lưu để bỏ phím tắt này"; }
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            _pressed = -1; return 0;
        }
        int i = hitEl(x, y), n = hitNav(x, y);
        if (n >= 0 && n == _pressedNav) setPage(n);
        else if (i >= 0 && i == _pressed) clickElement(i, x, y);
        _pressed = _pressedNav = -1; return 0; }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (_recording < 0) break;
        dialogKey(m, (DWORD)w); return 0;
    case WM_SYSCHAR: case WM_CHAR: if (_recording >= 0) return 0; break;
    case WM_KILLFOCUS: stopRecording(); break;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) stopRecording(); break;
    case WM_COMMAND:
        if (w == 0xF00D) { if (_recording >= 0 && _dlgValid) commitHotkey(_dlgValue); return 0; }   // Enter in the shortcut dialog
        if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) {
            for (auto& e : _els) if (e.edit == (HWND)lp) { if (HIWORD(w) == EN_KILLFOCUS) saveField(e); }
            InvalidateRect(h, nullptr, FALSE);
        } else if (HIWORD(w) == EN_CHANGE) {
            for (auto& e : _els) if (e.edit == (HWND)lp && e.ctl == C_EDIT) saveField(e);
        }
        return 0;
    case WM_CTLCOLOREDIT: {
        const Theme& t = theme();
        SetTextColor((HDC)w, gdi(t.text, t.card)); SetBkColor((HDC)w, gdi(t.control, t.card)); return (LRESULT)_editBrush; }
    case WM_REFRESH: {
        bool keepDesc = w == 1;   // the API key card's message stays
        std::wstring keyDesc; if (keepDesc) if (El* e = find(L"API key")) keyDesc = e->desc;
        float sc = _scroll; build(); _scroll = sc;
        if (keepDesc) if (El* e = find(L"API key")) e->desc = keyDesc;
        layout(); InvalidateRect(h, nullptr, FALSE); return 0; }
    case translate::WM_TRANSLATED: {
        translate::Result r;
        if (!translate::take((unsigned)w, r)) return 0;
        _testing = false;
        if (El* e = find(L"Kiểm tra")) e->desc = r.ok ? L"Kết quả: " + r.text : L"Lỗi: " + r.error;
        layout(); InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_SETTINGCHANGE:
        if (lp && wcscmp((LPCWSTR)lp, L"ImmersiveColorSet") == 0) { refreshTheme(); applyWindowStyle(h, false); makeEditResources(); InvalidateRect(h, nullptr, TRUE); }
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        stopRecording();
        for (auto& e : _els) if (e.edit && e.ctl == C_EDIT) saveField(e);
        _els.clear(); if (_rt) { _rt->Release(); _rt = nullptr; } _w = nullptr; return 0;
    }
    return DefWindowProcW(h, m, w, lp);
}

void showSettings(HINSTANCE inst, int page) {
    _inst = inst;
    if (_w) { setPage(page); ShowWindow(_w, IsIconic(_w) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(_w); return; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"FriKeySettings";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP)); wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassW(&wc); reg = true;
    }
    refreshTheme();
    POINT pt; GetCursorPos(&pt); HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
    _w = CreateWindowExW(0, L"FriKeySettings", L"FriKey", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, mi.rcWork.left, mi.rcWork.top, 100, 100, nullptr, nullptr, inst, nullptr);
    _s = scaleOf(_w);
    applyWindowStyle(_w, false);
    makeEditResources();
    _page = page; _scroll = 0; build();
    int cw = (int)(1060 * _s), ch = (int)(760 * _s);
    RECT wr = {0, 0, cw, ch}; AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int ww = std::min((int)(wr.right - wr.left), (int)(mi.rcWork.right - mi.rcWork.left)), wh = std::min((int)(wr.bottom - wr.top), (int)(mi.rcWork.bottom - mi.rcWork.top));
    SetWindowPos(_w, nullptr, (mi.rcWork.left + mi.rcWork.right - ww) / 2, (mi.rcWork.top + mi.rcWork.bottom - wh) / 2, ww, wh, SWP_NOZORDER);
    layout();
    ShowWindow(_w, SW_SHOW); SetForegroundWindow(_w);
}

void languageChanged(int lang) {
    emoji::languageChanged(lang);
    if (_w && _page == 0) for (auto& e : _els) if (e.ctl == C_SEG) { e.index = lang ? 0 : 1; InvalidateRect(_w, nullptr, FALSE); }
}
void refreshSettings() {
    if (!_w) return;
    for (auto& e : _els) if (e.title == L"Chế độ dịch") e.on = core::translateEnabled();
    InvalidateRect(_w, nullptr, FALSE);
}

}}
