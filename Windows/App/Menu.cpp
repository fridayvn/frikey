#include "Menu.h"
#include "Ui.h"
#include <windowsx.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>

namespace vikey { namespace menu {

using namespace look;

// metrics in DIPs (Windows 11 MenuFlyout, a little roomier)
static const float PAD = 4, ROW = 26, SEP = 9, RADIUS = 4, GUTTER = 30, TEXT_PAD = 12, HINT_GAP = 24, CHEVRON = 18, MIN_W = 190;
static const float FONT = 12.5f, HINT_FONT = 11.5f;

struct Level {
    HWND hwnd = nullptr;
    const std::vector<Item>* items = nullptr;
    std::vector<float> tops;          // each item's top, DIPs
    float width = 0, height = 0, scale = 1;
    int hover = -1;
    Level* child = nullptr; Level* parent = nullptr; int childOf = -1;
    ID2D1HwndRenderTarget* rt = nullptr;
};

static Level* _root = nullptr;
static bool _done = false; static int _result = 0;
static bool _registered = false;

bool isOpen() { return _root != nullptr; }

static float itemHeight(const Item& it) { return it.separator ? SEP : ROW; }

static void layout(Level* l, float minWidth) {
    bool gutter = false; float textW = 0, hintW = 0; bool chevron = false;
    for (auto& it : *l->items) {
        if (it.separator) continue;
        if (it.checked || it.icon) gutter = true;
        textW = std::max(textW, measureWidth(it.text, font(FONT, it.bold ? 600 : 400)));
        if (!it.hint.empty()) hintW = std::max(hintW, measureWidth(it.hint, font(HINT_FONT)));
        if (!it.sub.empty()) chevron = true;
    }
    (void)gutter;
    float w = PAD + GUTTER + textW + (hintW > 0 ? HINT_GAP + hintW : 0) + (chevron ? CHEVRON : 0) + TEXT_PAD + PAD;
    l->width = std::ceil(std::max({w, MIN_W, minWidth}));
    float y = PAD; l->tops.clear();
    for (auto& it : *l->items) { l->tops.push_back(y); y += itemHeight(it); }
    l->height = y + PAD;
}

static D2D1_RECT_F itemRect(Level* l, int i) { return R(PAD, l->tops[i], l->width - 2 * PAD, itemHeight((*l->items)[i])); }

static void paint(Level* l) {
    if (!l->rt) {
        RECT rc; GetClientRect(l->hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96 * l->scale, 96 * l->scale);
        d2d()->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(l->hwnd, D2D1::SizeU(rc.right, rc.bottom)), &l->rt);
        if (!l->rt) return;
    }
    const Theme& t = theme();
    l->rt->BeginDraw();
    l->rt->Clear(t.menuBg);
    l->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    for (int i = 0; i < (int)l->items->size(); i++) {
        const Item& it = (*l->items)[i];
        D2D1_RECT_F r = itemRect(l, i);
        if (it.separator) { line(l->rt, r.left - PAD, r.top + SEP / 2, r.right + PAD, r.top + SEP / 2, t.divider); continue; }
        bool hot = i == l->hover && it.enabled;
        if (hot || (l->child && l->childOf == i)) fill(l->rt, r, RADIUS, t.subtleHover);
        D2D1_COLOR_F ink = it.enabled ? t.text : t.text3;
        if (it.icon) text(l->rt, std::wstring(1, it.icon), icons(13), R(r.left, r.top, GUTTER, ROW), ink);
        else if (it.checked) text(l->rt, std::wstring(1, IC_CHECK), icons(11), R(r.left, r.top, GUTTER, ROW), ink);
        float right = r.right - TEXT_PAD;
        if (!it.sub.empty()) { text(l->rt, std::wstring(1, IC_CHEVRON_R), icons(9), R(r.right - CHEVRON - 4, r.top, CHEVRON, ROW), it.enabled ? t.text2 : t.text3); right -= CHEVRON - 4; }
        if (!it.hint.empty())
            text(l->rt, it.hint, font(HINT_FONT), D2D1::RectF(r.left + GUTTER, r.top, right, r.bottom), it.enabled ? t.text2 : t.text3, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        text(l->rt, it.text, font(FONT, it.bold ? 600 : 400), D2D1::RectF(r.left + GUTTER, r.top, right, r.bottom), ink, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (l->rt->EndDraw() == D2DERR_RECREATE_TARGET) { l->rt->Release(); l->rt = nullptr; }
}

static Level* levelOf(HWND h) { for (Level* l = _root; l; l = l->child) if (l->hwnd == h) return l; return nullptr; }
static Level* deepest() { Level* l = _root; while (l && l->child) l = l->child; return l; }

static void closeChild(Level* l) {
    if (!l->child) return;
    closeChild(l->child);
    if (l->child->rt) l->child->rt->Release();
    DestroyWindow(l->child->hwnd); delete l->child; l->child = nullptr; l->childOf = -1;
    InvalidateRect(l->hwnd, nullptr, FALSE);
}

static float monitorScale(POINT p, RECT* work) {
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi); *work = mi.rcWork;
    UINT dx = 96, dy = 96;
    typedef HRESULT(WINAPI * Fn)(HMONITOR, int, UINT*, UINT*);
    static Fn fn = (Fn)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    if (fn) fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy);
    return dx / 96.0f;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM lp);

static Level* open(const std::vector<Item>* items, Level* parent, int childOf, POINT at, Align align, float minWidth, RECT avoid) {
    Level* l = new Level(); l->items = items; l->parent = parent; l->childOf = childOf;
    RECT work; l->scale = monitorScale(at, &work);
    layout(l, minWidth);
    int w = (int)std::ceil(l->width * l->scale), hgt = (int)std::ceil(l->height * l->scale);
    int x = at.x, y = align == AbovePoint ? at.y - hgt : at.y;
    if (parent) {   // a sub-menu: beside the parent row, the other side when it doesn't fit
        x = avoid.right - (int)(2 * l->scale); y = avoid.top - (int)(PAD * l->scale);
        if (x + w > work.right) x = avoid.left - w + (int)(2 * l->scale);
    } else if (x + w > work.right) x = at.x - w;
    if (x < work.left) x = work.left;
    if (y + hgt > work.bottom) y = work.bottom - hgt;
    if (y < work.top) y = work.top;
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | (parent ? WS_EX_NOACTIVATE : 0);
    l->hwnd = CreateWindowExW(ex, L"FriKeyMenu", L"", WS_POPUP, x, y, w, hgt, parent ? parent->hwnd : nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    applyWindowStyle(l->hwnd, true);
    if (parent) { parent->child = l; ShowWindow(l->hwnd, SW_SHOWNOACTIVATE); }
    else { _root = l; ShowWindow(l->hwnd, SW_SHOW); SetForegroundWindow(l->hwnd); SetFocus(l->hwnd); }   // _root first: the first paint looks it up
    UpdateWindow(l->hwnd);
    return l;
}

static void openSub(Level* l, int i, bool selectFirst) {
    const Item& it = (*l->items)[i];
    if (it.sub.empty() || !it.enabled) return;
    if (l->child && l->childOf == i) return;
    closeChild(l);
    D2D1_RECT_F r = itemRect(l, i);
    RECT wr; GetWindowRect(l->hwnd, &wr);
    RECT row = {wr.left, wr.top + (int)(r.top * l->scale), wr.right, wr.top + (int)(r.bottom * l->scale)};
    Level* c = open(&it.sub, l, i, POINT{row.right, row.top}, BelowPoint, 0, row);
    if (selectFirst) for (int k = 0; k < (int)c->items->size(); k++) if (!(*c->items)[k].separator && (*c->items)[k].enabled) { c->hover = k; break; }
    InvalidateRect(l->hwnd, nullptr, FALSE);
}

static void activate(Level* l, int i) {
    if (i < 0 || i >= (int)l->items->size()) return;
    const Item& it = (*l->items)[i];
    if (it.separator || !it.enabled) return;
    if (!it.sub.empty()) { openSub(l, i, true); return; }
    _result = it.id; _done = true;
}

static void move(Level* l, int dir) {
    int n = (int)l->items->size(); if (!n) return;
    int i = l->hover;
    if (i < 0) i = dir > 0 ? -1 : 0;   // nothing selected: Down starts at the first row, Up at the last
    for (int k = 0; k < n; k++) {
        i = (i + dir + n) % n;
        if (!(*l->items)[i].separator && (*l->items)[i].enabled) { l->hover = i; break; }
    }
    InvalidateRect(l->hwnd, nullptr, FALSE);
}

static void key(WPARAM vk) {
    Level* l = deepest(); if (!l) return;
    switch (vk) {
    case VK_DOWN: move(l, 1); break;
    case VK_UP: move(l, -1); break;
    case VK_RIGHT: if (l->hover >= 0) openSub(l, l->hover, true); break;
    case VK_LEFT: if (l->parent) closeChild(l->parent); break;
    case VK_RETURN: case VK_SPACE: activate(l, l->hover); break;
    case VK_ESCAPE: if (l->parent) closeChild(l->parent); else _done = true; break;
    case VK_MENU: case VK_F10: _done = true; break;
    }
}

static int hitTest(Level* l, LPARAM lp) {
    float x = GET_X_LPARAM(lp) / l->scale, y = GET_Y_LPARAM(lp) / l->scale;
    for (int i = 0; i < (int)l->items->size(); i++) { D2D1_RECT_F r = itemRect(l, i); if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i; }
    return -1;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM lp) {
    Level* l = levelOf(h);
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); if (l) paint(l); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return l && l->parent ? MA_NOACTIVATE : MA_ACTIVATE;
    case WM_MOUSEMOVE: {
        if (!l) return 0;
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        int i = hitTest(l, lp);
        if (i != l->hover) {
            l->hover = i;
            if (i >= 0 && !(*l->items)[i].sub.empty()) openSub(l, i, false);
            else if (i >= 0 && l->child) closeChild(l);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0; }
    case WM_MOUSELEAVE: if (l && !l->child) { l->hover = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: if (l) activate(l, hitTest(l, lp)); return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && l == _root) _done = true; return 0;
    case WM_DPICHANGED: return 0;   // laid out for the monitor it opened on; it closes before it could move
    }
    return DefWindowProcW(h, m, w, lp);
}

int show(HWND owner, POINT at, const std::vector<Item>& items, Align align, float minWidth) {
    if (_root || items.empty()) return 0;
    if (!_registered) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FriKeyMenu";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.style = CS_DROPSHADOW;
        RegisterClassW(&wc); _registered = true;
    }
    (void)owner;
    _done = false; _result = 0;
    open(&items, nullptr, -1, at, align, minWidth, RECT{});
    MSG msg; bool quit = false;
    while (!_done) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) { quit = true; break; }
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) { key(msg.wParam); continue; }
        if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP || msg.message == WM_CHAR || msg.message == WM_SYSCHAR) continue;
        bool down = msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN || msg.message == WM_MBUTTONDOWN ||
                    msg.message == WM_NCLBUTTONDOWN || msg.message == WM_NCRBUTTONDOWN;
        if (down && !levelOf(msg.hwnd)) { _done = true; break; }   // a click on another window of this app: close, and eat it
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    Level* l = _root; _root = nullptr;
    closeChild(l); if (l->rt) l->rt->Release(); DestroyWindow(l->hwnd); delete l;
    if (quit) PostQuitMessage((int)msg.wParam);
    return _result;
}

}}
