// FriKey's look: Windows 11 (WinUI 3 / Fluent) colours and metrics, Plus Jakarta Sans through DirectWrite, drawing
// helpers on Direct2D. Everything is laid out in DIPs (1/96 inch) and drawn by a render target set to the window's DPI,
// so it stays sharp and the same physical size on a 4K screen.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>

namespace vikey { namespace look {

struct Theme {
    bool dark;
    D2D1_COLOR_F bg, nav, card, cardHover, cardStroke, text, text2, text3, accent, accentText, accentHover,
                 control, controlHover, controlPressed, controlStroke, controlBottom, divider, subtle, subtleHover,
                 toggleOffStroke, menuBg, menuStroke, danger, success;
};
const Theme& theme();
void refreshTheme();                 // after WM_SETTINGCHANGE "ImmersiveColorSet" or an accent colour change
COLORREF gdi(const D2D1_COLOR_F& c, const D2D1_COLOR_F& over);   // a translucent colour flattened onto `over`

ID2D1Factory* d2d();
IDWriteFactory* dwrite();
/// Plus Jakarta Sans (embedded). weight: 400, 500, 600, 700. size in DIPs.
IDWriteTextFormat* font(float size, int weight = 400, bool center = false);
/// The Fluent icon font (Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets before).
IDWriteTextFormat* icons(float size);
/// The GDI face name for native controls (Edit boxes): the embedded font is also registered with GDI for this process.
const wchar_t* gdiFace();

float scaleOf(HWND h);               // DPI / 96
void applyWindowStyle(HWND h, bool popup);   // dark title bar, caption colour, rounded corners

D2D1_RECT_F R(float x, float y, float w, float h);
void fill(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c);
void stroke(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float width = 1);
void line(ID2D1RenderTarget* rt, float x1, float y1, float x2, float y2, const D2D1_COLOR_F& c, float width = 1);
/// Draws text in r; returns the height it took (wrapped to r's width).
float text(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c,
           DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
float measureHeight(const std::wstring& s, IDWriteTextFormat* f, float width);
float measureWidth(const std::wstring& s, IDWriteTextFormat* f);

/// The brand mark: a rounded square in FriKey's gradient with an "F".
void logo(ID2D1RenderTarget* rt, float x, float y, float size);

// Fluent icon code points
const wchar_t IC_CHECK = 0xE73E, IC_CHEVRON_R = 0xE76C, IC_CHEVRON_D = 0xE70D, IC_KEYBOARD = 0xE765, IC_GLOBE = 0xE774,
              IC_INFO = 0xE946, IC_SWAP = 0xE8AB, IC_SETTINGS = 0xE713, IC_POWER = 0xE7E8, IC_SHIELD = 0xEA18, IC_LINK = 0xE71B;
}}
