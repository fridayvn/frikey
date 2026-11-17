#include "Ui.h"
#include "resource.h"
#include <dwrite_3.h>
#include <dwmapi.h>
#include <map>
#include <vector>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

namespace vikey { namespace look {

static D2D1_COLOR_F hex(UINT32 rgb, float a = 1) { return D2D1::ColorF(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a); }
static D2D1_COLOR_F mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) { return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1); }

static Theme _theme; static bool _themeReady = false;

static DWORD regDword(const wchar_t* key, const wchar_t* name, DWORD fallback) {
    HKEY h; DWORD v = fallback, s = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &h) == ERROR_SUCCESS) { if (RegQueryValueExW(h, name, nullptr, nullptr, (LPBYTE)&v, &s) != ERROR_SUCCESS) v = fallback; RegCloseKey(h); }
    return v;
}

void refreshTheme() {
    bool dark = regDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) == 0;
    // the user's accent colour (ABGR), as Settings > Personalization > Colors has it; Windows' default blue otherwise
    DWORD abgr = regDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFD47800);
    D2D1_COLOR_F base = D2D1::ColorF((abgr & 255) / 255.0f, ((abgr >> 8) & 255) / 255.0f, ((abgr >> 16) & 255) / 255.0f, 1);
    Theme t = {};
    t.dark = dark;
    if (!dark) {
        t.bg = hex(0xF3F3F3); t.nav = hex(0xF3F3F3); t.card = hex(0xFBFBFB); t.cardHover = hex(0xF6F6F6); t.cardStroke = hex(0x000000, 0.0578f);
        t.text = hex(0x000000, 0.8956f); t.text2 = hex(0x000000, 0.6063f); t.text3 = hex(0x000000, 0.4458f);
        t.accent = mix(base, hex(0x000000), 0.18f); t.accentText = hex(0xFFFFFF); t.accentHover = mix(t.accent, hex(0xFFFFFF), 0.1f);
        t.control = hex(0xFFFFFF, 0.7f); t.controlHover = hex(0xF9F9F9, 0.5f); t.controlPressed = hex(0xF9F9F9, 0.3f);
        t.controlStroke = hex(0x000000, 0.0578f); t.controlBottom = hex(0x000000, 0.1622f); t.divider = hex(0x000000, 0.0803f);
        t.subtle = hex(0x000000, 0.0241f); t.subtleHover = hex(0x000000, 0.0373f); t.toggleOffStroke = hex(0x000000, 0.6063f);
        t.menuBg = hex(0xF9F9F9); t.menuStroke = hex(0x000000, 0.0578f); t.danger = hex(0xC42B1C); t.success = hex(0x0F7B0F);
    } else {
        t.bg = hex(0x202020); t.nav = hex(0x202020); t.card = hex(0x2B2B2B); t.cardHover = hex(0x323232); t.cardStroke = hex(0x000000, 0.2f);
        t.text = hex(0xFFFFFF); t.text2 = hex(0xFFFFFF, 0.786f); t.text3 = hex(0xFFFFFF, 0.5442f);
        t.accent = mix(base, hex(0xFFFFFF), 0.45f); t.accentText = hex(0x000000); t.accentHover = mix(t.accent, hex(0x000000), 0.1f);
        t.control = hex(0xFFFFFF, 0.0605f); t.controlHover = hex(0xFFFFFF, 0.0837f); t.controlPressed = hex(0xFFFFFF, 0.0326f);
        t.controlStroke = hex(0xFFFFFF, 0.0698f); t.controlBottom = hex(0xFFFFFF, 0.0930f); t.divider = hex(0xFFFFFF, 0.0837f);
        t.subtle = hex(0xFFFFFF, 0.0419f); t.subtleHover = hex(0xFFFFFF, 0.0605f); t.toggleOffStroke = hex(0xFFFFFF, 0.786f);
        t.menuBg = hex(0x2C2C2C); t.menuStroke = hex(0x000000, 0.2f); t.danger = hex(0xFF99A4); t.success = hex(0x6CCB5F);
    }
    _theme = t; _themeReady = true;
}
const Theme& theme() { if (!_themeReady) refreshTheme(); return _theme; }
COLORREF gdi(const D2D1_COLOR_F& c, const D2D1_COLOR_F& over) {
    float r = over.r + (c.r - over.r) * c.a, g = over.g + (c.g - over.g) * c.a, b = over.b + (c.b - over.b) * c.a;
    return RGB((int)(r * 255 + 0.5f), (int)(g * 255 + 0.5f), (int)(b * 255 + 0.5f));
}

