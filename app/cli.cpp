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
    "  lifelog transcription auto | always | paused | continue   when the GPU work may run (auto = not while\n"
    "                          gaming; continue = back to the mode before it was paused)\n"
    "  lifelog mics            list the mics plugged in (the one to record is picked in Settings)\n"
    "  lifelog launch APP      start an installed app or game (spotify, \"visual studio code\")\n"
    "  lifelog summary [now | later | skip] [DAY]       the daily summary (DAY = YYYY-MM-DD, default today)\n"
    "  lifelog check-ai        start the summary AI, ask it one thing, stop it\n"
    "  lifelog summarise [RANGE]   a few sentences about the last 15m (default), 1h or today, from the summary AI\n"
    "  lifelog status          what the running service is doing\n"
    "  lifelog quit            stop the running service\n"
    "  lifelog config          print the config file path\n"
    "  lifelog voice \"TEXT\"    what a voice command would do with TEXT, without doing it\n"
    "  lifelog commands [allow ID]   list the custom voice commands, or allow one to run\n"
    "  lifelog commands secret ID KEY   save a command's secret, typed or piped in (empty removes it)\n"
    "  lifelog command \"TEXT\"  run the custom command TEXT matches (TEXT without the wake word)\n"
    "  lifelog mcp             an MCP server on stdin/stdout, for AI agents to read the transcript\n"
    "                          (claude mcp add lifelog -- <path to lifelog.exe> mcp)\n";

static i32 run_app();  // main.cpp, after this file in the unity build
static i32 cli_mcp();  // mcp.cpp, after this file
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

// `lifelog summarise [RANGE]`: a few sentences about the last stretch from the summary AI, as the
// voice command makes, printed here (not put on the clipboard).
static i32 cli_summarise(Str range) {
    if (!cli_config(&g_cli.config)) return 1;
    f64 now = now_ts(), from = 0;
    if (!clip_range(range, now, &from)) return cli_error("lifelog summarise [RANGE]; RANGE = 15m (default), 1h, today");
    g_cli.stop = CreateEventW(0, TRUE, FALSE, 0);
    summary_options_publish(&g_cli);
    Arena a = {};
    char error[512] = {};
    i32 lines = 0;
    Str text = clip_text(0, &a, from, now, &lines, error, sizeof(error));
    Str out = {};
    bool ok = !error[0] && lines > 0 && summary_quick(&g_cli, &a, text, &out, error, sizeof(error));
    if (!error[0] && !lines) snprintf(error, sizeof(error), "nothing was heard then");
    if (ok) cli_print("%.*s\n", (int)out.len, out.ptr);
    arena_release(&a);
    return ok ? 0 : cli_error("%s", error);
}

// What voice commands would make of a sentence, without doing it.
static i32 cli_voice(Str text) {
    if (!cli_config(&g_cli.config)) return 1;
    commands_load(&g_cli.config);
    const char* wake = wake_word(&g_cli.config);
    Understood u;
    voice_understand(text, wake, false, &u);
    const VoiceCommand* c = &u.c;
    if (!u.for_us) cli_print("not for lifelog: it does not start with \"%s\"\n", wake);
    else if (u.name_only) cli_print("the name alone: it chimes and waits 6 s for the command\n");
    else if (c->what == DO_NOTHING) cli_print("not a command\n");
    else if (c->what == DO_API) cli_print("%s: %s %s%s%s\n", c->said, c->method, c->path, c->query[0] ? "?" : "", c->query);
    else if (c->what == DO_CUSTOM) cli_print("custom command %s\n  slots: %s\n  said: %s\n", c->custom.label, c->custom.slots, c->custom.said);
    else cli_print("%s\n", c->what == DO_CANCEL ? "cancel" : c->said);
    if (c->shown[0]) cli_print("banner when it works: %s\n", c->shown);
    return 0;
}

