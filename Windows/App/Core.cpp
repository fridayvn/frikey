#include "Core.h"
#include "Site.h"
#include "KeyMap.h"
#include "Prefs.h"
#include "Engine.h"
#include "Translate.h"
#include <thread>
#include <psapi.h>
#include <mmsystem.h>
#include "resource.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <mutex>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

// Settings the engine reads (see Engine.h). Same defaults as the macOS app.
int vLanguage = 1, vSwitchKeyStatus = 0x7A000206, vCheckSpelling = 1, vUseModernOrthography = 1;
int vRestoreIfWrongSpelling = 1, vFixRecommendBrowser = 1, vUseSmartSwitchKey = 1, vOtherLanguage = 1;
int vAutoSwitchByTyping = 0, vPerformLayoutCompat = 0;   // FriKey: off unless turned on
// Emoji picker (Settings.cpp): its hotkey (0 = off); while it is open keys go to the picker, not the engine (Windows' own shortcuts and media keys excepted)
static int vEmojiHotkey = 0;
static volatile bool vEmojiOpen = false;
static HWND vEmojiWindow = nullptr;

namespace vikey { namespace core {

/// The switch sound (Tony picked "sparkle", 2026-10-29): two tiny notes, rising into Vietnamese, falling into
/// English. The same WAVs as the Mac's .caf files, embedded in Vikey.exe (Vikey.rc).
static void PlaySwitchSound(int language) {
    static const void* data[2] = {nullptr, nullptr};
    int i = language == 1 ? 1 : 0;
    if (!data[i]) {
        HRSRC r = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(i ? IDR_SOUND_VI : IDR_SOUND_EN), RT_RCDATA);
        HGLOBAL g = r ? LoadResource(GetModuleHandleW(nullptr), r) : nullptr;
        data[i] = g ? LockResource(g) : nullptr;
    }
    if (!data[i]) { MessageBeep(MB_OK); return; }
    PlaySoundW((LPCWSTR)data[i], nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// Marks keystrokes Vikey posts itself, so the hook can skip them (dwExtraInfo).
static const ULONG_PTR VIKEY_EVENT_TAG = 0x0F341991;
static const int EMPTY_HOTKEY = 0xFE0000FE;

// modifier flags, mirroring CGEventFlags usage on macOS
enum : unsigned { F_SHIFT = 1, F_CONTROL = 2, F_ALT = 4, F_WIN = 8, F_CAPS = 16 };
#define OTHER_CONTROL_KEY ((_flag & F_CONTROL) || (_flag & F_ALT) || (_flag & F_WIN))

static HHOOK _kbHook = nullptr, _mouseHook = nullptr;
static HWINEVENTHOOK _focusHook = nullptr, _fgHook = nullptr;
static vKeyHookState* pData = nullptr;
static int _trHotkey = 0, _trToggleKey = 0;   // translate mode keys (reloadSettings)
static unsigned _flag = 0, _lastFlag = 0;
static bool _modifierChordUsed = false;   // the modifiers held went with a click or another key: no modifier-only hotkey until all are released
static DWORD _hotkeyVk = 0;               // the hotkey's key while it is down (with or without its modifiers): the LL hook has no repeat flag, so key-downs before its key-up are auto-repeat
static Uint16 _keycode = 0;
static DWORD _keyVk = 0, _keyExtended = 0;   // the key as Windows sent it: re-sent as is after a restore (Home, numpad 1...)
static bool _hasJustUsedHotKey = false;
static std::function<void(int)> _observer;
static std::wstring _frontApp = L"UnknownApp";   // exe name of the app in front
static std::wstring _siteHost;                    // host of the browser tab in front (Phase 3)
static bool _otherInputLanguage = false;          // Japanese/Chinese IME layout active
static int _languageTemp = 0;
static std::vector<Byte> savedSmartSwitchKeyData;

// ---- Auto switch by typing (same state machine as VikeyCore.mm) --------------------------
static Uint32 _enKeys[MAX_BUFF];
static int _enCount = 0;
static bool _enValid = true;
static bool _enAfterShortcut = false;
static bool _enAfterDouble = false;
// ← then → back to the same spot inside a word: the English word is still the one being typed (D51)
static Uint32 _enNavKeys[MAX_BUFF]; static int _enNavCount = 0, _enNavPrevN = 0, _enNavOffset = 0;
static bool _enNavValid = true, _enNavAfterDouble = false;
static Uint32 _enPrevKeys[2][MAX_BUFF];
static int _enPrevCounts[2];
static int _enPrevN = 0;
static bool _enPrevRestored = false;   // _enPrevKeys[0] was put back to English by auto switch (D35)

struct AutoFixUndo {
    bool active; bool breakGone; int removeCount; wchar_t text[MAX_BUFF + 1]; int textCount; int language;   // breakGone: the first Backspace took the break key (D61)
    Uint32 keys[MAX_BUFF]; int keyCount;   // the keys of the original word (English mode can go on typing it after an undo, D32)
};
static AutoFixUndo _autoFixUndo = {};
// after an undo the restored word stays the current word; finishing it unchanged doesn't redo the fix (D32)
static Uint32 _undoneKeys[MAX_BUFF];
static int _undoneCount = 0;

static void StartEnglishWord(bool valid) { _enCount = 0; _enValid = valid; _enAfterShortcut = false; }
static void ResetEnglishWord(bool valid) { _enAfterDouble = false; _undoneCount = 0; StartEnglishWord(valid); _enPrevN = 0; _enNavOffset = 0; vAutoSwitchForget(); }   // also the D51 round trip: after a click or a switch → must not bring the old word back

// ---- Typing log ------------------------------------------------------------------------
int vTypingLogKeys = 1, vTypingLogEnabled = 0;
static std::string _tlPending;
static std::mutex _tlMutex;
static wchar_t _tlWord[64]; static int _tlWordLen = 0; static int _tlWordLang = 1; static bool _tlWordTruncated = false;
static bool _tlSinceAuto = false;
static std::wstring _tlApp;

static std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
static std::string jsonEscape(const std::string& s) {
    std::string o; for (unsigned char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += c; } return o;
}
std::wstring typingLogPath() { return prefs::appDataDir() + L"\\typing-log.jsonl"; }
static std::string TLTime() {
    SYSTEMTIME st; GetLocalTime(&st); TIME_ZONE_INFORMATION tz; DWORD r = GetTimeZoneInformation(&tz);
    long bias = -(tz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias));
    char b[64]; snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02ld:%02ld", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, bias < 0 ? '-' : '+', labs(bias) / 60, labs(bias) % 60);
    return b;
}
static std::string TLToday() { SYSTEMTIME st; GetLocalTime(&st); char b[16]; snprintf(b, sizeof b, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay); return b; }
static void TLCount(const char* name) {
    if (!vTypingLogEnabled) return;
    std::string day = TLToday();
    std::wstring key = L"TLStat_" + std::wstring(day.begin(), day.end()) + L"_" + std::wstring(name, name + strlen(name));
    prefs::setInt(key.c_str(), prefs::getInt(key.c_str(), 0) + 1);
}
void flushTypingLog() {
    std::string chunk; { std::lock_guard<std::mutex> g(_tlMutex); chunk.swap(_tlPending); }
    if (chunk.empty()) return;
    std::wstring path = typingLogPath();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && (fad.nFileSizeHigh || fad.nFileSizeLow > 20u * 1024 * 1024)) {
        std::wstring old = path.substr(0, path.size() - 6) + L".1.jsonl";
        DeleteFileW(old.c_str()); MoveFileW(path.c_str(), old.c_str());
    }
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, chunk.data(), (DWORD)chunk.size(), &w, nullptr); CloseHandle(h);
}
void clearTypingLog() {
    { std::lock_guard<std::mutex> g(_tlMutex); _tlPending.clear(); }
    std::wstring path = typingLogPath(); DeleteFileW(path.c_str()); DeleteFileW((path.substr(0, path.size() - 6) + L".1.jsonl").c_str());
}
static bool TLKeysAllowed() {
    static const wchar_t* excluded[] = { // password managers and terminals (sudo/ssh passwords are typed in plain view of the hook)
        L"1password.exe", L"bitwarden.exe", L"keepassxc.exe", L"keepass.exe", L"lastpass.exe", L"dashlane.exe", L"enpass.exe", L"nordpass.exe",
        L"windowsterminal.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"putty.exe", L"wezterm-gui.exe", L"alacritty.exe", L"conhost.exe", L"openconsole.exe", L"termius.exe" };
    std::wstring app = _tlApp; std::transform(app.begin(), app.end(), app.begin(), ::towlower);
    for (auto e : excluded) if (app == e) return false;
    if (site::focusedIsPassword()) return false;   // like secure input on macOS: never the keys of a password field
    return vTypingLogKeys != 0;
}
static void TLWrite(std::string fields) {
    if (!vTypingLogEnabled) return;
    std::string line = "{" + fields + ",\"t\":\"" + TLTime() + "\",\"app\":\"" + jsonEscape(utf8(_tlApp)) + "\"";
    if (!_siteHost.empty()) line += ",\"site\":\"" + jsonEscape(utf8(_siteHost)) + "\"";
    line += "}\n";
    bool flush;
    { std::lock_guard<std::mutex> g(_tlMutex); _tlPending += line; flush = _tlPending.size() > 8192; }
    if (flush) flushTypingLog();
}
static const char* TLLang(int lang) { return lang ? "vi" : "en"; }
static void TLEndWord(const std::string& reason) {
    if (_tlWordLen == 0) return;
    bool hasLetter = false; for (int x = 0; x < _tlWordLen; x++) if (_tlWord[x] != 0x232B) { hasLetter = true; break; }
    if (hasLetter) TLCount(_tlWordLang ? "words_vi" : "words_en");
    if (hasLetter && TLKeysAllowed()) {
        std::string f = std::string("\"e\":\"word\",\"lang\":\"") + TLLang(_tlWordLang) + "\",\"keys\":\"" + jsonEscape(utf8(std::wstring(_tlWord, _tlWordLen))) + "\",\"end\":\"" + jsonEscape(reason) + "\"";
        if (_tlWordTruncated) f += ",\"truncated\":true";
        TLWrite(f);
    }
    _tlWordLen = 0; _tlWordTruncated = false;
}
static void TLAppendChar(wchar_t c) { if (_tlWordLen == 0) _tlWordLang = vLanguage; if (_tlWordLen < 64) _tlWord[_tlWordLen++] = c; else _tlWordTruncated = true; }
static std::string TLKeyName(Uint16 key, bool shift) {
    switch (key) { case KEY_SPACE: return "space"; case KEY_RETURN: case KEY_ENTER: return "enter"; case KEY_TAB: return "tab"; case KEY_ESC: return "esc";
                   case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN: return "arrow"; }
    Uint16 ch = keyCodeToCharacter(key | (shift ? CAPS_MASK : 0));
    return ch ? utf8(std::wstring(1, (wchar_t)ch)) : "other";
}
static void TLNoteKey(Uint16 key, unsigned flags) {
    if (!vTypingLogEnabled) return;
    _tlSinceAuto = false;
    bool shift = flags & F_SHIFT;
    if (flags & (F_CONTROL | F_ALT | F_WIN)) {
        std::string combo; if (flags & F_CONTROL) combo += "Ctrl+"; if (flags & F_ALT) combo += "Alt+"; if (shift) combo += "Shift+"; if (flags & F_WIN) combo += "Win+";
        std::string k = key == KEY_DELETE ? "Backspace" : TLKeyName(key, false); std::transform(k.begin(), k.end(), k.begin(), ::toupper); combo += k;
        TLEndWord(combo); return;
    }
    bool caps = shift || (flags & F_CAPS);
    if (key == KEY_DELETE) { TLAppendChar(0x232B); return; }
    bool letter = false; for (Uint16 k : {KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z}) if (k == key) letter = true;
    if (letter || (IS_NUMBER_KEY(key) && !shift)) { Uint16 ch = keyCodeToCharacter(key | (caps ? CAPS_MASK : 0)); if (ch) TLAppendChar((wchar_t)ch); return; }
    TLEndWord(TLKeyName(key, shift));
}
static void TLNoteClick() { _tlSinceAuto = false; TLEndWord("click"); }
static void TLAuto(const char* rule, int to) { TLCount((std::string("auto_") + rule).c_str()); TLWrite(std::string("\"e\":\"auto\",\"rule\":\"") + rule + "\",\"to\":\"" + TLLang(to) + "\""); _tlSinceAuto = true; }
static void TLManual(const char* via, int to) {
    TLEndWord("switch"); bool afterAuto = _tlSinceAuto; _tlSinceAuto = false;
    TLCount("manual"); if (afterAuto) TLCount("manual_after_auto");
    TLWrite(std::string("\"e\":\"manual\",\"via\":\"") + via + "\",\"to\":\"" + TLLang(to) + "\"" + (afterAuto ? ",\"after_auto\":true" : ""));
}
static void TLUndo() { TLEndWord("undo"); _tlSinceAuto = false; TLCount("undo"); TLWrite(std::string("\"e\":\"undo\",\"via\":\"backspace\",\"to\":\"") + TLLang(_autoFixUndo.language) + "\""); }
static void TLContext(int to) { TLEndWord("context"); TLCount("context"); TLWrite(std::string("\"e\":\"context\",\"to\":\"") + TLLang(to) + "\""); }

// Translate mode (bottom of this file): what the earlier code tells it.
static void TrForget();              // the cursor moved away from the tracked text: start over
static void TrRecordBatch();         // the keys Vikey is about to send

// ---- Front app / smart switch -----------------------------------------------------------
std::wstring frontAppName() { return _frontApp; }

static void queryFrontMostApp() {
    HWND w = GetForegroundWindow(); DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return;   // our own windows: keep the app being typed in
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t path[MAX_PATH] = {0}; DWORD n = MAX_PATH;
    std::wstring name = L"UnknownApp";
    if (p) { if (QueryFullProcessImageNameW(p, 0, path, &n)) { std::wstring s(path); size_t i = s.find_last_of(L'\\'); name = i == std::wstring::npos ? s : s.substr(i + 1); } CloseHandle(p); }
    _frontApp = name;
}

static std::string SmartSwitchContextKey() {
    std::wstring k = _frontApp; if (!_siteHost.empty()) k += L"|" + _siteHost;
    return utf8(k);
}
static void saveSmartSwitchKeyData() {
    getSmartSwitchKeySaveData(savedSmartSwitchKeyData);
    prefs::setBinary(L"smartSwitchKey", savedSmartSwitchKeyData.data(), (DWORD)savedSmartSwitchKeyData.size());
}
static void VKLanguageChanged(bool rememberForContext) {
    if (rememberForContext && vUseSmartSwitchKey) { setAppInputMethodStatus(SmartSwitchContextKey(), vLanguage); saveSmartSwitchKeyData(); }
    prefs::setInt(L"InputMethod", vLanguage);
    if (_observer) _observer(vLanguage);
}
static void ApplySmartSwitch() {
    _languageTemp = getAppInputMethodStatus(SmartSwitchContextKey(), vLanguage);
    if ((_languageTemp & 0x01) != vLanguage) {
        if (_languageTemp != -1) { vLanguage = _languageTemp & 0x01; VKLanguageChanged(false); TLContext(vLanguage); startNewSession(); }
        else saveSmartSwitchKeyData();
    }
}
void onForegroundChanged() {
    queryFrontMostApp();
    _siteHost.clear();
    ResetEnglishWord(true); _autoFixUndo.active = false;
    TLEndWord("app"); _tlApp = _frontApp;
    TrForget();
    if (vUseSmartSwitchKey) ApplySmartSwitch();
    startNewSession();
}
void onSiteChanged(const std::wstring& host) {
    if (host == _siteHost) return;
    _siteHost = host;
    TrForget();
    ResetEnglishWord(true); _autoFixUndo.active = false;
    if (vUseSmartSwitchKey) ApplySmartSwitch();
    startNewSession();
}

// ---- Other input language (Japanese, Chinese...) ------------------------------------------
static void updateOtherInputLanguage() {
    HWND w = GetForegroundWindow(); DWORD tid = GetWindowThreadProcessId(w, nullptr);
    HKL hkl = GetKeyboardLayout(tid); LANGID lang = LOWORD((DWORD_PTR)hkl); WORD primary = PRIMARYLANGID(lang);
    _otherInputLanguage = !(primary == LANG_ENGLISH || primary == LANG_VIETNAMESE || primary == LANG_NEUTRAL);
}

// ---- Pause auto switch after a manual switch (D21) ------------------------------------------
static ULONGLONG _autoPauseUntil = 0;
static bool AutoSwitchPaused() { return GetTickCount64() < _autoPauseUntil; }
static void PauseAutoSwitchAfterManual() {
    if (prefs::has(L"PauseAutoAfterManual") && !prefs::getInt(L"PauseAutoAfterManual", 1)) return;
    _autoPauseUntil = GetTickCount64() + 5000;
    vAutoSwitchByTyping = 0;
}
static void checkPauseExpired() {
    if (_autoPauseUntil && !AutoSwitchPaused()) {
        _autoPauseUntil = 0;
        vAutoSwitchByTyping = prefs::getInt(L"AutoSwitchByTyping", 0);
        StartEnglishWord(false);
    }
}

void reloadSettings() {
    vLanguage = prefs::getInt(L"InputMethod", 1) ? 1 : 0;
    vCheckSpelling = prefs::getInt(L"Spelling", 1); vSetCheckSpelling();
    vRestoreIfWrongSpelling = prefs::getInt(L"RestoreIfInvalidWord", 1);
    vUseModernOrthography = prefs::getInt(L"ModernOrthography", 1);
    vTelexStyle = prefs::getInt(L"TelexStyle", 0);   // D56: Telex / Simple Telex 1 / 2
    vUseSmartSwitchKey = prefs::getInt(L"UseSmartSwitchKey", 1);
    vAutoSwitchByTyping = AutoSwitchPaused() ? 0 : prefs::getInt(L"AutoSwitchByTyping", 0);
    vTypingLogKeys = prefs::getInt(L"TypingLogKeys", 1);
    vTypingLogEnabled = 0;   // fork: the typing log records keystrokes to disk; never on
    int hotkey = prefs::getInt(L"SwitchKeyStatus", 0);
    vSwitchKeyStatus = hotkey ? hotkey : 0x7A000206;   // default Alt+Z (same bits as ⌥Z on macOS)
    vEmojiHotkey = prefs::getInt(L"EmojiPicker", 0) ? prefs::getInt(L"EmojiHotkey", 0) : 0;
    _trHotkey = prefs::getInt(L"TranslateHotkey", KEY_T | 0x200);                 // Alt+T
    _trToggleKey = prefs::getInt(L"TranslateToggleHotkey", KEY_T | 0x200 | 0x800); // Alt+Shift+T
    updateOtherInputLanguage();
    if (_observer) _observer(vLanguage);
}

// ---- Sending output ----------------------------------------------------------------------
static void inputKey(INPUT& in, WORD vk, bool down, DWORD extra = 0) {
    in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | extra; in.ki.dwExtraInfo = VIKEY_EVENT_TAG;
}
static void inputChar(INPUT& in, wchar_t c, bool down) {
    in = {}; in.type = INPUT_KEYBOARD; in.ki.wScan = c; in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP); in.ki.dwExtraInfo = VIKEY_EVENT_TAG;
}
static std::vector<INPUT> _batch;
static void batchBackspaces(int n) { for (int i = 0; i < n; i++) { INPUT a, b; inputKey(a, VK_BACK, true); inputKey(b, VK_BACK, false); _batch.push_back(a); _batch.push_back(b); } }
static void batchText(const wchar_t* text, int count) { for (int i = 0; i < count; i++) { INPUT a, b; inputChar(a, text[i], true); inputChar(b, text[i], false); _batch.push_back(a); _batch.push_back(b); } }
/// A key press and release. Modifiers aren't sent: the user is still holding them (an injected Shift up would
/// release a Shift that is physically down, and the next letters would come out lowercase).
static void batchKey(WORD vk, DWORD extra = 0) {
    INPUT c, d;
    inputKey(c, vk, true, extra); inputKey(d, vk, false, extra); _batch.push_back(c); _batch.push_back(d);
}
static void flushBatch() { if (!_batch.empty()) { TrRecordBatch(); SendInput((UINT)_batch.size(), _batch.data(), sizeof(INPUT)); _batch.clear(); } }