// ---- factories and fonts ---------------------------------------------------------------------
static ID2D1Factory* _d2d = nullptr; static IDWriteFactory* _dw = nullptr; static IDWriteFontCollection* _fonts = nullptr;
static bool _fontsTried = false; static const wchar_t* _gdiFace = L"Segoe UI";
ID2D1Factory* d2d() { if (!_d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &_d2d); return _d2d; }
IDWriteFactory* dwrite() { if (!_dw) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&_dw); return _dw; }

static bool resourceBytes(int id, const void** data, DWORD* size) {
    HMODULE m = GetModuleHandleW(nullptr); HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(m, r) : nullptr; *data = g ? LockResource(g) : nullptr; *size = r ? SizeofResource(m, r) : 0;
    return *data && *size;
}
/// Plus Jakarta Sans from the exe's resources: a DirectWrite collection for drawing, and GDI (this process only) for Edit boxes.
static void loadFonts() {
    if (_fontsTried) return; _fontsTried = true;
    const int ids[] = {IDR_FONT_REGULAR, IDR_FONT_MEDIUM, IDR_FONT_SEMIBOLD, IDR_FONT_BOLD};
    IDWriteFactory5* f5 = nullptr;
    if (dwrite() && SUCCEEDED(_dw->QueryInterface(__uuidof(IDWriteFactory5), (void**)&f5)) && f5) {
        IDWriteInMemoryFontFileLoader* loader = nullptr; IDWriteFontSetBuilder1* builder = nullptr;
        if (SUCCEEDED(f5->CreateInMemoryFontFileLoader(&loader)) && SUCCEEDED(f5->RegisterFontFileLoader(loader)) && SUCCEEDED(f5->CreateFontSetBuilder(&builder))) {
            for (int id : ids) {
                const void* data; DWORD size; IDWriteFontFile* file = nullptr;
                if (resourceBytes(id, &data, &size) && SUCCEEDED(loader->CreateInMemoryFontFileReference(f5, data, size, nullptr, &file))) { builder->AddFontFile(file); file->Release(); }
            }
            IDWriteFontSet* set = nullptr; IDWriteFontCollection1* coll = nullptr;
            if (SUCCEEDED(builder->CreateFontSet(&set)) && SUCCEEDED(f5->CreateFontCollectionFromFontSet(set, &coll))) _fonts = coll;
            if (set) set->Release();
        }
        if (builder) builder->Release();
        f5->Release();   // the loader stays registered for the life of the process
    }
    for (int id : ids) { const void* data; DWORD size; DWORD n = 0; if (resourceBytes(id, &data, &size)) AddFontMemResourceEx((void*)data, size, nullptr, &n); }
    HDC dc = GetDC(nullptr); LOGFONTW lf = {}; wcscpy_s(lf.lfFaceName, L"Plus Jakarta Sans"); lf.lfCharSet = DEFAULT_CHARSET;
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *(bool*)p = true; return 0; }, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    if (found) _gdiFace = L"Plus Jakarta Sans";
}
const wchar_t* gdiFace() { loadFonts(); return _gdiFace; }

static bool hasFamily(const wchar_t* name) { UINT32 i; BOOL e = FALSE; return _fonts && SUCCEEDED(_fonts->FindFamilyName(name, &i, &e)) && e; }

