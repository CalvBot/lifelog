// notify.cpp: lifelog's notifications ("toasts"), small windows of its own in the corner of the main
// monitor, drawn with GDI: a tile with an icon beside a title over a few lines, then a row with the
// Dismiss X at the left and the buttons at the right, the first one (the main one) last, in the accent.
// Windows' own toasts are not used: they add Windows' buttons and menu next to ours.
//
// Each toast goes after its own time (TOAST_LIFE_*, a question longer), counted down by a ring round
// the Dismiss X. While the mouse is on a toast the count holds at full, and starts again from full when
// the mouse leaves. A button opens its `lifelog:` link; a click anywhere else opens the toast's own link
// (ToastJob.launch) if it has one, and closes it. Toasts never take the keyboard, and are left out of
// screen captures. Up to TOAST_MAX are up at once, stacked upward; the same tag replaces a toast in place.
//
// The links are also a link type, registered to run lifelog.exe <link>, which passes it on to the
// running app (notify_forward, then notify_open_link) and exits. A test copy (LIFELOG_CONFIG set)
// registers no link type, so it never takes over the installed lifelog's.
//
// The windows belong to the notify thread, which also owns the window links are sent to; the other
// threads only queue toasts, and a click is posted to the main thread as an event.

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")

static const wchar_t* NOTIFY_CLASS = L"lifelog.notify";
static const wchar_t* TOAST_CLASS = L"lifelog.toast";
static const ULONG_PTR LINK_MAGIC = 0x6C696E6B;  // "link", in WM_COPYDATA's dwData
enum : UINT { WM_NOTIFY_QUEUE = WM_APP + 40 };

static bool notify_test_mode() { return GetEnvironmentVariableW(L"LIFELOG_CONFIG", 0, 0) != 0; }
static const char* link_scheme() { return notify_test_mode() ? "lifelog-test" : "lifelog"; }

// "lifelog:<path>" into `out`, `path` given printf-style.
static void make_link(char* out, i32 cap, const char* fmt, ...) {
    i32 n = snprintf(out, cap, "%s:", link_scheme());
    va_list args;
    va_start(args, fmt);
    vsnprintf(out + n, cap - n, fmt, args);
    va_end(args);
}

static void toast_button(ToastJob* t, const char* text, const char* link) {
    CHECK(t->button_count < COUNT(t->buttons), "at most %d buttons on a toast", COUNT(t->buttons));
    ToastButton* b = &t->buttons[t->button_count++];
    SET(b->text, str(text));
    SET(b->link, str(link));
}

// Whether Windows' apps are set to dark (Settings > Personalisation > Colours): the toasts and the
// window (main.cpp) follow it.
static bool system_dark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, 0, &light, &size);
    return light == 0;
}

// ---------------------------------------------------------------------------------------------
// Queueing (any thread)

static void notify_queue(App* app, ToastJob* job) {
    Notify* n = &app->summary.notify;
    AcquireSRWLockExclusive(&n->lock);
    list_push(&n->queue, *job);
    HWND w = n->window;
    ReleaseSRWLockExclusive(&n->lock);
    if (w) PostMessageW(w, WM_NOTIFY_QUEUE, 0, 0);
}

void notify_show(App* app, ToastJob* toast) {
    toast->remove = false;
    notify_queue(app, toast);
}

void notify_remove(App* app, const char* tag) {
    ToastJob job = {};
    job.remove = true;
    SET(job.tag, str(tag));
    notify_queue(app, &job);
}

// ---------------------------------------------------------------------------------------------
// Registration

static bool reg_string(const wchar_t* key, const wchar_t* name, const wchar_t* value) {
    LSTATUS rc = RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    if (rc != ERROR_SUCCESS) WARN("could not write the registry key HKCU\\%ls (error %ld)", key, (long)rc);
    return rc == ERROR_SUCCESS;
}

// Registers the `lifelog:` link type. Safe to repeat; done at every start so the path follows the exe.
static void notify_register() {
    if (notify_test_mode()) {
        LOG("notifications: test copy, the lifelog: link type is left as it is");
        return;
    }
    wchar_t exe[MAX_PATH], value[MAX_PATH + 16];
    GetModuleFileNameW(0, exe, MAX_PATH);
    reg_string(L"Software\\Classes\\lifelog", 0, L"URL:lifelog");
    reg_string(L"Software\\Classes\\lifelog", L"URL Protocol", L"");
    swprintf(value, COUNT(value), L"\"%ls\" \"%%1\"", exe);
    reg_string(L"Software\\Classes\\lifelog\\shell\\open\\command", 0, value);
}

// ---------------------------------------------------------------------------------------------
// The toast windows (the notify thread)

