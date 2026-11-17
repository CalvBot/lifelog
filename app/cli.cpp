// cli.cpp: `lifelog.exe <command>` from a terminal: talks to the running
// lifelog through its API, or works straight from the database where the old CLI did.
//
// lifelog.exe is a windows-subsystem program, so it has no console of its own: it attaches to
// the terminal it was started from. Output goes there as UTF-16 (a console) or UTF-8 (a pipe).

static const char* CLI_USAGE =
    "lifelog — always-on personal transcript\n"
    "\n"
    "  lifelog run             run in this terminal (normally it runs from the tray)\n"
    "  lifelog clip [RANGE]    copy a transcript file to the clipboard; RANGE = 15m (default), 1h, 2d, today\n"
    "  lifelog pause | resume | toggle     private mode\n"
    "  lifelog exclude APP.exe | include APP.exe     never capture an app / allow it again\n"
    "  lifelog game APP.exe | not-game APP.exe       while a game runs, only record; transcribe after\n"
    "  lifelog transcription auto | always | paused     when the GPU work may run (auto = not while gaming)\n"
    "  lifelog summary [now | later | skip] [DAY]       the daily summary (DAY = YYYY-MM-DD, default today)\n"
    "  lifelog check-ai        start the summary AI, ask it one thing, stop it\n"
    "  lifelog status          what the running service is doing\n"
    "  lifelog quit            stop the running service\n"
    "  lifelog config          print the config file path\n";

static i32 run_app();  // main.cpp, after this file in the unity build
static bool single_instance();  // main.cpp

// The App for commands that work without the running copy (clip, check-ai): only its config.
static App g_cli;

static bool handle_ok(HANDLE h) { return h && h != INVALID_HANDLE_VALUE; }

// Points stdout and stderr at the parent's console, unless they were redirected already.
static void cli_console() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    if (handle_ok(out) && handle_ok(err)) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (!handle_ok(con)) return;
    if (!handle_ok(out)) SetStdHandle(STD_OUTPUT_HANDLE, con);
    if (!handle_ok(err)) SetStdHandle(STD_ERROR_HANDLE, con);
}

static void cli_write(DWORD which, Str s) {
    HANDLE h = GetStdHandle(which);
    if (!handle_ok(h) || s.len == 0) return;
    DWORD mode = 0, wrote = 0;
    if (GetConsoleMode(h, &mode)) {
        // A console shows UTF-8 bytes in its own code page; UTF-16 always reads right.
        Mark m = mark(scratch());
        wchar_t* w = wide(scratch(), s);
        WriteConsoleW(h, w, (DWORD)wcslen(w), &wrote, 0);
        rewind(m);
    } else {
        WriteFile(h, s.ptr, (DWORD)s.len, &wrote, 0);
    }
}

static void cli_print(const char* fmt, ...) {
    Mark m = mark(scratch());
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(0, 0, fmt, args);
    va_end(args);
    char* text = PUSH_ARRAY(scratch(), char, n + 1);
    va_start(args, fmt);
    vsnprintf(text, n + 1, fmt, args);
    va_end(args);
    cli_write(STD_OUTPUT_HANDLE, {text, n});
    rewind(m);
}

// An error for the terminal (as the old CLI's log line was), and the exit code for it.
static i32 cli_error(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    Mark m = mark(scratch());
    cli_write(STD_ERROR_HANDLE, str_fmt(scratch(), "[ERROR] %s\n", text));
    rewind(m);
    return 1;
}

static bool cli_config(Config* cfg) {
    char error[512] = {};
    if (config_load(cfg, error, sizeof(error))) return true;
    cli_error("%s", error);
    return false;
}

// Sends `method path` to the running lifelog and prints its answer.
static i32 cli_call(const char* method, Str path) {
    Config* cfg = &g_cli.config;
    if (!cli_config(cfg)) return 1;
    Str body = {};
    char error[256] = {};
    if (!api_call(cfg->api_port, method, path, scratch(), &body, error, sizeof(error))) return cli_error("%s", error);
    cli_print("%.*s\n", (int)body.len, body.ptr);
    return 0;
}

// Ctrl+C in `lifelog run`: stop the way the tray's Quit does.
static DWORD g_cli_main_thread;
static BOOL WINAPI cli_ctrl(DWORD) {
    PostThreadMessageW(g_cli_main_thread, WM_APP + 4, 0, 0);  // main.cpp's WM_APP_QUIT
    return TRUE;
}

static i32 cli_run() {
    if (!single_instance()) return cli_error("lifelog is already running");
    g_cli_main_thread = GetCurrentThreadId();
    SetConsoleCtrlHandler(cli_ctrl, TRUE);
    // The log already goes to stderr as well as the file, so it shows in this terminal.
    i32 code = run_app();
    ExitProcess((UINT)code);
}

static i32 cli_clip(Str range) {
    if (!cli_config(&g_cli.config)) return 1;
    f64 now = now_ts(), from = 0;
    char label[64] = {};
    if (!clip_range(range, now, &from, label, sizeof(label))) {
        Mark m = mark(scratch());
        Str lower = str_lower(scratch(), str_trim(range));
        cli_error("range must look like 15m, 1h, 2d or today (got '%s')", lower.ptr);
        rewind(m);
        return 1;
    }
    ClipResult c = clip_copy(0, from, now, 0, CLIP_FILE, label);
    if (!c.ok) return cli_error("%s", c.error);
    cli_print("%d lines → %s (on the clipboard)\n", c.lines, c.path);
    return 0;
}