IDWriteTextFormat* font(float size, int weight, bool center) {
    static std::map<long long, IDWriteTextFormat*> cache;
    long long key = (long long)(size * 100) * 10000 + weight * 10 + (center ? 1 : 0);
    auto it = cache.find(key); if (it != cache.end()) return it->second;
    loadFonts();
    // the static font files name their families by weight: "Plus Jakarta Sans" holds Regular and Bold
    const wchar_t* family = L"Plus Jakarta Sans"; DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL;
    if (weight >= 700) w = DWRITE_FONT_WEIGHT_BOLD;
    else if (weight >= 600) { w = DWRITE_FONT_WEIGHT_SEMI_BOLD; if (hasFamily(L"Plus Jakarta Sans SemiBold")) family = L"Plus Jakarta Sans SemiBold"; }
    else if (weight >= 500) { w = DWRITE_FONT_WEIGHT_MEDIUM; if (hasFamily(L"Plus Jakarta Sans Medium")) family = L"Plus Jakarta Sans Medium"; }
    IDWriteFontCollection* coll = _fonts;
    if (!hasFamily(family)) { family = L"Segoe UI Variable Text"; coll = nullptr; }
    IDWriteTextFormat* f = nullptr;
    if (dwrite()) _dw->CreateTextFormat(family, coll, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"vi-vn", &f);
    if (f) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        if (center) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    }
    cache[key] = f; return f;
}
IDWriteTextFormat* icons(float size) {
    static std::map<int, IDWriteTextFormat*> cache;
    int key = (int)(size * 100); auto it = cache.find(key); if (it != cache.end()) return it->second;
    static const wchar_t* face = nullptr;
    if (!face) {
        face = L"Segoe MDL2 Assets"; IDWriteFontCollection* sys = nullptr;
        if (dwrite() && SUCCEEDED(_dw->GetSystemFontCollection(&sys)) && sys) { UINT32 i; BOOL e = FALSE; if (SUCCEEDED(sys->FindFamilyName(L"Segoe Fluent Icons", &i, &e)) && e) face = L"Segoe Fluent Icons"; sys->Release(); }
    }
    IDWriteTextFormat* f = nullptr;
    if (dwrite()) _dw->CreateTextFormat(face, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    cache[key] = f; return f;
}

float scaleOf(HWND h) {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = fn && h ? fn(h) : 96;
    if (!dpi) { HDC dc = GetDC(nullptr); dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
    return dpi / 96.0f;
}

void applyWindowStyle(HWND h, bool popup) {
    const Theme& t = theme();
    BOOL dark = t.dark; DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    int corner = popup ? 3 /* DWMWCP_ROUNDSMALL */ : 2 /* DWMWCP_ROUND */; DwmSetWindowAttribute(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    if (!popup) { COLORREF cap = gdi(t.bg, t.bg); DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap)); }
}

// ---- drawing ---------------------------------------------------------------------------------
D2D1_RECT_F R(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
/// A brush for one call (render targets come and go with their windows; a cached brush would outlive its target).
struct Brush { ID2D1SolidColorBrush* b = nullptr; Brush(ID2D1RenderTarget* rt, const D2D1_COLOR_F& c) { rt->CreateSolidColorBrush(c, &b); } ~Brush() { if (b) b->Release(); } };
void fill(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
    Brush b(rt, c); if (!b.b) return;
    if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b.b); else rt->FillRectangle(r, b.b);
}
void stroke(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float width) {
    Brush b(rt, c); if (!b.b) return;
    D2D1_RECT_F in = D2D1::RectF(r.left + width / 2, r.top + width / 2, r.right - width / 2, r.bottom - width / 2);
    if (radius > 0) rt->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), b.b, width); else rt->DrawRectangle(in, b.b, width);
}
void line(ID2D1RenderTarget* rt, float x1, float y1, float x2, float y2, const D2D1_COLOR_F& c, float width) {
    Brush b(rt, c); if (b.b) rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b.b, width);
}
float text(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c,
           DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign) {
    if (!f || s.empty() || !dwrite()) return 0;
    IDWriteTextLayout* l = nullptr;
    if (FAILED(_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, r.right - r.left, r.bottom - r.top, &l)) || !l) return 0;
    l->SetTextAlignment(align); l->SetParagraphAlignment(valign);
    DWRITE_TEXT_METRICS m = {}; l->GetMetrics(&m);
    Brush b(rt, c);
    if (b.b) rt->DrawTextLayout(D2D1::Point2F(r.left, r.top), l, b.b, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    l->Release(); return m.height;
}
float measureHeight(const std::wstring& s, IDWriteTextFormat* f, float width) {
    if (!f || s.empty() || !dwrite()) return 0;
    IDWriteTextLayout* l = nullptr; if (FAILED(_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, width, 10000, &l)) || !l) return 0;
    DWRITE_TEXT_METRICS m = {}; l->GetMetrics(&m); l->Release(); return m.height;
}
float measureWidth(const std::wstring& s, IDWriteTextFormat* f) {
    if (!f || s.empty() || !dwrite()) return 0;
    IDWriteTextLayout* l = nullptr; if (FAILED(_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 1000, &l)) || !l) return 0;
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m = {}; l->GetMetrics(&m); l->Release(); return m.widthIncludingTrailingWhitespace;
}

void logo(ID2D1RenderTarget* rt, float x, float y, float size) {
    D2D1_GRADIENT_STOP stops[2] = {{0, hex(0x7C5CFF)}, {1, hex(0x3B5BDB)}};
    ID2D1GradientStopCollection* sc = nullptr; ID2D1LinearGradientBrush* g = nullptr;
    rt->CreateGradientStopCollection(stops, 2, &sc);
    if (sc) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x, y), D2D1::Point2F(x + size, y + size)), sc, &g);
    float rad = size * 0.24f;
    if (g) { rt->FillRoundedRectangle(D2D1::RoundedRect(R(x, y, size, size), rad, rad), g); g->Release(); }
    if (sc) sc->Release();
    text(rt, L"F", font(size * 0.62f, 700, true), R(x, y - size * 0.04f, size, size), hex(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

}}
