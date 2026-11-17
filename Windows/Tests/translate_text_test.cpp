// Unit tests for TranslateText.h, no Windows needed: clang++ -std=c++17 -I../App translate_text_test.cpp && ./a.out
#include "TranslateText.h"
#include <cstdio>
using namespace vikey::translate::text;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    // UTF-8 round trip: Vietnamese, CJK, an astral emoji
    std::wstring vi = L"Xin chào, mình là Long 😀 你好";
    CHECK(fromUtf8(toUtf8(vi)) == vi);
    CHECK(toUtf8(L"đ") == "\xC4\x91");

    // request body: escapes and the message as the user turn, not inside the system prompt
    std::string body = requestBody("gpt-x", L"anh nói \"ok\"\nnhé", L"vi", L"en", 0);
    CHECK(body.find("\"model\":\"gpt-x\"") != std::string::npos);
    CHECK(body.find("anh n\xC3\xB3i \\\"ok\\\"\\nnh\xC3\xA9") != std::string::npos);
    CHECK(body.find("from Vietnamese into English") != std::string::npos);
    CHECK(requestBody("m", L"x", L"auto", L"ja", 2).find("alternative #2") != std::string::npos);
    CHECK(requestBody("m", L"x", L"auto", L"ja", 0).find("into Japanese") != std::string::npos);

    // response parsing
    std::wstring out;
    std::string r1 = "{\"id\":\"x\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Hello, I'm \\\"Long\\\" \\ud83d\\ude00\\n\\u00e9\"},\"finish_reason\":\"stop\"}]}";
    CHECK(parseContent(r1, out) && out == L"Hello, I'm \"Long\" 😀\né");
    std::string r2 = "{ \"choices\" : [ { \"message\" : { \"content\" : \"Xin ch\xC3\xA0o\" , \"role\":\"assistant\"} } ] }";
    CHECK(parseContent(r2, out) && out == L"Xin chào");
    CHECK(!parseContent("{\"choices\":[{\"message\":{\"content\":null}}]}", out));
    CHECK(!parseContent("{\"error\":{\"message\":\"bad key\"}}", out));
    CHECK(!parseContent("{\"choices\":[{\"message\":{\"content\":\"cut off", out));
    CHECK(parseError("{\"error\":{\"message\":\"Incorrect API key\",\"type\":\"x\"}}") == L"Incorrect API key");

    // output filter
    CHECK(filterOutput(L"  \"Hello there\"  ", 11) == L"Hello there");
    CHECK(filterOutput(L"\"a\" and \"b\"", 5) == L"\"a\" and \"b\"");
    CHECK(filterOutput(L"Hi\r\nthere\x1b[2J", 8) == L"Hi\nthere[2J");
    CHECK(filterOutput(L"ab\u202Ecd\u200Bef\uFEFF", 6) == L"abcdef");
    CHECK(filterOutput(L"line1\rline2", 10) == L"line1\nline2");
    CHECK(filterOutput(L"   \n ", 5).empty());
    CHECK(filterOutput(std::wstring(500, L'x'), 10).empty());
    CHECK(filterOutput(L"“Chào”", 4) == L"Chào");

    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails;
}