// Sizes at 100%.
static const f32 TOAST_W = 420, TOAST_PAD = 16, TOAST_GAP = 12, TOAST_MARGIN = 12, TOAST_TILE = 36, TOAST_ROUND = 8,
                 TOAST_BUTTON_H = 30, TOAST_BUTTON_GAP = 8, TOAST_BUTTON_PAD = 14, TOAST_BUTTON_MIN = 72,
                 TOAST_BUTTON_ROUND = 6, TOAST_DIAL = 24, TOAST_DIAL_RING = 2;
static const i32 TOAST_BODY_LINES = 3;  // more is cut short with "…"
// How long each shows once its countdown starts: a question longest, a failure next.
static const u32 TOAST_LIFE_QUESTION = 30000, TOAST_LIFE_FAILED = 12000, TOAST_LIFE_INFO = 8000;
static const UINT_PTR TOAST_TICK = 1;
static const UINT TOAST_TICK_MS = 33;  // the dial moves at 30 fps
static const i32 TOAST_SS = 4;         // shapes are drawn this many times the size and scaled down, for smooth edges
static const i32 TOAST_MAX = 4;
// The parts a click lands on: the buttons (0..3), Dismiss, or the rest.
enum { TP_NONE = -1, TP_DISMISS = 10, TP_BODY = 11 };

// The colours, 0xRRGGBB, as the window's theme (main.cpp's style_init).
struct ToastPalette {
    u32 panel, button, hover, line, text, dim, accent, accent_hover, ink, red;
};
static ToastPalette toast_palette(bool dark) {
    if (dark) return {0x262626, 0x333333, 0x424242, 0x404040, 0xdbdbdb, 0x8c8c8c, 0x0078d4, 0x268cdb, 0xffffff, 0xc42b1c};
    return {0xffffff, 0xe8e8e8, 0xdbdbdb, 0xd9d9d9, 0x1a1a1a, 0x737373, 0x0078d4, 0x1a86d9, 0xffffff, 0xc42b1c};
}

struct ToastFonts {
    f32 scale;
    HFONT title, body, strong, icons, small_icons;
};

struct Toast {
    HWND hwnd;
    ToastJob job;
    wchar_t title[256], body[1024], buttons[4][64];
    bool open, tracking, held;  // held: the mouse is on it, so the countdown waits
    i32 hot;                    // the part under the mouse
    u64 start_ms;               // the countdown's start (GetTickCount64)
    u32 life_ms;
    u64 shown_ms;               // when it was shown, so a full stack replaces the oldest
    // The layout, in pixels at `scale`
    f32 scale;
    i32 w, h, title_h, body_h;
    RECT tile, text, button[4], dismiss, dial;
    HBITMAP base;  // the toast drawn without its dial; redrawn when dirty
    i32 base_w, base_h;
    bool dirty;
};

static Toast g_toasts[TOAST_MAX];
static ToastFonts g_toast_fonts;
static ToastPalette g_toast_colours;
static App* g_toast_app;

static COLORREF toast_rgb(u32 c) { return RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff); }
static i32 toast_px(f32 scale, f32 v) { return (i32)(v * scale + 0.5f); }

static void toast_fonts(f32 s) {
    ToastFonts* f = &g_toast_fonts;
    if (f->title && f->scale == s) return;
    if (f->title) {
        DeleteObject(f->title);
        DeleteObject(f->body);
        DeleteObject(f->strong);
        DeleteObject(f->icons);
        DeleteObject(f->small_icons);
    }
    f->scale = s;
    auto make = [&](f32 px, i32 weight, const wchar_t* face) {
        return CreateFontW(-(i32)(px * s + 0.5f), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    f->title = make(16, FW_SEMIBOLD, L"Segoe UI");
    f->body = make(14, FW_NORMAL, L"Segoe UI");
    f->strong = make(14, FW_SEMIBOLD, L"Segoe UI");
    f->icons = make(18, FW_NORMAL, L"Segoe MDL2 Assets");
    f->small_icons = make(12, FW_NORMAL, L"Segoe MDL2 Assets");
}

static i32 text_width(HDC dc, HFONT font, const wchar_t* s) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, s, (i32)wcslen(s), &size);
    SelectObject(dc, old);
    return size.cx;
}

// The height `s` takes wrapped to `width`.
static i32 text_height(HDC dc, HFONT font, const wchar_t* s, i32 width) {
    HGDIOBJ old = SelectObject(dc, font);
    RECT r = {0, 0, width, 0};
    DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom;
}

// Cuts `s` (in place) to what fits in `lines` lines of `width`, ending it with "…" when cut.
static void text_fit(HDC dc, HFONT font, wchar_t* s, i32 width, i32 lines) {
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    i32 most = lines * tm.tmHeight;
    if (text_height(dc, font, s, width) <= most) return;
    wchar_t cut[1024];
    i32 lo = 0, hi = (i32)wcslen(s);
    if (hi > COUNT(cut) - 2) hi = COUNT(cut) - 2;
    while (lo < hi) {  // the longest start that fits with "…" after it
        i32 mid = (lo + hi + 1) / 2;
        memcpy(cut, s, mid * sizeof(wchar_t));
        cut[mid] = 0x2026;
        cut[mid + 1] = 0;
        if (text_height(dc, font, cut, width) <= most) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && s[lo - 1] == ' ') lo--;
    s[lo] = 0x2026;
    s[lo + 1] = 0;
}