static bool IsLetterKey(Uint16 key);
/// Types the engine's new characters; for a restore, also the key that triggered it.
static void batchEngineChars() {
    wchar_t text[MAX_BUFF + 2]; int n = 0;
    for (int k = pData->newCharCount - 1; k >= 0 && n < MAX_BUFF; k--) {
        Uint32 c = pData->charData[k];
        text[n++] = (c & CHAR_CODE_MASK) ? (wchar_t)(c & 0xFFFF) : (wchar_t)keyCodeToCharacter(c);
    }
    bool sendControlKey = false;
    if (pData->code == vRestore || pData->code == vRestoreAndStartNewSession) {
        Uint16 ch = keyCodeToCharacter(_keycode | ((_flag & F_SHIFT) || ((_flag & F_CAPS) && IsLetterKey(_keycode)) ? CAPS_MASK : 0));   // Caps Lock shifts letters only ("NAS," gave "NAS<")
        if (ch != 0) text[n++] = (wchar_t)ch; else sendControlKey = true;
    }
    if (pData->code == vRestoreAndStartNewSession) startNewSession();
    if (n > 0) batchText(text, n);
    // the key itself, not the engine's code for it: Home, End and numpad keys all reach the engine as Esc
    if (sendControlKey) { WORD vk = _keyVk ? (WORD)_keyVk : vkOf(_keycode); if (vk) batchKey(vk, _keyExtended); }
}

