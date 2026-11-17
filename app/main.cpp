// main.cpp: the program's start, the main thread's message loop, and the windows.
//
// The main thread owns every window: the tray icon's hidden window, the main window while it is
// open, and the tray menu. Each window that shows ImGui is a Surface with its own ImGui context
// and swap chain; they share one D3D11 device. A Surface draws only when something happened:
// input, a wake_window() from another thread, or a frame ui.cpp asked for (an animation).

static App g_app;

enum : UINT {
    WM_APP_WAKE = WM_APP + 1,  // a background thread changed something the window shows
    WM_APP_SHOW,               // App.ui.want_* says what to open
    WM_APP_TRAY,               // the tray icon's messages
    WM_APP_QUIT,
};

static const UINT TRAY_ID = 1;
static const UINT TRAY_OPEN_WINDOW = WM_APP + 0x100;  // our own WM_APP_TRAY code: open the window
static const UINT_PTR REDRAW_TIMER = 1;

// ---------------------------------------------------------------------------------------------
// App helpers (app.h)

// The main thread is in a frame or applying events: events that arrive meanwhile (a menu or a
// window drag running its own loop) wait for the message loop.
static i32 g_busy;

static void apply_events(App* app) {
    if (g_busy) return;
    g_busy++;
    events_apply(app);
    g_busy--;
}

void wake_window(App* app) {
    HWND w = app->ui.window;
    if (w) PostMessageW(w, WM_APP_WAKE, 0, 0);
    HWND p = app->ui.popup;
    if (p) PostMessageW(p, WM_APP_WAKE, 0, 0);
}

bool stopping(App* app) { return WaitForSingleObject(app->stop, 0) == WAIT_OBJECT_0; }

bool sleep_unless_stopping(App* app, u32 ms) { return WaitForSingleObject(app->stop, ms) != WAIT_OBJECT_0; }

struct ThreadStart {
    App* app;
    LPTHREAD_START_ROUTINE fn;
    char name[64];
};

static DWORD WINAPI thread_main(void* p) {
    ThreadStart start = *(ThreadStart*)p;
    free(p);
    Mark m = mark(scratch());
    SetThreadDescription(GetCurrentThread(), wide(scratch(), str(start.name)));
    rewind(m);
    return start.fn(start.app);
}

void start_thread(App* app, const char* name, LPTHREAD_START_ROUTINE fn) {
    ThreadStart* s = (ThreadStart*)calloc(1, sizeof(ThreadStart));
    CHECK(s, "memory for a thread start");
    s->app = app;
    s->fn = fn;
    snprintf(s->name, sizeof(s->name), "%s", name);
    HANDLE t = CreateThread(0, 0, thread_main, s, 0, 0);
    CHECK(t, "thread %s to start (error %lu)", name, GetLastError());
    CloseHandle(t);
}

void ui_show(App* app, Tab tab, const char* day) {
    app->ui.want_window = true;
    app->ui.want_tab = tab;
    SET(app->ui.want_day, str(day ? day : ""));
    PostThreadMessageW(app->ui.main_thread, WM_APP_SHOW, 0, 0);
}

void ui_popup(App* app, Popup popup, const char* day) {
    app->ui.want_popup = popup;
    SET(app->ui.popup_day, str(day ? day : ""));
    PostThreadMessageW(app->ui.main_thread, WM_APP_SHOW, 0, 0);
}

void ui_say(App* app, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->ui.toast, sizeof(app->ui.toast), fmt, args);
    va_end(args);
    app->ui.toast_ts = now_ts();
    wake_window(app);
}

void app_quit(App* app) {
    SetEvent(app->stop);
    PostThreadMessageW(app->ui.main_thread, WM_APP_QUIT, 0, 0);
}

void app_restart(App* app) {
    app->restarting = true;
    app_quit(app);
}

// ---------------------------------------------------------------------------------------------
// D3D11 and surfaces

struct Gpu {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    IDXGIFactory2* factory;
};
static Gpu g_gpu;

struct Surface {
    HWND hwnd;
    IDXGISwapChain1* swap;
    ID3D11RenderTargetView* target;
    ImGuiContext* imgui;
    i32 frames_wanted;     // draw this many more frames (ImGui settles over two or three)
    f32 dpi_scale;
    bool closing;
};
static Surface g_main;
static Surface g_popup;