static i32 cli_check_ai() {
    if (!cli_config(&g_cli.config)) return 1;
    g_cli.stop = CreateEventW(0, TRUE, FALSE, 0);
    static char said[4096];
    summary_options_publish(&g_cli);
    bool ok = summary_check_ai(&g_cli, said, sizeof(said));
    if (!ok) return cli_error("%s", said);
    cli_print("%s\n", said);
    return 0;
}

// For developers: writes the logo as an .ico (PNG images in it), as `lifelog icon` did.
static i32 cli_icon(Str path) {
    if (!path.len) return cli_error("lifelog icon PATH.ico");
    const i32 sizes[] = {16, 20, 24, 32, 48, 64, 128};
    Arena* a = scratch();
    Mark m = mark(a);
    Str pngs[COUNT(sizes)];
    for (i32 i = 0; i < COUNT(sizes); i++) pngs[i] = logo_png(a, sizes[i]);
    Buf b = {};
    u8 head[6] = {0, 0, 1, 0, (u8)COUNT(sizes), 0};
    buf_add(&b, {(const char*)head, 6});
    u32 offset = 6 + 16 * COUNT(sizes);
    for (i32 i = 0; i < COUNT(sizes); i++) {
        u8 e[16] = {(u8)sizes[i], (u8)sizes[i], 0, 0, 1, 0, 32, 0};
        u32 len = (u32)pngs[i].len;
        memcpy(e + 8, &len, 4);
        memcpy(e + 12, &offset, 4);
        buf_add(&b, {(const char*)e, 16});
        offset += len;
    }
    for (i32 i = 0; i < COUNT(sizes); i++) buf_add(&b, pngs[i]);
    Str data = buf_take(&b, a);
    bool ok = write_file(wide(a, path), data);
    rewind(m);
    return ok ? 0 : cli_error("could not write %.*s", (int)path.len, path.ptr);
}

// The mutex the running lifelog holds until it exits (one per config, so a test copy is apart).
static void instance_name(wchar_t out[96]) {
    swprintf(out, 96, L"Local\\lifelog.cpp.running");
    wchar_t test_config[MAX_PATH];
    if (GetEnvironmentVariableW(L"LIFELOG_CONFIG", test_config, MAX_PATH)) {
        u32 hash = 2166136261u;
        for (wchar_t* c = test_config; *c; c++) hash = (hash ^ towlower(*c)) * 16777619u;
        swprintf(out, 96, L"Local\\lifelog.cpp.test.%08x", hash);
    }
}

// `lifelog quit`: asks, then waits (up to 30 s) until the program has exited, so an installer
// can replace its files straight after.
static i32 cli_quit() {
    wchar_t name[96];
    instance_name(name);
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (!running) return 0;  // not running: nothing to do
    i32 code = cli_call("POST", str("/quit"));
    if (code == 0) WaitForSingleObject(running, 30000);
    CloseHandle(running);
    return code;
}

// Runs a command and returns the exit code. Called before anything else starts.
static i32 cli_main(i32 argc, wchar_t** argv) {
    cli_console();
    Arena* a = scratch();
    Str args[3] = {};
    for (i32 i = 0; i < 3 && i + 1 < argc; i++) args[i] = narrow(a, argv[i + 1]);
    Str cmd = args[0];
    bool has_arg = argc > 2;

    if (str_eq(cmd, str("run"))) return cli_run();
    if (str_eq(cmd, str("clip"))) return cli_clip(has_arg ? args[1] : str("15m"));
    if (str_eq(cmd, str("quit"))) return cli_quit();
    if (str_eq(cmd, str("pause")) || str_eq(cmd, str("resume")) || str_eq(cmd, str("toggle")))
        return cli_call("POST", str_fmt(a, "/%s", cmd.ptr));
    if (str_eq(cmd, str("exclude")) || str_eq(cmd, str("include")) || str_eq(cmd, str("game")) || str_eq(cmd, str("not-game"))) {
        if (!cli_config(&g_cli.config)) return 1;
        if (!has_arg) return cli_error("say which app: lifelog %s spotify.exe", cmd.ptr);
        return cli_call("POST", str_fmt(a, "/%s?app=%s", cmd.ptr, url_enc(a, args[1]).ptr));
    }
    if (str_eq(cmd, str("transcription"))) {
        if (!cli_config(&g_cli.config)) return 1;
        Mode mode = MODE_AUTO;
        if (!has_arg || !mode_from_text(args[1], &mode)) return cli_error("say which: lifelog transcription auto | always | paused");
        return cli_call("POST", str_fmt(a, "/transcription?mode=%s", mode_name(mode)));
    }
    if (str_eq(cmd, str("summary"))) {
        if (!cli_config(&g_cli.config)) return 1;
        Str action = has_arg ? args[1] : str("now");
        if (!str_eq(action, str("now")) && !str_eq(action, str("later")) && !str_eq(action, str("skip")))
            return cli_error("lifelog summary now | later | skip [YYYY-MM-DD]");
        // Only digits and dashes go into the query; anything else means today.
        Str day = argc > 3 ? args[2] : str("");
        for (i64 i = 0; i < day.len; i++)
            if (!isdigit((u8)day.ptr[i]) && day.ptr[i] != '-') day = str("");
        return cli_call("POST", str_fmt(a, "/summary?do=%s&day=%s", action.ptr, day.ptr));
    }
    if (str_eq(cmd, str("check-ai"))) return cli_check_ai();
    if (str_eq(cmd, str("status"))) return cli_call("GET", str("/status"));
    if (str_eq(cmd, str("icon"))) return cli_icon(has_arg ? args[1] : str(""));
    if (str_eq(cmd, str("config"))) {
        if (!cli_config(&g_cli.config)) return 1;
        cli_print("%s\n", narrow(a, config_path()).ptr);
        return 0;
    }
    cli_print("%s", CLI_USAGE);
    return 0;
}