/// Sends the engine's result for the current key: backspaces, then the new characters.
static void SendEngineOutput() {
    // defeats address-bar autocomplete; not when the extra backspace would reach MAX_BUFF (the engine can ask for 31): then none would be sent
    if (vFixRecommendBrowser && pData->extCode != 4 && pData->backspaceCount < MAX_BUFF - 1) { wchar_t e = 0x202F; batchText(&e, 1); pData->backspaceCount++; }
    if (pData->backspaceCount > 0 && pData->backspaceCount < MAX_BUFF) batchBackspaces(pData->backspaceCount);
    batchEngineChars();
    flushBatch();
}

// ---- Hotkey ------------------------------------------------------------------------------
/// flags: for a key hotkey (Alt+Z) the key press's own, so Alt held and Z again switches again; for modifiers
/// alone (Ctrl+Shift) the most modifiers held before the release
static bool checkHotKey(int hotKeyData, unsigned flags, bool checkKeyCode = true) {
    if ((hotKeyData & (~0x8000)) == EMPTY_HOTKEY) return false;
    if (HAS_CONTROL(hotKeyData) ^ GET_BOOL(flags & F_CONTROL)) return false;
    if (HAS_OPTION(hotKeyData) ^ GET_BOOL(flags & F_ALT)) return false;
    if (HAS_COMMAND(hotKeyData) ^ GET_BOOL(flags & F_WIN)) return false;
    if (HAS_SHIFT(hotKeyData) ^ GET_BOOL(flags & F_SHIFT)) return false;
    if (checkKeyCode && GET_SWITCH_KEY(hotKeyData) != _keycode) return false;
    return true;
}
// a key or click with modifiers held: releasing them afterwards isn't the modifier-only hotkey
static void cancelModifierHotkey() { _lastFlag = 0; if (_flag & (F_SHIFT | F_CONTROL | F_ALT | F_WIN)) _modifierChordUsed = true; }
static void switchLanguage(const char* via) {
    PauseAutoSwitchAfterManual();
    _autoFixUndo.active = false;
    ResetEnglishWord(true);
    vLanguage = vLanguage == 0 ? 1 : 0;
    TLManual(via, vLanguage);
    if (HAS_BEEP(vSwitchKeyStatus)) PlaySwitchSound(vLanguage);
    VKLanguageChanged(true);
    startNewSession();
}
void setLanguageByUser(int lang) {
    if (vLanguage == lang) return;
    PauseAutoSwitchAfterManual(); _autoFixUndo.active = false; ResetEnglishWord(true);
    vLanguage = lang; TLManual("menu", vLanguage); VKLanguageChanged(true); startNewSession();
}
void setLanguageQuiet(int lang) {
    _autoFixUndo.active = false; ResetEnglishWord(true); _autoPauseUntil = 0; vAutoSwitchByTyping = prefs::getInt(L"AutoSwitchByTyping", 0);
    vLanguage = lang ? 1 : 0; VKLanguageChanged(false); startNewSession();
}
int language() { return vLanguage; }
void setLanguageObserver(std::function<void(int)> f) { _observer = f; }

// ---- Emoji picker (Settings.cpp) ------------------------------------------------------------
static std::function<void()> _emojiHandler;
void setEmojiHotkeyHandler(std::function<void()> f) { _emojiHandler = f; }
void setEmojiPickerOpen(bool open, HWND picker) { vEmojiWindow = picker; vEmojiOpen = open; }
/// Windows' own shortcuts while the picker is open: Win+D, Alt+Tab, Alt+F4, Ctrl+Esc (Alt+Backspace deletes a search
/// word, Ctrl+Alt is AltGr). The picker keeps every other key, like the Mac's panel.
static bool windowsShortcut(DWORD vk, unsigned flag) {
    if (flag & F_WIN) return true;
    if (flag & F_ALT) return !(flag & F_CONTROL) && vk != VK_BACK;
    return vk == VK_ESCAPE && (flag & F_CONTROL);
}
/// Volume, media, browser and launch keys, PrintScreen: they go on and the picker stays open.
static bool deviceKey(DWORD vk) { return vk == VK_SNAPSHOT || (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2); }
void resetTyping() { ResetEnglishWord(true); _autoFixUndo.active = false; startNewSession(); }
void insertText(const std::wstring& text) {
    resetTyping();
    batchText(text.c_str(), (int)text.size());   // KEYEVENTF_UNICODE per UTF-16 unit: surrogate pairs arrive as one character
    flushBatch();
}

