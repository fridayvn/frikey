#include "Translate.h"
#include "TranslateText.h"
#include "Prefs.h"
#include <wincred.h>
#include <winhttp.h>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

namespace vikey { namespace translate {

static HWND _owner = nullptr;
static std::mutex _mutex;
static std::map<unsigned, Result> _done;
static const wchar_t* CRED_TARGET = L"VikeyDich/translate-api-key";

void start(HWND owner) { _owner = owner; }

// ---- API key (Credential Manager, this user on this machine, not roamed) -------------------------
bool hasKey() {
    PCREDENTIALW c = nullptr;
    if (!CredReadW(CRED_TARGET, CRED_TYPE_GENERIC, 0, &c)) return false;
    bool ok = c->CredentialBlobSize > 0; CredFree(c); return ok;
}
bool setKey(const std::wstring& key) {
    if (key.empty()) { CredDeleteW(CRED_TARGET, CRED_TYPE_GENERIC, 0); return true; }
    std::string blob = text::toUtf8(key);
    CREDENTIALW c = {}; c.Type = CRED_TYPE_GENERIC; c.TargetName = (LPWSTR)CRED_TARGET; c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    c.CredentialBlob = (LPBYTE)&blob[0]; c.CredentialBlobSize = (DWORD)blob.size(); c.UserName = (LPWSTR)L"VikeyDich";
    BOOL ok = CredWriteW(&c, 0);
    SecureZeroMemory(&blob[0], blob.size());
    return ok != FALSE;
}
static std::string readKey() {
    PCREDENTIALW c = nullptr; std::string k;
    if (CredReadW(CRED_TARGET, CRED_TYPE_GENERIC, 0, &c)) {
        k.assign((const char*)c->CredentialBlob, c->CredentialBlobSize);
        SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize); CredFree(c);
    }
    return k;
}

static bool mock() { return prefs::getInt(L"TranslateMock", 0) != 0; }
bool configured() {
    if (mock()) return true;
    return !prefs::getString(L"TranslateHost", L"").empty() && !prefs::getString(L"TranslatePath", L"").empty() &&
           !prefs::getString(L"TranslateModel", L"").empty() && hasKey();
}

// ---- One HTTPS POST ------------------------------------------------------------------------------
static bool post(const std::wstring& host, const std::wstring& path, const std::string& key, const std::string& body,
                 DWORD& status, std::string& response, std::wstring& error) {
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"VikeyDich", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"VikeyDich", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);   // before Windows 8.1
    if (!s) { error = L"Không mở được WinHTTP"; return false; }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2; WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    WinHttpSetTimeouts(s, 3000, 3000, 8000, 8000);
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r) {
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;   // the text goes to the configured host and nowhere else
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
        std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + text::fromUtf8(key) + L"\r\n";
        BOOL sent = WinHttpSendRequest(r, headers.c_str(), (DWORD)headers.size(), (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        SecureZeroMemory(&headers[0], headers.size() * sizeof(wchar_t));
        if (sent && WinHttpReceiveResponse(r, nullptr)) {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail > 0 && response.size() < 1024 * 1024) {
                std::vector<char> buf(avail); DWORD read = 0;
                if (!WinHttpReadData(r, buf.data(), avail, &read) || read == 0) break;
                response.append(buf.data(), read);
            }
            ok = true;
        } else {
            DWORD e = GetLastError();
            error = e == ERROR_WINHTTP_TIMEOUT ? L"Hết thời gian chờ máy chủ dịch" :
                    e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? L"Không tìm thấy máy chủ dịch (kiểm tra mạng hoặc host)" :
                    e == ERROR_WINHTTP_SECURE_FAILURE ? L"Lỗi chứng chỉ HTTPS của máy chủ dịch" :
                    L"Không kết nối được máy chủ dịch (lỗi " + std::to_wstring(e) + L")";
        }
        WinHttpCloseHandle(r);
    } else error = L"Host hoặc đường dẫn API không hợp lệ";
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

static Result run(const Request& q) {
    Result res; res.id = q.id;
    if (mock()) { Sleep(300); res.ok = true; res.text = text::filterOutput(L"EN:" + q.text, q.text.size()); return res; }
    std::wstring host = prefs::getString(L"TranslateHost", L""), path = prefs::getString(L"TranslatePath", L"");
    std::string model = text::toUtf8(prefs::getString(L"TranslateModel", L""));
    std::string key = readKey();
    if (host.empty() || path.empty() || model.empty() || key.empty()) { res.error = L"Chưa cài đặt API dịch"; return res; }
    std::string body = text::requestBody(model, q.text, q.source, q.target, q.variant), response;
    DWORD status = 0; std::wstring error;
    bool sent = post(host, path, key, body, status, response, error);
    SecureZeroMemory(&key[0], key.size());
    SecureZeroMemory(&body[0], body.size());
    if (!sent) { res.error = error; return res; }
    if (status != 200) {
        std::wstring why = text::parseError(response);
        res.error = L"Máy chủ dịch trả lỗi " + std::to_wstring(status) + (why.empty() ? L"" : L": " + why);
    } else {
        std::wstring answer;
        if (!text::parseContent(response, answer)) res.error = L"Không đọc được câu trả lời của máy chủ dịch";
        else {
            res.text = text::filterOutput(answer, q.text.size());
            if (res.text.empty()) res.error = L"Bản dịch trống hoặc bất thường, đã bỏ";
            else res.ok = true;
            SecureZeroMemory(&answer[0], answer.size() * sizeof(wchar_t));
        }
    }
    if (!response.empty()) SecureZeroMemory(&response[0], response.size());
    return res;
}

void request(const Request& q) {
    HWND owner = _owner;
    std::thread([q, owner] {
        Result res = run(q);
        { std::lock_guard<std::mutex> g(_mutex); _done[res.id] = res; }
        if (owner) PostMessageW(owner, WM_TRANSLATED, (WPARAM)res.id, 0);
    }).detach();
}

bool take(unsigned id, Result& out) {
    std::lock_guard<std::mutex> g(_mutex);
    auto it = _done.find(id); if (it == _done.end()) return false;
    out = it->second; _done.erase(it);
    for (auto i = _done.begin(); i != _done.end();) if (i->first < id) i = _done.erase(i); else ++i;   // older, abandoned results
    return true;
}

}}
