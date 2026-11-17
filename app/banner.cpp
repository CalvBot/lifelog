// banner.cpp: a small note at the top of the screen when a voice command worked ("Jarvis — Alarm
// set: 4 minutes"). It never takes the focus or a click, so a game keeps both; it shows on the
// screen with the window in front, for 2.5 s, then fades. It has its own thread and window, made
// on first use, and draws itself with GDI: a dark rounded box (its corners smoothed by hand, as a
// layered window's alpha) with the wake word in blue.

struct Banner {
    SRWLOCK lock;       // guards who, text, pending
    wchar_t who[64];
    wchar_t text[160];
    bool pending;       // a note is waiting for the banner thread
    volatile HWND hwnd; // the banner thread's, once made
    HANDLE thread;      // the main thread's
};
static Banner g_banner;

static const UINT BANNER_SHOW = WM_APP + 1;
static const UINT_PTR BANNER_TIMER = 1;
static const u64 BANNER_MS = 2500, BANNER_FADE_MS = 300;
static const COLORREF BANNER_BG = RGB(30, 30, 34), BANNER_WHO = RGB(96, 170, 255), BANNER_TEXT = RGB(240, 240, 240);

// The banner thread's.
static HDC g_banner_dc;          // holds the drawn note while it is up
static HBITMAP g_banner_bitmap;
static SIZE g_banner_size;
static u64 g_banner_shown_ms;