// ---- Auto switch helpers (same as VikeyCore.mm) ---------------------------------------------
static bool IsWhitespaceKey(Uint16 key) { return key == KEY_SPACE || key == KEY_RETURN || key == KEY_ENTER || key == KEY_TAB; }
static bool IsWordStartKey(Uint16 key, bool shift) { return IsWhitespaceKey(key) || (key == KEY_9 && shift) || key == KEY_LEFT_BRACKET || key == KEY_QUOTE; }
static bool IsLetterKey(Uint16 key) { for (Uint16 k : {KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z}) if (k == key) return true; return false; }
static wchar_t CharacterOf(Uint32 data) { return (data & CHAR_CODE_MASK) ? (wchar_t)(data & 0xFFFF) : (wchar_t)keyCodeToCharacter(data); }

static void RememberAutoFix(int correctedCount) {
    _autoFixUndo.active = false;
    if (vAutoSwitchUndoCount <= 0 || correctedCount <= 0) return;
    for (int x = 0; x < vAutoSwitchUndoCount; x++) if ((vAutoSwitchUndoData[x] & 0xFFFF) == KEY_SPACE && !(vAutoSwitchUndoData[x] & CHAR_CODE_MASK)) return;
    Uint16 breakChar = keyCodeToCharacter(_keycode | ((_flag & F_SHIFT) || ((_flag & F_CAPS) && IsLetterKey(_keycode)) ? CAPS_MASK : 0));   // Caps Lock shifts letters only (Space + CAPS_MASK is 0)
    if (breakChar == 0) return;
    int n = 0;
    for (int x = 0; x < vAutoSwitchUndoCount && n < MAX_BUFF; x++) { wchar_t c = CharacterOf(vAutoSwitchUndoData[x]); if (c == 0) return; _autoFixUndo.text[n++] = c; }
    _autoFixUndo.textCount = n;
    _autoFixUndo.keyCount = 0;
    for (int x = 0; x < vAutoSwitchUndoCount && x < MAX_BUFF; x++) {
        if (vAutoSwitchUndoData[x] & CHAR_CODE_MASK) { _autoFixUndo.keyCount = 0; break; }   // shown text, not keys
        _autoFixUndo.keys[_autoFixUndo.keyCount++] = vAutoSwitchUndoData[x];
    }
    _autoFixUndo.removeCount = correctedCount + 1; _autoFixUndo.language = vLanguage; _autoFixUndo.breakGone = false; _autoFixUndo.active = true;
}
static bool UndoAutoFixWithBackspace() {
    if (!_autoFixUndo.active) return false;
    _autoFixUndo.active = false;
    int removeCount = _autoFixUndo.removeCount;
    if (vFixRecommendBrowser) { wchar_t e = 0x202F; batchText(&e, 1); removeCount++; }
    batchBackspaces(removeCount);
    batchText(_autoFixUndo.text, _autoFixUndo.textCount);
    flushBatch();
    TLUndo();
    if (vLanguage != _autoFixUndo.language) { vLanguage = _autoFixUndo.language; VKLanguageChanged(true); }
    startNewSession();
    ResetEnglishWord(false);   // the cursor is at the end of a word we didn't see typed
    // back in English with the typed keys on screen: they are the current word again, so typing on
    // ("vaay" + "j") is checked as a whole ("vaayj" -> "vậy"); the same word again stays as typed (D32)
    if (vLanguage == 0 && _autoFixUndo.keyCount > 0) {
        memcpy(_enKeys, _autoFixUndo.keys, _autoFixUndo.keyCount * sizeof(Uint32));
        _enCount = _autoFixUndo.keyCount;
        _enValid = true;
        memcpy(_undoneKeys, _autoFixUndo.keys, _autoFixUndo.keyCount * sizeof(Uint32));
        _undoneCount = _autoFixUndo.keyCount;
    }
    return true;
}
static void AutoSwitchTo(int language, const char* rule) {
    if (vLanguage == language) return;
    TLAuto(rule, language); vLanguage = language; VKLanguageChanged(true); startNewSession(); vAutoSwitchForget();
}
static void ApplyAutoSwitchToEnglish(int correctedCount, bool atWordBreak) {
    vAutoSwitchRequest = -1;
    if (atWordBreak) RememberAutoFix(correctedCount); else _autoFixUndo.active = false;
    AutoSwitchTo(0, atWordBreak ? "vi_word_to_en" : "double_key");
    ResetEnglishWord(atWordBreak && IsWhitespaceKey(_keycode));   // mid-word: the rest of this word is not a whole word
    // D35: the word just put back to English is the previous word for look-back ("em is" + "ạ" -> "em í ạ")
    if (atWordBreak && _keycode == KEY_SPACE && vAutoSwitchRestoredCount > 0) {
        memcpy(_enPrevKeys[0], vAutoSwitchRestoredKeys, vAutoSwitchRestoredCount * sizeof(Uint32));
        _enPrevCounts[0] = vAutoSwitchRestoredCount;
        _enPrevN = 1;
        _enPrevRestored = true;
    }
    _enAfterDouble = !atWordBreak;   // at its end, "mixx" may become "mix"
}
static bool HandleEnglishKeyForAutoSwitch() {
    if (OTHER_CONTROL_KEY) { ResetEnglishWord(true); _enAfterShortcut = (_flag & (F_CONTROL | F_WIN)) != 0; return false; }
    bool shift = _flag & F_SHIFT; bool caps = shift || (_flag & F_CAPS);
    if ((_keycode == KEY_LEFT || _keycode == KEY_RIGHT) && !shift) {   // D51: ← → round trip inside a word
        if (_keycode == KEY_LEFT && _enNavOffset == 0 && (_enCount > 0 || _enPrevN > 0)) {
            memcpy(_enNavKeys, _enKeys, _enCount * sizeof(Uint32)); _enNavCount = _enCount; _enNavValid = _enValid;
            _enNavAfterDouble = _enAfterDouble; _enNavPrevN = _enPrevN; _enNavOffset = -1;
            StartEnglishWord(false); _enAfterDouble = false; _enPrevN = 0;
            return false;
        }
        if (_enNavOffset < 0) {
            if (_keycode == KEY_LEFT) _enNavOffset--;
            else if (++_enNavOffset == 0) {   // back at the word's end: it's that word again
                memcpy(_enKeys, _enNavKeys, _enNavCount * sizeof(Uint32)); _enCount = _enNavCount; _enValid = _enNavValid;
                _enAfterDouble = _enNavAfterDouble; _enPrevN = _enNavPrevN;
            }
            return false;
        }
    }
    _enNavOffset = 0;
    if (IsLetterKey(_keycode)) { if (_enCount < MAX_BUFF - 2) _enKeys[_enCount++] = _keycode | (caps ? CAPS_MASK : 0); else _enValid = false; _enAfterShortcut = false; return false; }
    if (_keycode == KEY_DELETE) {
        if (_enCount == 0) { _enPrevN = 0; _enAfterDouble = false; }
        if (_enCount > 0) _enCount--; else if (!_enAfterShortcut) _enValid = false;
        return false;
    }
    if (IS_NUMBER_KEY(_keycode) && !shift) {   // digits after a space start a word of their own ("5trawm" -> "5trăm", D33); glued to letters, a code
        _enValid = _enCount == 0 && _enValid;
        _enCount = 0;
        _enPrevN = 0;
        _enAfterDouble = false;
        return false;
    }
    bool wordEnd = _keycode == KEY_SPACE || _keycode == KEY_COMMA || _keycode == KEY_DOT || _keycode == KEY_SEMICOLON || _keycode == KEY_RETURN || _keycode == KEY_ENTER ||
                   (shift && (_keycode == KEY_SLASH || _keycode == KEY_1 || _keycode == KEY_QUOTE || _keycode == KEY_0));
    // Enter sends what's on screen (D62): it ends the word but never fixes it; a word to fix gets a space or . first
    bool fixAt = wordEnd && _keycode != KEY_RETURN && _keycode != KEY_ENTER;
    int count = _enCount; bool valid = _enValid; bool afterDouble = _enAfterDouble; _enAfterDouble = false;
    if (afterDouble && fixAt && vAutoSwitchFixDoubledKey(_enKeys, count)) {
        StartEnglishWord(IsWordStartKey(_keycode, shift)); _enPrevN = 0;
        int correctedCount = pData->newCharCount; SendEngineOutput(); RememberAutoFix(correctedCount); return true;
    }
    StartEnglishWord(IsWordStartKey(_keycode, shift));
    bool checked = fixAt && valid && count >= 2 && !(vOtherLanguage && _otherInputLanguage);
    if (_undoneCount > 0) {   // the word just put back by Backspace, finished unchanged: keep it as typed (D32)
        if (count == _undoneCount && !memcmp(_enKeys, _undoneKeys, count * sizeof(Uint32))) checked = false;
        if (wordEnd) _undoneCount = 0;
    }
    const Uint32* previous[2] = {_enPrevKeys[0], _enPrevKeys[1]};
    vAutoSwitchPrevRestored = _enPrevRestored && _enPrevN >= 1;
    bool converted = checked && vAutoSwitchCheckEnglishWord(_enKeys, count, previous, _enPrevCounts, _enPrevN);
    // remember this word for look-back only if it was followed by a single space
    if (!converted && _keycode == KEY_SPACE && valid && count >= 1) {
        _enPrevRestored = false;
        memcpy(_enPrevKeys[1], _enPrevKeys[0], sizeof(_enPrevKeys[0])); _enPrevCounts[1] = _enPrevCounts[0];
        memcpy(_enPrevKeys[0], _enKeys, count * sizeof(Uint32)); _enPrevCounts[0] = count; if (_enPrevN < 2) _enPrevN++;
    } else _enPrevN = 0;
    if (!converted) return false;
    int correctedCount = pData->newCharCount; SendEngineOutput(); RememberAutoFix(correctedCount); AutoSwitchTo(1, "en_word_to_vi");
    return true;
}

