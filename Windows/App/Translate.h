// Translate mode, the network side: the only code in this fork that talks to the network. One HTTPS POST to the
// configured chat-completions endpoint per press of the translate key, on a thread of its own (never the hook thread).
// Nothing is written to disk: no log, no cache. The API key lives in Windows Credential Manager, not the registry.
#pragma once
#include <windows.h>
#include <string>

namespace vikey { namespace translate {

/// Posted to the owner window when a request finishes (wParam = request id). Read the result with take().
const UINT WM_TRANSLATED = WM_APP + 21;

struct Request {
    std::wstring text;      // what to translate
    std::wstring source;    // language code ("vi", "en"...) or "auto"
    std::wstring target;    // language code
    int variant = 0;        // 0 = first translation, 1, 2... = another wording of the same text
    unsigned id = 0;        // echoed in the result; the caller drops results it no longer waits for
    HWND reply = nullptr;   // where WM_TRANSLATED goes (default: the window given to start()). Ids with the top bit set
                            // are a separate series (the settings window's "Kiểm tra"), so neither drops the other's results.
};
struct Result {
    bool ok = false;
    std::wstring text;      // filtered, safe to type
    std::wstring error;     // one line for the tray tooltip; never contains the message
    unsigned id = 0;
};

void start(HWND owner);
/// Starts a request on its own thread and returns at once.
void request(const Request& r);
/// The finished result for this id, if any.
bool take(unsigned id, Result& out);

// Settings (registry, under the app's key): TranslateHost, TranslatePath, TranslateModel; TranslateMock = 1 answers
// "EN:<text>" without any network call (live tests).
bool configured();          // host, path, model and key all set (or mock on)
bool hasKey();
bool setKey(const std::wstring& key);   // empty = delete
}}