// The toast's size and its parts, from its text, at `scale`.
static void toast_layout(Toast* t, HDC dc, f32 scale) {
    toast_fonts(scale);
    ToastFonts* f = &g_toast_fonts;
    auto px = [&](f32 v) { return toast_px(scale, v); };
    t->scale = scale;
    i32 pad = px(TOAST_PAD), tile = px(TOAST_TILE), gap = px(TOAST_GAP);
    // As wide as TOAST_W, or wider when the buttons need it: the X, then the buttons with their gaps
    i32 widths[COUNT(t->buttons)] = {}, need = 2 * pad + px(TOAST_BUTTON_H) + px(TOAST_BUTTON_GAP);
    for (i32 i = 0; i < t->job.button_count; i++) {
        widths[i] = text_width(dc, i == 0 ? f->strong : f->body, t->buttons[i]) + 2 * px(TOAST_BUTTON_PAD);
        if (widths[i] < px(TOAST_BUTTON_MIN)) widths[i] = px(TOAST_BUTTON_MIN);
        need += widths[i] + (i ? px(TOAST_BUTTON_GAP) : 0);
    }
    t->w = need > px(TOAST_W) ? need : px(TOAST_W);
    t->tile = {pad, pad, pad + tile, pad + tile};
    i32 tx = pad + tile + gap, right = t->w - pad;
    t->title_h = text_height(dc, f->title, L"Ag", right - tx);
    text_fit(dc, f->body, t->body, right - tx, TOAST_BODY_LINES);
    t->body_h = t->body[0] ? text_height(dc, f->body, t->body, right - tx) : 0;
    i32 text_h = t->title_h + (t->body_h ? px(2) + t->body_h : 0);
    i32 top = pad, content = text_h > tile ? text_h : tile;
    // The title and its lines, centred on the tile when shorter than it
    i32 ty = text_h < tile ? top + (tile - text_h) / 2 : top;
    t->text = {tx, ty, right, ty + text_h};
    i32 row = top + content + gap, bh = px(TOAST_BUTTON_H);
    t->h = row + bh + pad;
    t->dismiss = {pad, row, pad + bh, row + bh};
    i32 d = px(TOAST_DIAL);
    t->dial = {pad + (bh - d) / 2, row + (bh - d) / 2, pad + (bh - d) / 2 + d, row + (bh - d) / 2 + d};
    // The buttons from the right: the main one (the first), then the rest in their order to its left
    i32 x = right;
    for (i32 k = 0; k < t->job.button_count; k++) {
        i32 i = k == 0 ? 0 : t->job.button_count - k;
        t->button[i] = {x - widths[i], row, x, row + bh};
        x -= widths[i] + px(TOAST_BUTTON_GAP);
    }
}

static i32 toast_hit(Toast* t, POINT p) {
    for (i32 i = 0; i < t->job.button_count; i++)
        if (PtInRect(&t->button[i], p)) return i;
    if (PtInRect(&t->dismiss, p)) return TP_DISMISS;
    return TP_BODY;
}