// ---- Hooks --------------------------------------------------------------------------------
static bool modifierVk(DWORD vk, bool down) {
    unsigned bit = 0;
    switch (vk) { case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: bit = F_SHIFT; break; case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: bit = F_CONTROL; break;
                  case VK_LMENU: case VK_RMENU: case VK_MENU: bit = F_ALT; break; case VK_LWIN: case VK_RWIN: bit = F_WIN; break; default: return false; }
    if (down) _flag |= bit; else _flag &= ~bit;
    return true;
}

/// The original Vikey hook. The keyboardHook() below wraps it for translate mode.
static bool _passed = false;   // the inner hook let the key through to the app unchanged
static LRESULT keyboardHookInner(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION) return (_passed = true, CallNextHookEx(_kbHook, nCode, wParam, lParam));
    KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
    if (k->dwExtraInfo == VIKEY_EVENT_TAG) return (_passed = true, CallNextHookEx(_kbHook, nCode, wParam, lParam));   // our own output
    bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    // modifiers as Windows has them, not only from the events seen here: keys let go on the lock screen (Win+L) or
    // after Ctrl+Alt+Del never reach the hook, and a Win or Alt left set stops Telex until that key is tapped again
    _flag = (GetKeyState(VK_CAPITAL) & 1 ? F_CAPS : 0) | (GetAsyncKeyState(VK_SHIFT) < 0 ? F_SHIFT : 0) | (GetAsyncKeyState(VK_CONTROL) < 0 ? F_CONTROL : 0) |
            (GetAsyncKeyState(VK_MENU) < 0 ? F_ALT : 0) | (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0 ? F_WIN : 0);
    checkPauseExpired();

    // modifier keys: track them, and detect a modifiers-only hotkey on release
    if (modifierVk(k->vkCode, down)) {
        if (_modifierChordUsed) {   // Ctrl+Alt+Shift-click, then letting go of Alt and Shift one by one, must not switch
            _lastFlag = 0;
            if (!(_flag & (F_SHIFT | F_CONTROL | F_ALT | F_WIN))) _modifierChordUsed = false;
        } else if (_lastFlag == 0 || _lastFlag < _flag) _lastFlag = _flag;
        else if (_lastFlag > _flag) {
            // only a hotkey without a key (a key hotkey fired on its key-down; _keycode is just the last key typed, so
            // Alt+Z, then Alt alone, flipped back). The key-up goes on: a swallowed one leaves the modifier held down.
            bool hit = GET_SWITCH_KEY(vSwitchKeyStatus) == 0xFE && checkHotKey(vSwitchKeyStatus, _lastFlag, false);
            _lastFlag = 0; _hasJustUsedHotKey = hit;
            if (hit) switchLanguage("hotkey");
        }
        return (_passed = true, CallNextHookEx(_kbHook, nCode, wParam, lParam));
    }
    if (!down) { if (k->vkCode == _hotkeyVk) _hotkeyVk = 0; return (_passed = true, CallNextHookEx(_kbHook, nCode, wParam, lParam)); }

    _keycode = engineKey(k->vkCode);
    _keyVk = k->vkCode; _keyExtended = (k->flags & LLKHF_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0;
    // emoji picker hotkey (opens and closes it): a key with exactly these modifiers
    if (vEmojiHotkey && GET_SWITCH_KEY(vEmojiHotkey) == _keycode &&
        HAS_CONTROL(vEmojiHotkey) == GET_BOOL(_flag & F_CONTROL) && HAS_OPTION(vEmojiHotkey) == GET_BOOL(_flag & F_ALT) &&
        HAS_COMMAND(vEmojiHotkey) == GET_BOOL(_flag & F_WIN) && HAS_SHIFT(vEmojiHotkey) == GET_BOOL(_flag & F_SHIFT)) {
        cancelModifierHotkey();   // Ctrl+Shift+E: releasing Ctrl+Shift afterwards isn't the Ctrl+Shift hotkey
        if (_emojiHandler) _emojiHandler();
        return 1;
    }
    // while open, the picker reads the keys it uses for its search (it does its own Telex reading); the app in front keeps
    // its cursor. Characters sent by other tools (VK_PACKET) go to it as characters.
    if (vEmojiOpen && vEmojiWindow) {
        if (k->vkCode == VK_PACKET) { PostMessageW(vEmojiWindow, WM_APP + 12, k->scanCode, 0); return 1; }
        if (windowsShortcut(k->vkCode, _flag)) { vEmojiOpen = false; PostMessageW(vEmojiWindow, WM_APP + 11, 0, 0); }   // closes it (closed now: the next key goes to the app) and goes on as usual
        else if (!deviceKey(k->vkCode)) { PostMessageW(vEmojiWindow, WM_APP + 10, k->vkCode, (LPARAM)_flag); return 1; }
    }
    if (_keycode == KEY_EMPTY) {   // F-keys, media keys...: not ours. A key typing a character still ends the word.
        cancelModifierHotkey();   // Ctrl+Shift+F5, Ctrl+Shift+numpad +: releasing Ctrl+Shift afterwards isn't the hotkey
        if (unmodeledKeyEndsWord(k->vkCode)) { TLEndWord("other"); resetTyping(); }
        return (_passed = true, CallNextHookEx(_kbHook, nCode, wParam, lParam));
    }

    // a password field gets the keys exactly as typed, in both languages: no switch hotkey (Shift+Z types Z, as under the
    // Mac's secure input and Linux's password purpose), no Telex, no auto switch, no Backspace undo (also right after a
    // focus change, until the field has been checked)
    if (site::focusedIsPassword()) { cancelModifierHotkey(); _hotkeyVk = 0; resetTyping(); return (_passed = true, CallNextHookEx(_kbHook, nCode, wParam, lParam)); }

    // switch language hotkey
    if (GET_SWITCH_KEY(vSwitchKeyStatus) != _keycode) { cancelModifierHotkey(); _hotkeyVk = 0; }
    else {
        bool repeat = k->vkCode == _hotkeyVk; _hotkeyVk = k->vkCode;   // any press, matched or not: Alt+Shift+Z, then Shift up, mustn't switch on a repeat (the Mac's autorepeat flag)
        if (checkHotKey(vSwitchKeyStatus, _flag, GET_SWITCH_KEY(vSwitchKeyStatus) != 0xFE)) {
            if (repeat) return 1;   // holding Alt+Z: switch once per press, swallow the auto-repeat
            switchLanguage("hotkey"); _lastFlag = 0; _hasJustUsedHotKey = true; return 1;
        }
    }
    _hasJustUsedHotKey = _lastFlag != 0;

    // Backspace right after an automatic correction (D61): the first takes only the break key and keeps the
    // correction ("được " ⌫ "," -> "được,"); a second one right after undoes it. Any other key ends that chance
    bool keepUndo = false;
    if (_keycode == KEY_DELETE && _autoFixUndo.active && !(OTHER_CONTROL_KEY)) {
        if (!_autoFixUndo.breakGone) { _autoFixUndo.breakGone = true; _autoFixUndo.removeCount--; keepUndo = true; }
        else if (UndoAutoFixWithBackspace()) return 1;
    }
    _autoFixUndo.active = keepUndo;
    TLNoteKey(_keycode, _flag);

    updateOtherInputLanguage();   // every key: Win+Space can switch to a Japanese/Chinese IME in the same window
    if (vLanguage == 0) {
        if (vAutoSwitchByTyping && HandleEnglishKeyForAutoSwitch()) return 1;
        return (_passed = true, CallNextHookEx(_kbHook, nCode, wParam, lParam));
    }
    if (vOtherLanguage && _otherInputLanguage) return (_passed = true, CallNextHookEx(_kbHook, nCode, wParam, lParam));

    vAutoSwitchRequest = -1;
    vKeyHandleEvent(vKeyEvent::Keyboard, vKeyEventState::KeyDown, _keycode,
                    (_flag & F_SHIFT) ? 1 : ((_flag & F_CAPS) ? 2 : 0), OTHER_CONTROL_KEY);
    if (pData->code == vDoNothing) return (_passed = true, CallNextHookEx(_kbHook, nCode, wParam, lParam));
    if (pData->code == vWillProcess || pData->code == vRestore || pData->code == vRestoreAndStartNewSession) {
        bool autoSwitchAtBreak = vAutoSwitchRequest == 0 && !IsLetterKey(_keycode);
        int correctedCount = pData->newCharCount;
        SendEngineOutput();
        if (vAutoSwitchRequest == 0) ApplyAutoSwitchToEnglish(correctedCount, autoSwitchAtBreak);
    }
    return 1;   // consumed
}