static void gpu_init() {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, levels, COUNT(levels), D3D11_SDK_VERSION,
                                   &g_gpu.device, 0, &g_gpu.context);
    if (FAILED(hr))  // no GPU driver (a VM, a remote desktop): software rendering still works
        hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0, 0, levels, COUNT(levels), D3D11_SDK_VERSION,
                               &g_gpu.device, 0, &g_gpu.context);
    CHECK(SUCCEEDED(hr), "a Direct3D 11 device (0x%08lx)", (unsigned long)hr);
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&g_gpu.factory);
    CHECK(SUCCEEDED(hr), "a DXGI factory (0x%08lx)", (unsigned long)hr);
}

static void surface_make_target(Surface* s) {
    ID3D11Texture2D* back = 0;
    s->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    CHECK(back, "the swap chain's back buffer");
    g_gpu.device->CreateRenderTargetView(back, 0, &s->target);
    back->Release();
}

static void load_fonts(f32 scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t fonts[MAX_PATH];
    GetWindowsDirectoryW(fonts, MAX_PATH);
    Mark m = mark(scratch());
    Str segoe = narrow(scratch(), fonts);
    Str path = str_fmt(scratch(), "%.*s\\Fonts\\segoeui.ttf", (int)segoe.len, segoe.ptr);
    ImFont* font = io.Fonts->AddFontFromFileTTF(path.ptr, 16.0f);
    if (!font) io.Fonts->AddFontDefault();
    // Lucide icons, merged into the same font so they can sit in labels.
    Str exe = narrow(scratch(), exe_dir());
    Str icons = str_fmt(scratch(), "%.*s\\assets\\lucide.ttf", (int)exe.len, exe.ptr);
    if (font && GetFileAttributesA(icons.ptr) != INVALID_FILE_ATTRIBUTES) {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.GlyphOffset.y = 3.0f;
        io.Fonts->AddFontFromFileTTF(icons.ptr, 16.0f, &cfg);
    }
    Str bold = str_fmt(scratch(), "%.*s\\Fonts\\seguisb.ttf", (int)segoe.len, segoe.ptr);
    // The second font, where ui_common.cpp's bold_font() finds it.
    if (font && GetFileAttributesA(bold.ptr) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(bold.ptr, 16.0f);
    rewind(m);
    ImGui::GetStyle().FontScaleDpi = scale;
}

// Whether Windows' apps are set to dark (Settings > Personalisation > Colours).
static bool system_dark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, 0, &light, &size);
    return light == 0;
}

static void style_init(f32 scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    bool dark = system_dark();
    if (dark) ImGui::StyleColorsDark(&st);
    else ImGui::StyleColorsLight(&st);
    st.WindowRounding = 0;
    st.ChildRounding = 6;
    st.FrameRounding = 6;
    st.GrabRounding = 6;
    st.PopupRounding = 8;
    st.FramePadding = ImVec2(10, 4);
    st.ItemSpacing = ImVec2(8, 6);
    st.WindowPadding = ImVec2(12, 12);
    st.ScrollbarSize = 12;
    st.WindowBorderSize = 0;
    ImVec4 accent = ImVec4(0 / 255.0f, 120 / 255.0f, 212 / 255.0f, 1);
    ImVec4* c = st.Colors;
    if (dark) {
        c[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.11f, 0.11f, 1);
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.15f, 0.15f, 1);
        c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.17f, 0.17f, 1);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.22f, 1);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.26f, 0.26f, 1);
        c[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.20f, 1);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.26f, 0.26f, 1);
        c[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.32f, 0.32f, 1);
        c[ImGuiCol_Header] = ImVec4(0.24f, 0.24f, 0.24f, 1);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1);
        c[ImGuiCol_HeaderActive] = ImVec4(0.32f, 0.32f, 0.32f, 1);
        c[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.25f, 1);
        c[ImGuiCol_Text] = ImVec4(0.86f, 0.86f, 0.86f, 1);
        c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.55f, 1);
    } else {
        c[ImGuiCol_WindowBg] = ImVec4(0.97f, 0.97f, 0.97f, 1);
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = ImVec4(1, 1, 1, 1);
        c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 1);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.93f, 0.93f, 0.93f, 1);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.89f, 0.89f, 0.89f, 1);
        c[ImGuiCol_Button] = ImVec4(0.91f, 0.91f, 0.91f, 1);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.86f, 0.86f, 0.86f, 1);
        c[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.80f, 0.80f, 1);
        c[ImGuiCol_Header] = ImVec4(0.87f, 0.87f, 0.87f, 1);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.84f, 0.84f, 0.84f, 1);
        c[ImGuiCol_HeaderActive] = ImVec4(0.80f, 0.80f, 0.80f, 1);
        c[ImGuiCol_Border] = ImVec4(0.85f, 0.85f, 0.85f, 1);
        c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.45f, 0.45f, 1);
    }
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_TextLink] = accent;
    c[ImGuiCol_NavCursor] = accent;
    st.ScaleAllSizes(scale);
}