// The Dismiss X, centred in r.
static void toast_draw_x(Toast* t, HDC dc, RECT r) {
    HGDIOBJ old = SelectObject(dc, g_toast_fonts.small_icons);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, toast_rgb(t->hot == TP_DISMISS ? g_toast_colours.text : g_toast_colours.dim));
    const wchar_t cancel[] = {(wchar_t)0xE711, 0};  // Segoe MDL2: Cancel
    DrawTextW(dc, cancel, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

// The tile's glyph and colour for the toast's icon.
static wchar_t toast_glyph(ToastIcon icon) {
    // Segoe MDL2: Calendar, Ringer, Error, Microphone
    return icon == TOAST_ICON_REMINDER ? 0xEA8F : icon == TOAST_ICON_ERROR ? 0xE783 : icon == TOAST_ICON_VOICE ? 0xE720 : 0xE787;
}

// The toast without its dial into dc (w x h): the shapes at TOAST_SS times the size scaled down, then
// the text and the icons at their real size.
static void toast_draw(Toast* t, HDC wdc, HDC dc) {
    const i32 SS = TOAST_SS;
    const ToastPalette* c = &g_toast_colours;
    ToastFonts* f = &g_toast_fonts;
    i32 w = t->w, h = t->h;
    HDC big = CreateCompatibleDC(wdc);
    HBITMAP big_bmp = CreateCompatibleBitmap(wdc, w * SS, h * SS);
    HGDIOBJ old_big = SelectObject(big, big_bmp);
    auto brush = [&](u32 rgb) { DeleteObject(SelectObject(big, CreateSolidBrush(toast_rgb(rgb)))); };
    auto rounded = [&](RECT q, f32 radius) {
        i32 d = toast_px(t->scale, 2 * radius) * SS;
        RoundRect(big, q.left * SS, q.top * SS, q.right * SS + 1, q.bottom * SS + 1, d, d);
    };

    RECT all = {0, 0, w * SS, h * SS};
    HBRUSH bg = CreateSolidBrush(toast_rgb(c->panel));
    FillRect(big, &all, bg);
    DeleteObject(bg);
    HGDIOBJ old_pen = SelectObject(big, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(big, CreateSolidBrush(0));
    brush(t->job.icon == TOAST_ICON_ERROR ? c->red : c->accent);
    rounded(t->tile, TOAST_ROUND);
    for (i32 i = 0; i < t->job.button_count; i++) {
        if (i == 0) brush(t->hot == 0 ? c->accent_hover : c->accent);
        else brush(t->hot == i ? c->hover : c->button);
        rounded(t->button[i], TOAST_BUTTON_ROUND);
    }
    if (t->hot == TP_DISMISS) {  // a ghost button: only its X until the mouse is on it
        brush(c->hover);
        rounded(t->dismiss, TOAST_BUTTON_ROUND);
    }
    DeleteObject(SelectObject(big, old_brush));
    SelectObject(big, old_pen);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, 0);
    StretchBlt(dc, 0, 0, w, h, big, 0, 0, w * SS, h * SS, SRCCOPY);
    SelectObject(big, old_big);
    DeleteObject(big_bmp);
    DeleteDC(big);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, f->icons);
    SetTextColor(dc, toast_rgb(c->ink));
    wchar_t glyph[] = {toast_glyph(t->job.icon), 0};
    DrawTextW(dc, glyph, 1, &t->tile, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, f->title);
    SetTextColor(dc, toast_rgb(c->text));
    RECT r1 = {t->text.left, t->text.top, t->text.right, t->text.top + t->title_h};
    DrawTextW(dc, t->title, -1, &r1, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (t->body_h) {
        SelectObject(dc, f->body);
        SetTextColor(dc, toast_rgb(c->dim));
        RECT r2 = {t->text.left, r1.bottom + toast_px(t->scale, 2), t->text.right, t->text.bottom};
        DrawTextW(dc, t->body, -1, &r2, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
    for (i32 i = 0; i < t->job.button_count; i++) {
        SelectObject(dc, i == 0 ? f->strong : f->body);
        SetTextColor(dc, toast_rgb(i == 0 ? c->ink : c->text));
        DrawTextW(dc, t->buttons[i], -1, &t->button[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old_font);
}

// The countdown dial onto wdc: a thin ring in the accent round the Dismiss X, which empties clockwise
// from the top over the toast's life, full while the mouse is on it. Round it and inside it is the
// toast as drawn in `under`.
static void toast_draw_dial(Toast* t, HDC wdc, HDC under) {
    const i32 SS = TOAST_SS;
    const ToastPalette* c = &g_toast_colours;
    RECT d = t->dial;
    i32 w = d.right - d.left, h = d.bottom - d.top, W = w * SS, H = h * SS;
    f32 left = t->held || !t->life_ms ? 1 : 1 - (f32)(GetTickCount64() - t->start_ms) / t->life_ms;
    left = left < 0 ? 0 : left;
    HDC big = CreateCompatibleDC(wdc), dc = CreateCompatibleDC(wdc);
    HBITMAP big_bmp = CreateCompatibleBitmap(wdc, W, H), bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old_big = SelectObject(big, big_bmp), old_bmp = SelectObject(dc, bmp);
    SetStretchBltMode(big, COLORONCOLOR);
    StretchBlt(big, 0, 0, W, H, under, d.left, d.top, w, h, SRCCOPY);
    HGDIOBJ old_pen = SelectObject(big, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(big, CreateSolidBrush(toast_rgb(c->button)));
    Ellipse(big, 0, 0, W + 1, H + 1);  // the track
    DeleteObject(SelectObject(big, CreateSolidBrush(toast_rgb(c->accent))));
    if (left >= 0.999f) {
        Ellipse(big, 0, 0, W + 1, H + 1);
    } else if (left > 0) {  // a pie from the top round to where the time left ends (GDI's pies run anticlockwise)
        f32 a = 3.14159265f * (0.5f - 2 * left), cx = W / 2.f, cy = H / 2.f;
        Pie(big, 0, 0, W + 1, H + 1, (i32)(cx + cx * cosf(a)), (i32)(cy - cy * sinf(a)), (i32)cx, 0);
    }
    i32 ring = toast_px(t->scale, TOAST_DIAL_RING) * SS;  // hollowed out to a ring: the toast again inside it
    HRGN inside = CreateEllipticRgn(ring, ring, W - ring + 1, H - ring + 1);
    SelectClipRgn(big, inside);
    StretchBlt(big, 0, 0, W, H, under, d.left, d.top, w, h, SRCCOPY);
    SelectClipRgn(big, 0);
    DeleteObject(inside);
    DeleteObject(SelectObject(big, old_brush));
    SelectObject(big, old_pen);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, 0);
    StretchBlt(dc, 0, 0, w, h, big, 0, 0, W, H, SRCCOPY);
    RECT x = {0, 0, w, h};
    toast_draw_x(t, dc, x);
    BitBlt(wdc, d.left, d.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(big, old_big);
    SelectObject(dc, old_bmp);
    DeleteObject(big_bmp);
    DeleteObject(bmp);
    DeleteDC(big);
    DeleteDC(dc);
}

// The toast is kept drawn in `base`, so the dial's ticks only copy it out and draw the dial.
static void toast_paint(Toast* t) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(t->hwnd, &ps);
    if (t->base && (t->base_w != t->w || t->base_h != t->h)) {
        DeleteObject(t->base);
        t->base = 0;
    }
    if (!t->base) {
        t->base = CreateCompatibleBitmap(wdc, t->w, t->h);
        t->base_w = t->w;
        t->base_h = t->h;
        t->dirty = true;
    }
    HDC dc = CreateCompatibleDC(wdc);
    HGDIOBJ old = SelectObject(dc, t->base);
    if (t->dirty) {
        toast_draw(t, wdc, dc);
        t->dirty = false;
    }
    ExcludeClipRect(wdc, t->dial.left, t->dial.top, t->dial.right, t->dial.bottom);
    BitBlt(wdc, 0, 0, t->w, t->h, dc, 0, 0, SRCCOPY);
    SelectClipRgn(wdc, 0);
    toast_draw_dial(t, wdc, dc);
    SelectObject(dc, old);
    DeleteDC(dc);
    EndPaint(t->hwnd, &ps);
}

static void toast_redraw(Toast* t) {
    t->dirty = true;
    InvalidateRect(t->hwnd, 0, FALSE);
}

static void toast_hide(Toast* t) {
    t->open = false;
    if (!t->hwnd) return;
    KillTimer(t->hwnd, TOAST_TICK);
    ShowWindow(t->hwnd, SW_HIDE);
}

// A link from a toast, for the main thread to act on.
static void notify_link_apply(App* app, Event* e);
static void toast_open(const char* link) {
    i32 n = (i32)strlen(link);
    if (!n || !g_toast_app) return;
    LOG("toast: %s", link);
    event_post(g_toast_app, notify_link_apply, link, n + 1);
}

static LRESULT CALLBACK toast_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Toast* t = (Toast*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!t) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // the window in front keeps the keyboard
    case WM_PAINT: toast_paint(t); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(0, (LPCWSTR)IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!t->tracking) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            t->tracking = true;
        }
        POINT p = {(i16)LOWORD(lp), (i16)HIWORD(lp)};
        i32 hot = toast_hit(t, p);
        if (hot == TP_BODY) hot = TP_NONE;
        if (hot != t->hot) {
            t->hot = hot;
            toast_redraw(t);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        t->tracking = false;
        t->hot = TP_NONE;
        toast_redraw(t);
        return 0;
    case WM_LBUTTONUP: {
        POINT p = {(i16)LOWORD(lp), (i16)HIWORD(lp)};
        i32 k = toast_hit(t, p);
        toast_hide(t);
        if (k >= 0 && k < t->job.button_count) toast_open(t->job.buttons[k].link);
        else if (k == TP_BODY) toast_open(t->job.launch);
        return 0;
    }
    case WM_TIMER: {  // the countdown's tick: held at full while the mouse is on it, from full again once it leaves
        POINT p;
        RECT r;
        GetCursorPos(&p);
        GetWindowRect(hwnd, &r);
        u64 now = GetTickCount64();
        bool on = PtInRect(&r, p) != 0;
        if (on || t->held) t->start_ms = now;
        if (on != t->held) t->held = on;
        else if (on) return 0;  // still held: the dial is already full
        if (now - t->start_ms >= t->life_ms) {
            toast_hide(t);
            return 0;
        }
        InvalidateRect(hwnd, &t->dial, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool toast_window(Toast* t) {
    if (t->hwnd) return true;
    HINSTANCE inst = GetModuleHandleW(0);
    static bool registered;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = toast_proc;
        wc.hInstance = inst;
        wc.lpszClassName = TOAST_CLASS;
        RegisterClassW(&wc);
        registered = true;
    }
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    t->hwnd = CreateWindowExW(ex, TOAST_CLASS, L"lifelog", WS_POPUP, 0, 0, 1, 1, 0, 0, inst, 0);
    if (!t->hwnd) {
        WARN("notifications: no toast window (error %lu)", GetLastError());
        return false;
    }
    SetWindowLongPtrW(t->hwnd, GWLP_USERDATA, (LONG_PTR)t);
    // Left out of screen captures and recordings; Windows before 10 2004 refuse it and show it as before.
    SetWindowDisplayAffinity(t->hwnd, 0x11 /* WDA_EXCLUDEFROMCAPTURE */);
    // Corners rounded by Windows 11, which smooths them; Windows 10 leaves them square.
    i32 round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(t->hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    return true;
}

// Where a w x h toast goes: the bottom right of `work`, `m` from its edges, above the toasts already up.
static POINT toast_spot(RECT work, i32 w, i32 h, i32 m, Toast* self) {
    i32 x = work.right - w - m, y = work.bottom - m - h;
    for (bool moved = true; moved;) {  // again after a move, as it may then be in another's way
        moved = false;
        for (i32 i = 0; i < TOAST_MAX; i++) {
            Toast* o = &g_toasts[i];
            RECT r;
            if (o == self || !o->open || !GetWindowRect(o->hwnd, &r)) continue;
            if (r.top - m < y + h && r.bottom + m > y) {
                y = r.top - m - h;
                moved = true;
            }
        }
    }
    return {x, y};
}

// Shows `job` in a toast: the one with its tag, else a free one, else the oldest.
static void toast_show(ToastJob* job) {
    Toast* t = 0;
    for (i32 i = 0; i < TOAST_MAX && !t; i++)
        if (g_toasts[i].open && job->tag[0] && strcmp(g_toasts[i].job.tag, job->tag) == 0) t = &g_toasts[i];
    bool again = t != 0;  // in place, where it is
    for (i32 i = 0; i < TOAST_MAX && !t; i++)
        if (!g_toasts[i].open) t = &g_toasts[i];
    for (i32 i = 0; i < TOAST_MAX && !t; i++)
        if (!t || g_toasts[i].shown_ms < t->shown_ms) t = &g_toasts[i];
    if (!t) t = &g_toasts[0];
    if (!toast_window(t)) return;
    if (t->open && !again) toast_hide(t);

    t->job = *job;
    Mark m = mark(scratch());
    swprintf(t->title, COUNT(t->title), L"%ls", wide(scratch(), str(job->title)));
    swprintf(t->body, COUNT(t->body), L"%ls", wide(scratch(), str(job->body)));
    for (i32 i = 0; i < job->button_count; i++)
        swprintf(t->buttons[i], COUNT(t->buttons[i]), L"%ls", wide(scratch(), str(job->buttons[i].text)));
    rewind(m);
    g_toast_colours = toast_palette(system_dark());
    COLORREF edge = toast_rgb(g_toast_colours.line);
    DwmSetWindowAttribute(t->hwnd, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof(edge));

    // The main monitor's work area, at its scale: the window is moved there first so its DPI is that monitor's.
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT work = mi.rcWork;
    if (!again) SetWindowPos(t->hwnd, HWND_TOPMOST, work.right - 2, work.bottom - 2, 1, 1, SWP_NOACTIVATE);
    f32 scale = GetDpiForWindow(t->hwnd) / 96.f;
    HDC dc = GetDC(t->hwnd);
    toast_layout(t, dc, scale);
    ReleaseDC(t->hwnd, dc);
    POINT at;
    RECT now;
    if (again && GetWindowRect(t->hwnd, &now)) at = {now.left, now.bottom - t->h};  // grows upward in place
    else at = toast_spot(work, t->w, t->h, toast_px(scale, TOAST_MARGIN), t);
    SetWindowPos(t->hwnd, HWND_TOPMOST, at.x, at.y, t->w, t->h, SWP_NOACTIVATE);
    t->hot = TP_NONE;
    t->open = true;
    t->shown_ms = GetTickCount64();
    toast_redraw(t);
    ShowWindow(t->hwnd, SW_SHOWNOACTIVATE);
    t->life_ms = job->question ? TOAST_LIFE_QUESTION : job->icon == TOAST_ICON_ERROR ? TOAST_LIFE_FAILED : TOAST_LIFE_INFO;
    t->start_ms = GetTickCount64();
    t->held = false;
    SetTimer(t->hwnd, TOAST_TICK, TOAST_TICK_MS, 0);
}

static void toast_remove(const char* tag) {
    for (i32 i = 0; i < TOAST_MAX; i++)
        if (g_toasts[i].open && strcmp(g_toasts[i].job.tag, tag) == 0) toast_hide(&g_toasts[i]);
}

static void notify_drain(App* app) {
    Notify* n = &app->summary.notify;
    for (;;) {
        AcquireSRWLockExclusive(&n->lock);
        bool any = n->queue.count > 0;
        ToastJob job = {};
        if (any) {
            job = n->queue.items[0];
            list_remove(&n->queue, 0);
        }
        ReleaseSRWLockExclusive(&n->lock);
        if (!any) return;
        if (job.remove) toast_remove(job.tag);
        else toast_show(&job);
    }
}

// Every kind of toast, drawn into one picture at `path` (a .bmp), dark above light, for checking how
// they look without showing them (`lifelog toast-preview FILE`).
bool notify_preview(const wchar_t* path, f32 scale) {
    ToastJob jobs[4] = {};
    SET(jobs[0].title, str("Your summary starts in 5 minutes"));
    SET(jobs[0].body, str("It uses the graphics card for a few minutes. Today: 2 h 10 min of you talking, 3 h 5 min from apps."));
    const char* ask[] = {"Start now", "In an hour", "Delay until…", "Skip today"};
    for (i32 i = 0; i < 4; i++) toast_button(&jobs[0], ask[i], "");
    jobs[0].question = true;
    jobs[1].icon = TOAST_ICON_REMINDER;
    SET(jobs[1].title, str("Remind you tomorrow at 9:00?"));
    SET(jobs[1].body, str("Call the dentist. You said: “I need to call the dentist tomorrow”"));
    toast_button(&jobs[1], "Remind me", "");
    toast_button(&jobs[1], "No thanks", "");
    SET(jobs[2].title, str("Your day, summed up"));
    SET(jobs[2].body, str("1 reminder. Open it when you're ready."));
    toast_button(&jobs[2], "Read it", "");
    jobs[3].icon = TOAST_ICON_ERROR;
    SET(jobs[3].title, str("The summary could not be written"));
    SET(jobs[3].body, str("nothing was transcribed that day"));
    toast_button(&jobs[3], "Try again", "");

    HDC screen = GetDC(0);
    static Toast toasts[COUNT(jobs)];  // only the CLI draws these, once
    i32 m = toast_px(scale, TOAST_MARGIN), total_h = m, w = 0;
    for (i32 i = 0; i < COUNT(jobs); i++) {
        Toast* t = &toasts[i];
        t->job = jobs[i];
        Mark mk = mark(scratch());
        swprintf(t->title, COUNT(t->title), L"%ls", wide(scratch(), str(jobs[i].title)));
        swprintf(t->body, COUNT(t->body), L"%ls", wide(scratch(), str(jobs[i].body)));
        for (i32 k = 0; k < jobs[i].button_count; k++)
            swprintf(t->buttons[k], COUNT(t->buttons[k]), L"%ls", wide(scratch(), str(jobs[i].buttons[k].text)));
        rewind(mk);
        t->hot = TP_NONE;
        t->held = true;
        toast_layout(t, screen, scale);
        total_h += t->h + m;
        if (t->w + 2 * m > w) w = t->w + 2 * m;
    }
    i32 W = 2 * w, H = total_h;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(bi.bmiHeader), W, -H, 1, 32, BI_RGB};
    u32* px = 0;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&px, 0, 0);
    bool ok = bmp && px;
    if (ok) {
        HDC dc = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT all = {0, 0, W, H};
        HBRUSH grey = CreateSolidBrush(RGB(128, 128, 128));
        FillRect(dc, &all, grey);
        DeleteObject(grey);
        for (i32 dark = 1; dark >= 0; dark--) {
            g_toast_colours = toast_palette(dark != 0);
            i32 y = m;
            for (i32 i = 0; i < COUNT(jobs); i++) {
                Toast* t = &toasts[i];
                HDC tdc = CreateCompatibleDC(screen);
                HBITMAP tb = CreateCompatibleBitmap(screen, t->w, t->h);
                HGDIOBJ told = SelectObject(tdc, tb);
                toast_draw(t, screen, tdc);
                toast_draw_dial(t, tdc, tdc);
                BitBlt(dc, (dark ? 0 : w) + m, y, t->w, t->h, tdc, 0, 0, SRCCOPY);
                SelectObject(tdc, told);
                DeleteObject(tb);
                DeleteDC(tdc);
                y += t->h + m;
            }
        }
        GdiFlush();
        SelectObject(dc, old);
        DeleteDC(dc);
        BITMAPFILEHEADER fh = {};
        DWORD bytes = (DWORD)W * H * 4;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
        fh.bfSize = fh.bfOffBits + bytes;
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        DWORD wrote = 0;
        ok = f != INVALID_HANDLE_VALUE && WriteFile(f, &fh, sizeof(fh), &wrote, 0) &&
             WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wrote, 0) && WriteFile(f, px, bytes, &wrote, 0);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    if (bmp) DeleteObject(bmp);
    ReleaseDC(0, screen);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Links

// Only a few fixed actions exist, and the query may only hold a date or an id.
// Either scheme is taken, whichever copy registered it: the link only picks what to show.
bool notify_open_link(App* app, Str link) {
    Str rest = {};
    if (str_starts(link, str("lifelog:"))) rest = str_sub(link, 8);
    else if (str_starts(link, str("lifelog-test:"))) rest = str_sub(link, 13);
    else {
        WARN("not a lifelog: link: %.*s", (int)link.len, link.ptr);
        return false;
    }
    while (rest.len && rest.ptr[0] == '/') rest = str_sub(rest, 1);
    i64 q = str_find(rest, str("?"));
    Str path = q < 0 ? rest : str_sub(rest, 0, q);
    Str query = q < 0 ? Str{"", 0} : str_sub(rest, q + 1);
    while (path.len && path.ptr[path.len - 1] == '/') path.len--;
    for (i64 i = 0; i < query.len; i++) {
        char c = query.ptr[i];
        if (!isalnum((u8)c) && c != '=' && c != '&' && c != '-') {
            WARN("unexpected characters in %.*s", (int)link.len, link.ptr);
            return false;
        }
    }
    Mark m = mark(scratch());
    Str day_s = {}, id_s = {};
    query_param(scratch(), query, "day", &day_s);
    query_param(scratch(), query, "id", &id_s);
    char day[16], id[24];
    SET(day, day_s);
    SET(id, id_s);
    bool ok = true;
    if (str_eq(path, str("summary/open"))) {
        ui_show(app, TAB_JOURNAL, day);
    } else if (str_eq(path, str("summary/pick"))) {
        ui_popup(app, POPUP_DELAY, day);
    } else if (str_eq(path, str("summary/now")) || str_eq(path, str("summary/later")) || str_eq(path, str("summary/skip"))) {
        Str action = str_copy(scratch(), str_sub(path, 8));
        char text[64];
        LOG("summary link %s: %s", action.ptr, summary_acted_text(summary_act(app, action.ptr, day, 0), text, sizeof(text)));
    } else if (str_eq(path, str("remind/yes")) || str_eq(path, str("remind/no")) || str_eq(path, str("remind/done")) ||
               str_eq(path, str("remind/snooze"))) {
        Str action = str_copy(scratch(), str_sub(path, 7));
        reminder_answer(app, _strtoi64(id, 0, 10), action.ptr);
    } else {
        WARN("unknown link %.*s", (int)link.len, link.ptr);
        ok = false;
    }
    rewind(m);
    return ok;
}

bool notify_forward(const wchar_t* link) {
    HWND w = FindWindowExW(HWND_MESSAGE, 0, NOTIFY_CLASS, 0);
    if (!w) {
        WARN("a link was opened but lifelog is not running");
        return false;
    }
    // The running app opens a window for some links; let it come to the front, as this click would have.
    AllowSetForegroundWindow(ASFW_ANY);
    Mark m = mark(scratch());
    Str utf8 = narrow(scratch(), link);
    COPYDATASTRUCT data = {LINK_MAGIC, (DWORD)utf8.len, (void*)utf8.ptr};
    DWORD_PTR result = 0;
    bool ok = SendMessageTimeoutW(w, WM_COPYDATA, 0, (LPARAM)&data, SMTO_ABORTIFHUNG, 10000, &result) && result;
    rewind(m);
    return ok;
}

static LRESULT CALLBACK notify_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (app && msg == WM_NOTIFY_QUEUE) {
        notify_drain(app);
        return 0;
    }
    if (app && msg == WM_COPYDATA) {
        COPYDATASTRUCT* data = (COPYDATASTRUCT*)lp;
        if (data->dwData != LINK_MAGIC) return FALSE;
        if (data->cbData > EVENT_BYTES - 1) {  // it travels as an event record; ours are under 160 bytes
            WARN("link of %lu bytes ignored: too long", data->cbData);
            return FALSE;
        }
        char link[EVENT_BYTES];
        memcpy(link, data->lpData, data->cbData);
        link[data->cbData] = 0;
        LOG("link: %s", link);
        return event_post(app, notify_link_apply, link, data->cbData + 1);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The main thread: a link from a toast's button or a second copy of the program.
static void notify_link_apply(App* app, Event* e) {
    notify_open_link(app, str((const char*)e->data));
}

static DWORD WINAPI notify_main(void* param) {
    App* app = (App*)param;
    g_toast_app = app;
    notify_register();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = notify_proc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = NOTIFY_CLASS;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, NOTIFY_CLASS, L"lifelog", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, wc.hInstance, 0);
    CHECK(hwnd, "the notifications' message window (error %lu)", GetLastError());
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
    // Links come from the lifelog.exe the shell starts, which runs at the same level.
    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, 0);
    Notify* n = &app->summary.notify;
    AcquireSRWLockExclusive(&n->lock);
    n->window = hwnd;
    ReleaseSRWLockExclusive(&n->lock);
    notify_drain(app);  // what was queued before the window existed

    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &app->stop, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
    AcquireSRWLockExclusive(&n->lock);
    n->window = 0;
    ReleaseSRWLockExclusive(&n->lock);
    for (i32 i = 0; i < TOAST_MAX; i++)
        if (g_toasts[i].hwnd) DestroyWindow(g_toasts[i].hwnd);
    DestroyWindow(hwnd);
    return 0;
}