static LRESULT CALLBACK mouseHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN)) {
        if (vEmojiOpen && vEmojiWindow) {   // a click outside the picker closes it (the picker never takes the focus)
            RECT r; GetWindowRect(vEmojiWindow, &r); POINT p = ((MSLLHOOKSTRUCT*)lParam)->pt;
            if (!PtInRect(&r, p)) PostMessageW(vEmojiWindow, WM_APP + 11, 0, 0);
        }
        cancelModifierHotkey();   // Ctrl+Shift-click: the modifiers were for the click, so releasing them isn't the Ctrl+Shift hotkey
        _hotkeyVk = 0;   // a missed key-up (focus change, lock screen) must not swallow the next Alt+Z
        _autoFixUndo.active = false; TLNoteClick();
        TrForget();
        if (vLanguage == 0) ResetEnglishWord(true);
        else vKeyHandleEvent(vKeyEvent::Mouse, vKeyEventState::MouseDown, 0);
    }
    return CallNextHookEx(_mouseHook, nCode, wParam, lParam);
}

static VOID CALLBACK foregroundHook(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) { _hotkeyVk = 0; updateOtherInputLanguage(); onForegroundChanged(); }

// ---- Translate mode ----------------------------------------------------------------------------
// Off at every launch, and nothing of it runs while off. On: the text typed since the cursor last jumped is kept in
// tr::buf (RAM only, wiped on every reset). The translate key sends it to translate::request() and types the answer
// over it; Backspace right after puts the original back; the key again asks for another wording. Keys typed while a
// translation is on its way are swallowed. Nothing is ever sent without the translate key.
namespace tr {
enum State { Idle, Reading, Pending, Replaced };
static bool enabled = false;
static State state = Idle;
static std::wstring buf;                 // the tracked text, as it is on screen, ending at the cursor
static bool incomplete = false;          // something was typed or pasted that buf couldn't follow: use the whole field
static bool suppress = false;            // our own replacement is being sent: not recorded key by key
static HWND owner = nullptr;
static unsigned reqId = 0;
static HWND reqWindow = nullptr, reqFocus = nullptr;
static std::wstring source, shown, onScreen;   // the text translated, the translation typed, the text a result replaces
static bool wholeField = false;          // source is the whole field (replaced with Ctrl+A) rather than buf
static int variant = 0;
static ULONGLONG lastUse = 0, pendingSince = 0;
static DWORD hotkeyVk = 0;               // the translate or toggle key while it is held (the LL hook has no repeat flag)
static std::function<void(int, const std::wstring&)> observer;
static std::mutex fieldMutex; static std::wstring field; static int fieldRc = 1; static unsigned fieldId = 0;
}

static void TrWipe(std::wstring& w) { if (!w.empty()) SecureZeroMemory(&w[0], w.size() * sizeof(wchar_t)); w.clear(); }
/// The cursor moved (arrow keys, Enter sent the message...): tracking starts over from here, complete again.
static void TrJump() { TrWipe(tr::buf); tr::incomplete = false; }
/// Text changed in a way buf can't follow (paste, an IME, an emoji sequence...): the translate key reads the whole field.
static void TrLost() { TrWipe(tr::buf); tr::incomplete = true; }
static void TrNotify(int event, const std::wstring& message) { if (tr::observer) tr::observer(event, message); }

/// Apps whose keys never go into the buffer, translate mode on or off: password managers and terminals.
static bool TrSensitiveApp() {
    static const wchar_t* excluded[] = {
        L"1password.exe", L"bitwarden.exe", L"keepassxc.exe", L"keepass.exe", L"lastpass.exe", L"dashlane.exe", L"enpass.exe", L"nordpass.exe",
        L"windowsterminal.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"putty.exe", L"wezterm-gui.exe", L"alacritty.exe", L"conhost.exe", L"openconsole.exe", L"termius.exe",
        L"mstsc.exe", L"credentialuibroker.exe", L"consent.exe" };
    std::wstring app = _frontApp; std::transform(app.begin(), app.end(), app.begin(), ::towlower);
    for (auto e : excluded) if (app == e) return true;
    return false;
}
static bool TrRecording() {
    return tr::enabled && !tr::suppress && (tr::state == tr::Idle || tr::state == tr::Replaced) && !TrSensitiveApp() && !site::focusedIsPassword();
}

/// The text being tracked is no longer next to the cursor (click, arrow keys, another app...): start over from here.
/// A translation on its way is dropped: the place it was meant for may be gone.
static void TrForget() {
    TrJump();
    if (tr::state == tr::Pending || tr::state == tr::Reading) { tr::reqId++; TrNotify(1, L"Đã huỷ bản dịch đang chờ"); }
    if (tr::state != tr::Idle) { tr::state = tr::Idle; TrWipe(tr::source); TrWipe(tr::shown); TrWipe(tr::onScreen); TrNotify(0, L""); }
}

/// One UTF-16 unit typed. Emoji sequences (ZWJ, skin tones, flags, variation selectors) and combining marks delete in
/// ways a count of characters can't follow, so the tracking stops there; the translate key then reads the whole field.
static void TrPushUnit(wchar_t u) {
    if (tr::buf.size() >= 4000) { TrLost(); return; }
    if (u == 0x200D || u == 0xFE0E || u == 0xFE0F || u == 0x20E3 || (u >= 0x0300 && u <= 0x036F)) { TrLost(); return; }
    tr::buf += u;
    size_t n = tr::buf.size();
    if (n >= 2 && IS_LOW_SURROGATE(u) && IS_HIGH_SURROGATE(tr::buf[n - 2])) {
        unsigned cp = 0x10000 + (((unsigned)tr::buf[n - 2] - 0xD800) << 10) + ((unsigned)u - 0xDC00);
        if ((cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0x1F1E6 && cp <= 0x1F1FF) || (cp >= 0xE0020 && cp <= 0xE007F)) TrLost();
    }
}
static void TrPop() {
    size_t n = tr::buf.size();
    if (n == 0) return;   // deleting text from before the tracked part: what is tracked is still right before the cursor
    if (n >= 2 && IS_LOW_SURROGATE(tr::buf[n - 1]) && IS_HIGH_SURROGATE(tr::buf[n - 2])) tr::buf.resize(n - 2);
    else tr::buf.resize(n - 1);
}

/// Called by flushBatch(): the characters and Backspaces Vikey is about to send are what ends up on screen.
static void TrRecordBatch() {
    if (!tr::enabled || tr::suppress) return;
    if (!TrRecording()) { TrLost(); return; }
    for (const INPUT& in : _batch) {
        if (in.type != INPUT_KEYBOARD || (in.ki.dwFlags & KEYEVENTF_KEYUP)) continue;
        if (in.ki.dwFlags & KEYEVENTF_UNICODE) TrPushUnit((wchar_t)in.ki.wScan);
        else if (in.ki.wVk == VK_BACK) TrPop();
        else if (in.ki.wVk == VK_RETURN) { if (GetAsyncKeyState(VK_SHIFT) < 0) TrPushUnit(L'\n'); else TrJump(); }   // the key after a restore
        else if (in.ki.wVk == VK_TAB) TrJump();
        else { TrLost(); return; }   // Home, a numpad key...: the cursor may have moved
    }
}