// Draws the note into a 32-bit bitmap, sized to fit, and puts it on the screen at full strength.
static void banner_draw(HWND hwnd) {
    wchar_t who[64], text[160];
    AcquireSRWLockExclusive(&g_banner.lock);
    wcscpy_s(who, g_banner.who);
    wcscpy_s(text, g_banner.text);
    g_banner.pending = false;
    ReleaseSRWLockExclusive(&g_banner.lock);

    // The screen the user is looking at: the one with the window in front.
    HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    // Its DPI: the window takes the monitor's once it is on it.
    SetWindowPos(hwnd, 0, mi.rcMonitor.left, mi.rcMonitor.top, 1, 1, SWP_NOACTIVATE | SWP_NOZORDER);
    i32 dpi = (i32)GetDpiForWindow(hwnd);
    if (dpi <= 0) dpi = 96;
    auto scaled = [dpi](i32 v) { return MulDiv(v, dpi, 96); };

    HFONT bold = CreateFontW(-scaled(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT regular = CreateFontW(-scaled(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    const wchar_t* dash = L"  \x2014  ";

    HDC screen = GetDC(0);
    if (g_banner_dc) {
        DeleteDC(g_banner_dc);
        DeleteObject(g_banner_bitmap);
    }
    g_banner_dc = CreateCompatibleDC(screen);
    SIZE a = {}, b = {}, c = {};
    SelectObject(g_banner_dc, bold);
    GetTextExtentPoint32W(g_banner_dc, who, (int)wcslen(who), &a);
    SelectObject(g_banner_dc, regular);
    GetTextExtentPoint32W(g_banner_dc, dash, (int)wcslen(dash), &b);
    GetTextExtentPoint32W(g_banner_dc, text, (int)wcslen(text), &c);
    i32 pad_x = scaled(20), pad_y = scaled(10);
    i32 w = a.cx + b.cx + c.cx + pad_x * 2, h = a.cy + pad_y * 2;
    i32 most = mi.rcMonitor.right - mi.rcMonitor.left - scaled(40);
    if (w > most) w = most;
    g_banner_size = {w, h};

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    u32* px = 0;
    g_banner_bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&px, 0, 0);
    SelectObject(g_banner_dc, g_banner_bitmap);

    RECT all = {0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(BANNER_BG);
    FillRect(g_banner_dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(g_banner_dc, TRANSPARENT);
    RECT r = {pad_x, pad_y, w - pad_x, h - pad_y};
    SelectObject(g_banner_dc, bold);
    SetTextColor(g_banner_dc, BANNER_WHO);
    DrawTextW(g_banner_dc, who, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    r.left += a.cx;
    SelectObject(g_banner_dc, regular);
    SetTextColor(g_banner_dc, BANNER_TEXT);
    DrawTextW(g_banner_dc, dash, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    r.left += b.cx;
    DrawTextW(g_banner_dc, text, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    GdiFlush();
    DeleteObject(bold);
    DeleteObject(regular);

    // GDI leaves alpha at 0: make the rounded box opaque, its corners smooth, the rest clear
    // (premultiplied, as UpdateLayeredWindow takes it).
    f32 radius = (f32)scaled(10);
    for (i32 y = 0; y < h; y++) {
        for (i32 x = 0; x < w; x++) {
            f32 cx = x + 0.5f, cy = y + 0.5f;
            f32 dx = cx < radius ? radius - cx : cx > w - radius ? cx - (w - radius) : 0;
            f32 dy = cy < radius ? radius - cy : cy > h - radius ? cy - (h - radius) : 0;
            f32 cover = radius + 0.5f - sqrtf(dx * dx + dy * dy);
            cover = cover < 0 ? 0 : cover > 1 ? 1 : cover;
            u32 p = px[y * w + x];
            u32 rr = (u32)(((p >> 16) & 255) * cover), gg = (u32)(((p >> 8) & 255) * cover), bb = (u32)((p & 255) * cover);
            px[y * w + x] = ((u32)(cover * 255) << 24) | (rr << 16) | (gg << 8) | bb;
        }
    }

    POINT at = {mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - w) / 2, mi.rcMonitor.top + scaled(28)};
    POINT zero = {};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, &at, &g_banner_size, g_banner_dc, &zero, 0, &blend, ULW_ALPHA);
    ReleaseDC(0, screen);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_banner_shown_ms = GetTickCount64();
    SetTimer(hwnd, BANNER_TIMER, 16, 0);
}

static LRESULT CALLBACK banner_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case BANNER_SHOW:
        banner_draw(hwnd);
        return 0;
    case WM_TIMER: {
        u64 age = GetTickCount64() - g_banner_shown_ms;
        if (age < BANNER_MS) return 0;
        u64 fade = age - BANNER_MS;
        if (fade >= BANNER_FADE_MS) {
            KillTimer(hwnd, BANNER_TIMER);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)(255 - 255 * fade / BANNER_FADE_MS), AC_SRC_ALPHA};
        POINT zero = {};
        UpdateLayeredWindow(hwnd, 0, 0, &g_banner_size, g_banner_dc, &zero, 0, &blend, ULW_ALPHA);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI banner_main(void*) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = banner_proc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"lifelog.banner";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                                L"lifelog", WS_POPUP, 0, 0, 1, 1, 0, 0, wc.hInstance, 0);
    if (!hwnd) {
        WARN("banner: no window (error %lu)", GetLastError());
        return 0;
    }
    g_banner.hwnd = hwnd;
    // A note sent before the window was there.
    AcquireSRWLockShared(&g_banner.lock);
    bool waiting = g_banner.pending;
    ReleaseSRWLockShared(&g_banner.lock);
    if (waiting) banner_draw(hwnd);
    MSG m;
    while (GetMessageW(&m, 0, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

void banner_show(const char* who, const char* text) {
    Mark m = mark(scratch());
    AcquireSRWLockExclusive(&g_banner.lock);
    wcsncpy_s(g_banner.who, wide(scratch(), str(who)), _TRUNCATE);
    wcsncpy_s(g_banner.text, wide(scratch(), str(text)), _TRUNCATE);
    g_banner.pending = true;
    ReleaseSRWLockExclusive(&g_banner.lock);
    rewind(m);
    if (g_banner.hwnd) {
        PostMessageW(g_banner.hwnd, BANNER_SHOW, 0, 0);
    } else if (!g_banner.thread) {
        g_banner.thread = CreateThread(0, 0, banner_main, 0, 0, 0);
        if (g_banner.thread) SetThreadDescription(g_banner.thread, L"banner");
        else WARN("banner: no thread (error %lu)", GetLastError());
    }
}