// A texture from 32-bit RGBA pixels, for app icons and image previews (ui_common.cpp).
static ID3D11ShaderResourceView* texture_from_rgba(const u8* rgba, i32 w, i32 h) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {rgba, (UINT)w * 4, 0};
    ID3D11Texture2D* tex = 0;
    if (FAILED(g_gpu.device->CreateTexture2D(&desc, &data, &tex))) return 0;
    ID3D11ShaderResourceView* view = 0;
    g_gpu.device->CreateShaderResourceView(tex, 0, &view);
    tex->Release();
    return view;
}

static LRESULT CALLBACK surface_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static bool surface_open(Surface* s, const wchar_t* title, DWORD style, DWORD ex_style, i32 x, i32 y, i32 w, i32 h) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(0);
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = surface_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(0, IDC_ARROW);
        wc.hIcon = logo_icon(GetSystemMetrics(SM_CXICON), false);
        wc.hIconSm = logo_icon(GetSystemMetrics(SM_CXSMICON), false);
        wc.lpszClassName = L"lifelog.surface";
        RegisterClassExW(&wc);
        registered = true;
    }
    *s = {};
    s->hwnd = CreateWindowExW(ex_style, L"lifelog.surface", title, style, x, y, w, h, 0, 0, inst, 0);
    if (!s->hwnd) return false;
    SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, (LONG_PTR)s);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = g_gpu.factory->CreateSwapChainForHwnd(g_gpu.device, s->hwnd, &sd, 0, 0, &s->swap);
    CHECK(SUCCEEDED(hr), "a swap chain for the window (0x%08lx)", (unsigned long)hr);
    g_gpu.factory->MakeWindowAssociation(s->hwnd, DXGI_MWA_NO_ALT_ENTER);
    surface_make_target(s);

    s->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(s->imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = 0;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    s->dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(s->hwnd);
    style_init(s->dpi_scale);
    load_fonts(s->dpi_scale);
    ImGui_ImplWin32_Init(s->hwnd);
    ImGui_ImplDX11_Init(g_gpu.device, g_gpu.context);
    s->frames_wanted = 3;
    return true;
}

static void surface_close(Surface* s) {
    if (!s->hwnd) return;
    ImGui::SetCurrentContext(s->imgui);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(s->imgui);
    if (s->target) s->target->Release();
    if (s->swap) s->swap->Release();
    HWND hwnd = s->hwnd;
    *s = {};
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
}

// Draws one frame of `s`; `draw` is ui.cpp's function for it. Returns false when the surface
// asked to close.
static bool surface_draw(Surface* s, bool (*draw)(App*, f32* next_frame_ms)) {
    ImGui::SetCurrentContext(s->imgui);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    f32 next_ms = -1;
    g_busy++;
    bool keep = draw(&g_app, &next_ms);
    g_busy--;

    ImGui::Render();
    ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    f32 clear[4] = {bg.x, bg.y, bg.z, 1};
    g_gpu.context->OMSetRenderTargets(1, &s->target, 0);
    g_gpu.context->ClearRenderTargetView(s->target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    s->swap->Present(1, 0);

    if (next_ms == 0) s->frames_wanted = 2;
    else if (next_ms > 0) SetTimer(s->hwnd, REDRAW_TIMER, (UINT)next_ms, 0);
    return keep;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK surface_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Surface* s = (Surface*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    ImGui::SetCurrentContext(s->imgui);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        s->frames_wanted = 3;
        return 1;
    }
    // ImGui takes keys without saying it handled them, so wake a frame for them here.
    if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS) s->frames_wanted = 3;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && s->swap) {
            if (s->target) s->target->Release();
            s->target = 0;
            s->swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            surface_make_target(s);
            s->frames_wanted = 2;
            // While an edge is dragged, Windows runs its own loop and ours draws nothing, so the
            // last frame would be stretched to the new size: draw one here, at the new size.
            if (s == &g_main && !g_busy && !s->closing && !surface_draw(s, ui_draw_window)) s->closing = true;
        }
        return 0;
    case WM_GETMINMAXINFO:
        // The main window's layout needs this much room; smaller, things overlap or run off.
        if (s == &g_main) {
            UINT dpi = GetDpiForWindow(hwnd);
            RECT r = {0, 0, MulDiv(600, (i32)dpi, 96), MulDiv(460, (i32)dpi, 96)};
            AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE), dpi);
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        s->dpi_scale = HIWORD(wp) / 96.0f;
        style_init(s->dpi_scale);
        load_fonts(s->dpi_scale);
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        s->frames_wanted = 2;
        return 0;
    }
    case WM_TIMER:
        if (wp == REDRAW_TIMER) {
            KillTimer(hwnd, REDRAW_TIMER);
            s->frames_wanted = 2;
        }
        return 0;
    case WM_APP_WAKE:
        s->frames_wanted = 2;
        return 0;
    case WM_ACTIVATE:
        // The tray menu closes when it loses focus, like a menu.
        // LIFELOG_POPUP_STAY keeps it open, for screenshots while testing.
        if (s == &g_popup && LOWORD(wp) == WA_INACTIVE && !GetEnvironmentVariableW(L"LIFELOG_POPUP_STAY", 0, 0)) s->closing = true;
        s->frames_wanted = 2;
        return 0;
    case WM_CLOSE:
        s->closing = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd, 0);
        s->frames_wanted = 2;
        return 0;
    }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) s->frames_wanted = 3;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// The tray icon