/// A key the inner hook let through to the app unchanged.
static void TrNotePassedKey(DWORD vk) {
    if (!tr::enabled || tr::suppress) return;
    if (!TrRecording()) { TrLost(); return; }   // a password field being checked, a terminal...: not followed
    if (_flag & (F_CONTROL | F_ALT | F_WIN)) { TrLost(); return; }   // paste, undo, select all, word delete...
    switch (vk) {
    case VK_BACK: TrPop(); return;
    case VK_RETURN: if (_flag & F_SHIFT) TrPushUnit(L'\n'); else TrJump(); return;   // Enter sent the message: the field is empty
    case VK_TAB: case VK_ESCAPE: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: TrJump(); return;
    case VK_DELETE: case VK_INSERT: case VK_PACKET: TrLost(); return;
    }
    if (vOtherLanguage && _otherInputLanguage) { TrLost(); return; }   // a Japanese/Chinese IME composes on its own
    if (_keycode == KEY_EMPTY) { if (unmodeledKeyEndsWord(vk)) TrLost(); return; }
    Uint16 ch = keyCodeToCharacter(_keycode | ((_flag & F_SHIFT) || ((_flag & F_CAPS) && IsLetterKey(_keycode)) ? CAPS_MASK : 0));
    if (ch) TrPushUnit((wchar_t)ch); else TrLost();
}

static unsigned TrFlags() {
    return (GetKeyState(VK_CAPITAL) & 1 ? F_CAPS : 0) | (GetAsyncKeyState(VK_SHIFT) < 0 ? F_SHIFT : 0) | (GetAsyncKeyState(VK_CONTROL) < 0 ? F_CONTROL : 0) |
           (GetAsyncKeyState(VK_MENU) < 0 ? F_ALT : 0) | (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0 ? F_WIN : 0);
}
/// Ctrl, Alt or Win held: our characters would be shortcuts. Shift doesn't matter to typed Unicode characters.
static bool TrModifiersDown() { return (TrFlags() & (F_CONTROL | F_ALT | F_WIN)) != 0; }
static bool TrHotkeyIs(int hk, Uint16 key, unsigned f) {
    if (!hk || key == KEY_EMPTY || GET_SWITCH_KEY(hk) != key) return false;
    return HAS_CONTROL(hk) == GET_BOOL(f & F_CONTROL) && HAS_OPTION(hk) == GET_BOOL(f & F_ALT) &&
           HAS_COMMAND(hk) == GET_BOOL(f & F_WIN) && HAS_SHIFT(hk) == GET_BOOL(f & F_SHIFT);
}
static bool TrIsModifierVk(DWORD vk) {
    switch (vk) { case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: case VK_MENU: case VK_LMENU:
                  case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL: return true; }
    return false;
}
/// A swallowed Alt+key or Win+key: releasing Alt would open the app's menu bar, releasing Win the Start menu, as if Alt or
/// Win had been tapped alone. An unassigned key in between (0xE8, as AutoHotkey does) makes it a chord again.
static void TrMaskRelease(unsigned f) {
    if (!(f & (F_ALT | F_WIN))) return;
    INPUT in[2]; inputKey(in[0], 0xE8, true); inputKey(in[1], 0xE8, false); SendInput(2, in, sizeof(INPUT));
}
static HWND TrFocusNow() {
    GUITHREADINFO gi = {}; gi.cbSize = sizeof(gi);
    HWND fg = GetForegroundWindow();
    return GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gi) ? gi.hwndFocus : nullptr;
}

// Our output waits until the user lets go of Alt, Ctrl, Shift and Win: typed with Alt still down, the characters would
// be Alt+letter shortcuts in the app.
static std::function<void()> _trWaiting; static UINT_PTR _trTimer = 0; static int _trWaitTicks = 0;
static void CALLBACK TrWaitTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (TrModifiersDown() && ++_trWaitTicks < 100) return;   // up to 3 s
    KillTimer(nullptr, _trTimer); _trTimer = 0;
    auto f = _trWaiting; _trWaiting = nullptr;
    if (_trWaitTicks >= 100) { TrForget(); return; }
    if (f) f();
}
static void TrWhenKeysUp(std::function<void()> f) {
    if (!TrModifiersDown()) { f(); return; }
    _trWaiting = f; _trWaitTicks = 0;
    if (!_trTimer) _trTimer = SetTimer(nullptr, 0, 30, TrWaitTimer);
}

/// What Backspace has to remove for this text: one per character (a surrogate pair is one).
static int TrBackspaces(const std::wstring& t) {
    int n = 0;
    for (size_t i = 0; i < t.size(); i++) { if (IS_HIGH_SURROGATE(t[i]) && i + 1 < t.size() && IS_LOW_SURROGATE(t[i + 1])) i++; n++; }
    return n;
}
/// Types the text; a line break is Shift+Enter (a plain Enter sends the message in a chat app), or a space when the
/// original had none.
static void TrBatchType(const std::wstring& t, bool keepLines) {
    for (wchar_t c : t) {
        if (c == L'\n') {
            if (!keepLines) { wchar_t sp = L' '; batchText(&sp, 1); continue; }
            INPUT a, b, c2, d; inputKey(a, VK_SHIFT, true); inputKey(b, VK_RETURN, true); inputKey(c2, VK_RETURN, false); inputKey(d, VK_SHIFT, false);
            _batch.push_back(a); _batch.push_back(b); _batch.push_back(c2); _batch.push_back(d);
        } else batchText(&c, 1);
    }
}
static std::wstring TrTypedForm(const std::wstring& t, bool keepLines) {
    if (keepLines) return t;
    std::wstring o = t; for (auto& c : o) if (c == L'\n') c = L' '; return o;
}
/// Puts newText where oldText is: Backspaces over oldText, or Ctrl+A over the whole field.
static void TrReplace(const std::wstring& oldText, const std::wstring& newText, bool whole, bool keepLines) {
    resetTyping(); _autoFixUndo.active = false;
    tr::suppress = true;
    if (whole) {
        INPUT a, b, c, d; inputKey(a, VK_CONTROL, true); inputKey(b, 'A', true); inputKey(c, 'A', false); inputKey(d, VK_CONTROL, false);
        _batch.push_back(a); _batch.push_back(b); _batch.push_back(c); _batch.push_back(d);
    } else batchBackspaces(TrBackspaces(oldText));
    TrBatchType(newText, keepLines);
    flushBatch();
    tr::suppress = false;
    tr::buf = TrTypedForm(newText, keepLines);   // what is right before the cursor now
    tr::incomplete = false;
}

static std::wstring TrPref(const wchar_t* key, const wchar_t* fallback) { return prefs::getString(key, fallback); }

static void TrSend() {
    translate::Request q;
    q.text = tr::source; q.source = TrPref(L"TranslateSource", L"vi"); q.target = TrPref(L"TranslateTarget", L"en");
    q.variant = tr::variant; q.id = ++tr::reqId;
    tr::onScreen = tr::state == tr::Replaced ? tr::shown : tr::source;
    tr::state = tr::Pending; tr::pendingSince = tr::lastUse = GetTickCount64();
    translate::request(q);
    TrNotify(0, L"");
}

/// The translate key (called from the hook: quick, the request runs on its own thread).
static void TrPressed() {
    if (!tr::enabled) return;
    if (!translate::configured()) { TrNotify(1, L"Chưa cài đặt API dịch: chuột phải icon Vikey → Cài đặt dịch…"); return; }
    if (tr::state == tr::Pending || tr::state == tr::Reading) return;
    tr::reqWindow = GetForegroundWindow(); tr::reqFocus = TrFocusNow();
    if (tr::state == tr::Replaced) { tr::variant++; TrSend(); return; }   // another wording of the same text
    tr::variant = 0;
    bool blank = true; for (wchar_t c : tr::buf) if (c != L' ' && c != L'\n') blank = false;
    if (!blank && !tr::incomplete) { tr::source = tr::buf; tr::wholeField = false; TrSend(); return; }
    // nothing typed since the cursor moved, or typing buf couldn't follow: translate the whole field, read through UI
    // Automation (not the clipboard, whose history would keep it)
    tr::state = tr::Reading; tr::pendingSince = GetTickCount64(); unsigned id = ++tr::reqId; HWND o = tr::owner;
    std::thread([id, o] {
        std::wstring t; int rc = site::readFocusedField(t);
        { std::lock_guard<std::mutex> g(tr::fieldMutex); TrWipe(tr::field); tr::field = t; tr::fieldRc = rc; tr::fieldId = id; }
        TrWipe(t);
        PostMessageW(o, WM_TR_FIELD, id, 0);
    }).detach();
    TrNotify(0, L"");
}

static void TrFieldRead(unsigned id) {
    std::wstring t; int rc;
    { std::lock_guard<std::mutex> g(tr::fieldMutex); if (tr::fieldId != id) return; t = tr::field; TrWipe(tr::field); rc = tr::fieldRc; }
    if (id != tr::reqId || tr::state != tr::Reading) { TrWipe(t); return; }
    tr::state = tr::Idle; TrNotify(0, L"");
    if (GetForegroundWindow() != tr::reqWindow) { TrWipe(t); return; }
    std::wstring n; for (size_t i = 0; i < t.size(); i++) { if (t[i] == L'\r') { if (i + 1 < t.size() && t[i + 1] == L'\n') continue; n += L'\n'; } else n += t[i]; }
    TrWipe(t);
    bool blank = true; for (wchar_t c : n) if (c != L' ' && c != L'\n' && c != L'\t') blank = false;
    if (rc == 2) { TrWipe(n); TrNotify(1, L"Ô đang chọn là ô mật khẩu hoặc chỉ đọc: không dịch"); return; }
    if (rc != 0) { TrNotify(1, L"Không đọc được ô chat này. Gõ lại đoạn cần dịch rồi bấm phím dịch"); return; }
    if (blank) { TrWipe(n); TrNotify(1, L"Ô chat đang trống"); return; }
    if (n.size() > 4000) { TrWipe(n); TrNotify(1, L"Đoạn quá dài để dịch (tối đa 4000 ký tự)"); return; }
    tr::source = n; TrWipe(n); tr::wholeField = true; TrSend();
}