// `lifelog commands [allow ID]`: lists the custom commands, or allows one as its files are now.
static i32 cli_commands(Str verb, Str id, Str key) {
    if (!cli_config(&g_cli.config)) return 1;
    commands_load(&g_cli.config);
    if (str_eq(verb, str("allow"))) {
        bool found = false;
        for (i32 i = 0; i < g_commands.count; i++) found |= str_eq(str(g_commands.items[i].id), id);
        if (!found) return cli_error("there is no command %.*s (lifelog commands lists them)", (int)id.len, id.ptr);
        commands_allow(&g_cli, id.ptr);
        cli_print("allowed %.*s\n", (int)id.len, id.ptr);
        return 0;
    }
    // The value comes on stdin, so it is not left in the shell's history.
    if (str_eq(verb, str("secret"))) {
        if (!key.len) return cli_error("lifelog commands secret ID KEY, with the value on stdin (empty removes it)");
        static char value[4096];
        DWORD got = 0;
        ReadFile(GetStdHandle(STD_INPUT_HANDLE), value, sizeof(value) - 1, &got, 0);
        Str v = str_trim({value, (i64)got});
        char id_c[64], key_c[64];
        SET(id_c, id);
        SET(key_c, key);
        if (!secret_set(id_c, key_c, v)) return cli_error("it could not be saved");
        SecureZeroMemory(value, sizeof(value));
        cli_print("%s %s.%s\n", v.len ? "saved" : "removed", id_c, key_c);
        return 0;
    }
    if (verb.len) return cli_error("lifelog commands [allow ID | secret ID KEY]");
    cli_print("%ls\n", commands_dir());
    if (!g_commands.count) cli_print("  (none yet: each command is a folder here with a command.toml)\n");
    for (i32 i = 0; i < g_commands.count; i++) {
        const CustomCommand* c = &g_commands.items[i];
        const char* state = c->error[0] ? "broken" : !c->allowed ? (c->changed ? "changed: allow it again in Settings" : "new: allow it in Settings")
                            : !c->enabled ? "off" : "on";
        cli_print("\n%s (%s): %s\n", c->name, c->id, state);
        if (c->error[0]) cli_print("  %s\n", c->error);
        for (i32 a = 0; a < c->action_count; a++) {
            cli_print("  %s%s%s\n", c->actions[a].name, c->actions[a].description[0] ? ": " : "", c->actions[a].description);
            for (i32 p = 0; p < c->phrase_count; p++)
                if (c->phrases[p].action == a) cli_print("    \"%s\"\n", c->phrases[p].say);
        }
        for (i32 s = 0; s < c->secret_count; s++)
            cli_print("  secret %s: %s\n", c->secrets[s], secret_is_set(c->id, c->secrets[s]) ? "set" : "not set");
    }
    return 0;
}

// Runs the custom command a sentence (without the wake word) matches, here.
static i32 cli_command(Str text) {
    if (!text.len) return cli_error("lifelog command \"play bad guy\"");
    if (!cli_config(&g_cli.config)) return 1;
    commands_load(&g_cli.config);
    CmdMatch m;
    if (!commands_match_text(text, &m)) return cli_error("no custom command matches that (lifelog commands lists them)");
    cli_print("%s\n", m.label);
    char message[256], banner[160];
    bool ok = commands_run_here(&m, message, sizeof(message), banner, sizeof(banner));
    if (banner[0]) cli_print("banner: %s\n", banner);
    if (!ok) return cli_error("%s", message[0] ? message : "it failed");
    if (message[0]) cli_print("%s\n", message);
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
    Str args[4] = {};
    for (i32 i = 0; i < 4 && i + 1 < argc; i++) args[i] = narrow(a, argv[i + 1]);
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
        if (has_arg && str_eq(str_lower(a, args[1]), str("continue"))) return cli_call("POST", str("/transcription?mode=continue"));
        if (!has_arg || !mode_from_text(args[1], &mode)) return cli_error("say which: lifelog transcription auto | always | paused | continue");
        return cli_call("POST", str_fmt(a, "/transcription?mode=%s", mode_name(mode)));
    }
    if (str_eq(cmd, str("mics"))) {
        if (!cli_config(&g_cli.config)) return 1;
        MicDevice mics[32];
        char def[128];
        i32 n = capture_mics(mics, COUNT(mics), def, sizeof(def));
        Config* c = &g_cli.config;
        cli_print("Windows default: %s\n", def[0] ? def : "(none)");
        for (i32 i = 0; i < n; i++) cli_print("  %s%s\n", mics[i].name, strcmp(mics[i].id, c->capture.mic_device) == 0 ? "  (chosen)" : "");
        cli_print("Recording from: %s\n", c->capture.mic_device[0] ? c->capture.mic_name : "the Windows default");
        return 0;
    }
    if (str_eq(cmd, str("launch"))) {
        if (!has_arg) return cli_error("say which app: lifelog launch spotify");
        return cli_call("POST", str_fmt(a, "/launch?app=%s", url_enc(a, args[1]).ptr));
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
    if (str_eq(cmd, str("summarise")) || str_eq(cmd, str("summarize"))) return cli_summarise(has_arg ? args[1] : str("15m"));
    if (str_eq(cmd, str("status"))) return cli_call("GET", str("/status"));
    if (str_eq(cmd, str("voice"))) return cli_voice(has_arg ? args[1] : str(""));
    if (str_eq(cmd, str("commands")))
        return cli_commands(has_arg ? args[1] : str(""), argc > 3 ? args[2] : str(""), argc > 4 ? args[3] : str(""));
    if (str_eq(cmd, str("command"))) return cli_command(has_arg ? args[1] : str(""));
    if (str_eq(cmd, str("mcp"))) return cli_mcp();
    if (str_eq(cmd, str("icon"))) return cli_icon(has_arg ? args[1] : str(""));
    if (str_eq(cmd, str("config"))) {
        if (!cli_config(&g_cli.config)) return 1;
        cli_print("%s\n", narrow(a, config_path()).ptr);
        return 0;
    }
    cli_print("%s", CLI_USAGE);
    return 0;
}