static NOTIFYICONDATAW g_tray;
static UINT g_taskbar_created;

static void tray_add(HWND hwnd) {
    g_tray = {sizeof(g_tray)};
    g_tray.hWnd = hwnd;
    g_tray.uID = TRAY_ID;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = WM_APP_TRAY;
    g_tray.hIcon = logo_icon(GetSystemMetrics(SM_CXSMICON), g_app.capture.paused);
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(0, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"lifelog");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
    g_tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_tray);
}

void tray_update(App* app) {
    wcscpy_s(g_tray.szTip, app->capture.paused ? L"lifelog: private, recording nothing" : L"lifelog: recording");
    // Blue while recording, grey in private mode.
    HICON old = g_tray.hIcon;
    g_tray.hIcon = logo_icon(GetSystemMetrics(SM_CXSMICON), app->capture.paused);
    g_tray.uFlags = NIF_TIP | NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    if (old && old != g_tray.hIcon) DestroyIcon(old);
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_EVENT) {
        // Usually the message loop applies them; this is for while a menu or a window drag
        // runs its own loop. Not in the middle of a frame or of other events.
        apply_events(&g_app);
        return 0;
    }
    if (msg == WM_APP_TRAY) {
        switch (LOWORD(lp)) {
        case TRAY_OPEN_WINDOW:  // a second start of lifelog.exe
            ui_show(&g_app, g_app.ui.want_tab, 0);
            return 0;
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ui_popup(&g_app, POPUP_TRAY, 0);
            return 0;
        case WM_CONTEXTMENU: {
            // A plain Windows menu, for when lifelog's own one is not wanted.
            enum { CMD_PAUSE = 1, CMD_RESTART, CMD_QUIT };
            bool paused = g_app.capture.paused;
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, CMD_PAUSE, paused ? L"Resume" : L"Pause");
            AppendMenuW(menu, MF_STRING, CMD_RESTART, L"Restart");
            AppendMenuW(menu, MF_STRING, CMD_QUIT, L"Quit");
            POINT at;
            GetCursorPos(&at);
            SetForegroundWindow(hwnd);  // so the menu closes when you click elsewhere
            i32 cmd = (i32)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y, 0, hwnd, 0);
            DestroyMenu(menu);
            if (cmd == CMD_PAUSE) {
                capture_set_paused(&g_app, !g_app.capture.paused);
            } else if (cmd == CMD_RESTART) {
                app_restart(&g_app);
            } else if (cmd == CMD_QUIT) {
                app_quit(&g_app);
            }
            return 0;
        }
        }
        return 0;
    }
    if (msg == g_taskbar_created && msg) {  // Explorer restarted: the icon is gone
        tray_add(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND tray_window() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"lifelog.tray";
    RegisterClassExW(&wc);
    // A hidden top-level window rather than a message-only one: TrackPopupMenu needs a window
    // that can come to the foreground.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"lifelog.tray", L"lifelog", WS_POPUP, 0, 0, 0, 0, 0, 0, wc.hInstance, 0);
    CHECK(hwnd, "the tray's message window (error %lu)", GetLastError());
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd, g_taskbar_created, MSGFLT_ALLOW, 0);
    tray_add(hwnd);
    return hwnd;
}