static void TrTranslated(unsigned id) {
    translate::Result r;
    if (!translate::take(id, r)) return;
    if (id != tr::reqId || tr::state != tr::Pending) { TrWipe(r.text); return; }
    if (!r.ok) {
        tr::state = tr::variant > 0 ? tr::Replaced : tr::Idle;   // after a failed re-wording the last translation is still on screen
        TrNotify(0, L""); TrNotify(1, r.error); return;
    }
    // the spaces and line breaks around the original stay (the translator trims them): " bạn" typed after a word → " you"
    size_t lead = 0, trail = 0;
    while (lead < tr::source.size() && (tr::source[lead] == L' ' || tr::source[lead] == L'\n')) lead++;
    while (trail < tr::source.size() - lead && (tr::source[tr::source.size() - 1 - trail] == L' ' || tr::source[tr::source.size() - 1 - trail] == L'\n')) trail++;
    std::wstring text = tr::source.substr(0, lead) + r.text + tr::source.substr(tr::source.size() - trail); TrWipe(r.text);
    TrWhenKeysUp([text, id]() mutable {
        if (tr::state != tr::Pending || id != tr::reqId) { TrWipe(text); return; }
        if (GetForegroundWindow() != tr::reqWindow || TrFocusNow() != tr::reqFocus) {
            TrWipe(text); TrForget(); TrNotify(1, L"Đã đổi cửa sổ trong lúc dịch: bỏ bản dịch"); return;
        }
        bool keepLines = tr::source.find(L'\n') != std::wstring::npos;
        TrReplace(tr::onScreen, text, tr::wholeField, keepLines);
        tr::shown = TrTypedForm(text, keepLines); TrWipe(text); TrWipe(tr::onScreen);
        tr::state = tr::Replaced; tr::lastUse = GetTickCount64();
        TrNotify(0, L"");
    });
}

/// Backspace right after a translation: the original text comes back (and stays tracked, ready to edit and translate again).
static void TrUndo() {
    if (tr::state != tr::Replaced) return;
    tr::state = tr::Idle;   // now, in the hook: a second Backspace right after is an ordinary one
    TrWhenKeysUp([] {
        bool keepLines = tr::source.find(L'\n') != std::wstring::npos;
        TrReplace(tr::shown, tr::source, tr::wholeField, keepLines);
        TrWipe(tr::shown); TrWipe(tr::source);
        TrNotify(0, L"");
    });
}

/// Source Vietnamese: type in Vietnamese. Another source language: English mode, so Telex leaves the words alone.
static void TrApplyPairLanguage() {
    std::wstring src = TrPref(L"TranslateSource", L"vi");
    if (src == L"vi") setLanguageQuiet(1); else if (src != L"auto") setLanguageQuiet(0);
}

static bool _keysPaused = false;   // a shortcut is being recorded in the settings window: every key goes to it untouched
void setKeysPaused(bool paused) { _keysPaused = paused; if (paused) resetTyping(); }

static LRESULT CALLBACK keyboardHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || _keysPaused) return CallNextHookEx(_kbHook, nCode, wParam, lParam);
    KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
    if (k->dwExtraInfo == VIKEY_EVENT_TAG) return CallNextHookEx(_kbHook, nCode, wParam, lParam);   // our own output
    bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    bool modifier = TrIsModifierVk(k->vkCode);
    if (!down && k->vkCode == tr::hotkeyVk) tr::hotkeyVk = 0;
    if (down && !modifier) {
        unsigned f = TrFlags();
        Uint16 key = engineKey(k->vkCode);
        bool toggle = TrHotkeyIs(_trToggleKey, key, f);
        bool press = !toggle && tr::enabled && TrHotkeyIs(_trHotkey, key, f) && !site::focusedIsPassword() && !TrSensitiveApp();
        if (toggle || press) {
            if (k->vkCode == tr::hotkeyVk) return 1;   // held down: once per press
            tr::hotkeyVk = k->vkCode;
            TrMaskRelease(f); cancelModifierHotkey();
            if (toggle) PostMessageW(tr::owner, WM_TR_TOGGLE, 0, 0); else TrPressed();
            return 1;
        }
        if (tr::enabled) {
            if (tr::state == tr::Pending || tr::state == tr::Reading) {
                if (k->vkCode == VK_ESCAPE) { TrForget(); return 1; }
                if (!windowsShortcut(k->vkCode, f) && !deviceKey(k->vkCode)) return 1;   // swallowed until the translation is in
                TrForget();   // Alt+Tab, Win+D...: give the translation up, the key goes on
            } else if (tr::state == tr::Replaced) {
                if (k->vkCode == VK_BACK && !(f & (F_CONTROL | F_ALT | F_WIN))) { TrUndo(); return 1; }
                // typing on after a translation: only what comes next is tracked, the translation stays as it is
                TrJump(); TrWipe(tr::source); TrWipe(tr::shown); tr::state = tr::Idle; TrNotify(0, L"");
            }
        }
    }
    _passed = false;
    LRESULT r = keyboardHookInner(nCode, wParam, lParam);
    if (down && !modifier && _passed) TrNotePassedKey(k->vkCode);
    return r;
}

bool translateEnabled() { return tr::enabled; }
bool translateBusy() { return tr::state == tr::Pending || tr::state == tr::Reading; }
void setTranslateWindow(HWND owner) { tr::owner = owner; }
void setTranslateObserver(std::function<void(int, const std::wstring&)> f) { tr::observer = f; }
void setTranslateEnabled(bool on) {
    if (tr::state != tr::Idle) TrForget();
    TrJump();
    tr::enabled = on; tr::lastUse = GetTickCount64();
    if (on) TrApplyPairLanguage();
    TrNotify(0, L"");
}
void translatePairChanged() { if (tr::enabled) TrApplyPairLanguage(); TrNotify(0, L""); }
void onTranslateMessage(UINT msg, WPARAM wp) {
    if (msg == WM_TR_TOGGLE) setTranslateEnabled(!tr::enabled);
    else if (msg == WM_TR_FIELD) TrFieldRead((unsigned)wp);
    else if (msg == translate::WM_TRANSLATED) TrTranslated((unsigned)wp);
}
void translateTick() {
    ULONGLONG now = GetTickCount64();
    // keys are swallowed while waiting: never for long, whatever got stuck (the server, or an app not answering UI Automation)
    if (tr::state == tr::Pending && now - tr::pendingSince > 12000) { TrForget(); TrNotify(1, L"Máy chủ dịch không trả lời: đã huỷ"); }
    if (tr::state == tr::Reading && now - tr::pendingSince > 4000) { TrForget(); TrNotify(1, L"Không đọc được ô chat: đã huỷ"); }
    int minutes = prefs::getInt(L"TranslateAutoOffMinutes", 10);
    if (tr::enabled && minutes > 0 && !translateBusy() && now - tr::lastUse > (ULONGLONG)minutes * 60000) {
        setTranslateEnabled(false);
        TrNotify(1, L"Chế độ dịch đã tự tắt sau " + std::to_wstring(minutes) + L" phút không dùng");
    }
}

bool start() {
    if (_kbHook) return true;
    pData = (vKeyHookState*)vKeyInit();
    reloadSettings();
    std::vector<BYTE> data = prefs::getBinary(L"smartSwitchKey");
    initSmartSwitchKey(data.data(), (int)data.size());
    queryFrontMostApp(); _tlApp = _frontApp;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    _kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, inst, 0);
    _mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, inst, 0);
    _fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // focus moves (into a password field, say) are checked right away, not only on the 1 s poll
    _focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, [](HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) { site::pollPassword(h, obj, child); }, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    return _kbHook != nullptr;
}
void stop() {
    flushTypingLog();
    if (_kbHook) UnhookWindowsHookEx(_kbHook); if (_mouseHook) UnhookWindowsHookEx(_mouseHook); if (_fgHook) UnhookWinEvent(_fgHook); if (_focusHook) UnhookWinEvent(_focusHook);
    _kbHook = nullptr; _mouseHook = nullptr; _fgHook = nullptr;
}
bool isRunning() { return _kbHook != nullptr; }

}} // namespace
