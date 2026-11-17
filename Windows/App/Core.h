// The keyboard core on Windows: the low-level keyboard/mouse hooks that feed the Telex engine,
// smart switch per app/website, auto switch by typing, and the typing log. A port of
// App/Core/VikeyCore.mm (macOS) with the same behavior, decision for decision.
#pragma once
#include <windows.h>
#include <string>
#include <functional>

namespace vikey { namespace core {

bool start();                       // install hooks (needs a message loop on the calling thread)
void stop();
bool isRunning();
int language();                     // 0 English, 1 Vietnamese
void setLanguageByUser(int lang);   // from the tray menu / settings window
void setLanguageQuiet(int lang);    // tests: set the language without the 5 s pause or a log entry
void reloadSettings();              // after a setting changed in the registry
void onForegroundChanged();         // called by the foreground WinEvent hook
void onSiteChanged(const std::wstring& host);   // per-site memory (Phase 3)
std::wstring frontAppName();

/// Called on the UI thread whenever the language changes (menu icon, settings window).
void setLanguageObserver(std::function<void(int)> f);

// Emoji picker (Settings.cpp): called on the hook thread when its hotkey is pressed (EmojiPicker / EmojiHotkey prefs).
void setEmojiHotkeyHandler(std::function<void()> f);
/// While open, key downs go to the picker window as WM_APP+10 (wParam = VK, lParam = modifier flags: 1 Shift,
/// 2 Ctrl, 4 Alt, 8 Win, 16 Caps Lock); Windows' shortcuts (Alt+Tab, Win+D) or a click outside it post WM_APP+11
/// (close) and go on as usual, media keys just go on. The app in front keeps its cursor.
void setEmojiPickerOpen(bool open, HWND picker);
/// Forget the word being typed (the cursor moved to another field without a click).
void resetTyping();
/// Types text into the focused app (KEYEVENTF_UNICODE; surrogate pairs for astral emoji).
void insertText(const std::wstring& text);

// Translate mode (off at every launch). The window set here gets WM_TR_* and translate::WM_TRANSLATED: pass them to
// onTranslateMessage. The observer hears 0 (state changed: on/off, waiting) and 1 (a message for the user, never typed text).
const UINT WM_TR_TOGGLE = WM_APP + 22;   // the toggle key, posted from the hook
const UINT WM_TR_FIELD = WM_APP + 23;    // the focused field was read (wParam = request id)
bool translateEnabled();
bool translateBusy();
void setTranslateEnabled(bool on);
void translatePairChanged();             // after TranslateSource / TranslateTarget changed
void setTranslateWindow(HWND owner);
void setTranslateObserver(std::function<void(int event, const std::wstring& message)> f);
void onTranslateMessage(UINT msg, WPARAM wp);
void translateTick();                    // every few seconds: auto-off, a stuck request

/// While a shortcut is recorded in the settings window: no Telex, no hotkeys, every key reaches the window as pressed.
void setKeysPaused(bool paused);

// Typing log
std::wstring typingLogPath();
void flushTypingLog();
void clearTypingLog();

}} // namespace