// ---------------------------------------------------------------------------------------------
// Opening and closing the windows

static void open_main_window() {
    if (g_main.hwnd) {
        ShowWindow(g_main.hwnd, IsIconic(g_main.hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_main.hwnd);
        g_main.frames_wanted = 2;
        return;
    }
    // The main monitor's own scale, as the content uses; the system DPI can differ (Remote Desktop).
    POINT origin = {0, 0};
    f32 scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
    i32 w = (i32)(1000 * scale), h = (i32)(720 * scale);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    i32 x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;
    bool ok = surface_open(&g_main, L"lifelog", WS_OVERLAPPEDWINDOW, 0, x, y, w, h);
    CHECK(ok, "the main window to open (error %lu)", GetLastError());
    g_app.ui.window = g_main.hwnd;
    ui_window_opened(&g_app);
    ShowWindow(g_main.hwnd, SW_SHOW);
    SetForegroundWindow(g_main.hwnd);
}

static void close_main_window() {
    g_app.ui.window = 0;
    ui_window_closed(&g_app);
    surface_close(&g_main);
}

static void open_popup(Popup which) {
    if (g_popup.hwnd) surface_close(&g_popup);
    // Above the tray, at the cursor (where the icon was clicked).
    POINT at;
    GetCursorPos(&at);
    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    // That monitor's own scale, as the content uses; the system DPI can differ (Remote Desktop).
    f32 scale = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
    // A first guess at the height; popup_fit makes it match the content once drawn.
    i32 w = (i32)(300 * scale), h = (i32)(which == POPUP_DELAY ? 176 * scale : 380 * scale);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    i32 x = at.x - w / 2, y = at.y - h - (i32)(8 * scale);
    if (which == POPUP_DELAY) {
        x = mi.rcWork.right - w - (i32)(16 * scale);
        y = mi.rcWork.bottom - h - (i32)(16 * scale);
    }
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    bool ok = surface_open(&g_popup, L"lifelog", WS_POPUP | WS_BORDER, WS_EX_TOOLWINDOW | WS_EX_TOPMOST, x, y, w, h);
    CHECK(ok, "the tray menu to open (error %lu)", GetLastError());
    g_app.ui.popup = g_popup.hwnd;
    ui_popup_opened(&g_app);
    ShowWindow(g_popup.hwnd, SW_SHOW);
    SetForegroundWindow(g_popup.hwnd);
}

// Fits the popup's height to its content (App.ui.popup_height), keeping its bottom edge where it
// is, so it stays just above the tray.
static void popup_fit() {
    i32 want = g_app.ui.popup_height;
    RECT client, win;
    GetClientRect(g_popup.hwnd, &client);
    GetWindowRect(g_popup.hwnd, &win);
    i32 delta = want - (client.bottom - client.top);
    if (want <= 0 || delta == 0) return;
    i32 h = win.bottom - win.top + delta;
    SetWindowPos(g_popup.hwnd, 0, win.left, win.bottom - h, win.right - win.left, h, SWP_NOZORDER | SWP_NOACTIVATE);
    g_popup.frames_wanted = 2;
}

static void close_popup() {
    g_app.ui.popup = 0;
    g_app.ui.want_popup = POPUP_NONE;
    ui_popup_closed(&g_app);
    surface_close(&g_popup);
}

// ---------------------------------------------------------------------------------------------
// Start

static i32 run_app() {
    App* app = &g_app;
    app->stop = CreateEventW(0, TRUE, FALSE, 0);
    app->worker.wake = CreateEventW(0, FALSE, FALSE, 0);
    app->ui.main_thread = GetCurrentThreadId();
    // Make sure the thread has a message queue before anyone posts to it.
    MSG msg;
    PeekMessageW(&msg, 0, WM_USER, WM_USER, PM_NOREMOVE);

    char error[512] = {};
    if (!config_load(&app->config, error, sizeof(error))) {
        ERR("config: %s", error);
        Mark m = mark(scratch());
        MessageBoxW(0, wide(scratch(), str_fmt(scratch(), "lifelog could not read its settings:\n\n%s", error)), L"lifelog", MB_ICONERROR);
        rewind(m);
        return 1;
    }
    setup_cleanup(&app->config);  // before the AI is loaded from there
    disk_start();
    app->storage.ready = db_init();

    ImGui_ImplWin32_EnableDpiAwareness();
    gpu_init();
    app->ui.tray = tray_window();
    app->events.lock = SRWLOCK_INIT;
    app->events.window = app->ui.tray;

    db_thread_start(app);
    pool_start(app);
    apps_from_config(app);
    apps_start(app);
    capture_start(app);
    worker_start(app);
    summary_start(app);
    api_start(app);
    tray_update(app);
    LOG("lifelog started");

    bool want_main = true;  // the window opens at start unless --tray was passed
    // __argv is null in a wWinMain program; the wide arguments are in __wargv.
    for (i32 i = 1; i < __argc; i++)
        if (wcscmp(__wargv[i], L"--tray") == 0) want_main = false;
    if (want_main) open_main_window();

    for (;;) {
        // Handle everything waiting, then draw what needs it, then sleep until the next message.
        bool quit = false;
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_APP_QUIT || msg.message == WM_QUIT) quit = true;
            if (msg.message == WM_APP_SHOW) {
                bool open = app->ui.want_window;
                Popup popup = app->ui.want_popup;
                app->ui.want_window = false;
                if (open) open_main_window();
                if (popup != POPUP_NONE && !g_popup.hwnd) open_popup(popup);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        apply_events(app);

        if (g_main.hwnd && g_main.frames_wanted > 0) {
            g_main.frames_wanted--;
            if (!surface_draw(&g_main, ui_draw_window)) g_main.closing = true;
        }
        if (g_main.closing) close_main_window();
        if (g_popup.hwnd && g_popup.frames_wanted > 0) {
            g_popup.frames_wanted--;
            if (!surface_draw(&g_popup, ui_draw_popup)) g_popup.closing = true;
            else popup_fit();
        }
        if (g_popup.closing) close_popup();

        if ((g_main.hwnd && g_main.frames_wanted > 0) || (g_popup.hwnd && g_popup.frames_wanted > 0)) continue;
        WaitMessage();
    }

    LOG("lifelog stopping");
    SetEvent(app->stop);
    if (g_popup.hwnd) close_popup();
    if (g_main.hwnd) close_main_window();
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
    capture_stop(app);
    pool_stop();
    worker_stop(app);
    db_thread_stop();
    disk_stop();  // the settings and paths asked for are on disk before the program ends
    if (app->restarting) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(0, exe, MAX_PATH);
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        wchar_t cmd[MAX_PATH + 16];
        // The new copy waits for this one to exit before it takes the single-instance mutex.
        swprintf(cmd, COUNT(cmd), L"\"%ls\" --tray --after %lu", exe, GetCurrentProcessId());
        if (CreateProcessW(exe, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return 0;
}

// Takes the one-lifelog-at-a-time mutex; false if another copy holds it. A test copy
// (LIFELOG_CONFIG set) is one per config, so several can run beside the real one. After a
// restart (--after PID) it first waits up to 30 s for the old copy to exit.
static bool single_instance() {
    for (i32 i = 1; i + 1 < __argc; i++) {
        if (wcscmp(__wargv[i], L"--after") != 0) continue;
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)wcstoul(__wargv[i + 1], 0, 10));
        if (old) {
            WaitForSingleObject(old, 30000);
            CloseHandle(old);
        }
    }
    wchar_t name[96];
    instance_name(name);
    HANDLE single = CreateMutexW(0, TRUE, name);  // held until the process exits
    return !(single && GetLastError() == ERROR_ALREADY_EXISTS);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    log_open();
    // A notification's button opens a lifelog: link, which starts this exe with it; pass it to
    // the running copy and exit.
    if (__argc > 1 && wcsncmp(__wargv[1], L"lifelog", 7) == 0) return notify_forward(__wargv[1]) ? 0 : 1;
    // `lifelog.exe clip 15m` and the other commands: the CLI (options start with "-").
    if (__argc > 1 && __wargv[1][0] != L'-') return cli_main(__argc, __wargv);
    // One lifelog at a time: a second start opens the first one's window instead.
    if (!single_instance()) {
        HWND other = FindWindowW(L"lifelog.tray", 0);
        if (!other) other = FindWindowExW(HWND_MESSAGE, 0, L"lifelog.tray", 0);
        AllowSetForegroundWindow(ASFW_ANY);  // so its window comes to the front, not behind others
        if (other) PostMessageW(other, WM_APP_TRAY, 0, MAKELPARAM(TRAY_OPEN_WINDOW, 0));
        return 0;
    }
    i32 code = run_app();
    ExitProcess((UINT)code);
}
