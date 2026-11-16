// api.cpp: the small HTTP API on 127.0.0.1:<api_port> for the CLI, the Stream Deck and scripts.
// The window never uses it.
//
//   GET  /status            what is being captured, paused or not, queue length
//   POST /pause | /resume | /toggle     private mode
//   POST /exclude | /include | /game | /not-game ?app=X.exe
//   POST /transcription?mode=auto|always|paused     when the GPU work may run
//   POST /setup?part=transcriber|summary    download and set up that AI (as Settings' "Set up")
//   POST /clip?range=15m    write the clip file and put it on the clipboard
//   POST /summary?do=now|later|skip|until[&day=][&at=]
//   POST /remind?do=yes|no|done|snooze&id=N        a reminder notification's buttons
//   POST /show?tab=summary[&day=] | /popup?page=delay[&day=]   open the window
//   POST /quit | /restart
//
// Answers are JSON with the keys in sorted order, as the old app's were. Plain Winsock: one
// thread takes one request at a time, which is all a Stream Deck and a CLI need.

#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

// Percent-encodes a query value.
static Str url_enc(Arena* a, Str s) {
    char* out = PUSH_ARRAY(a, char, s.len * 3 + 1);
    i64 n = 0;
    for (i64 i = 0; i < s.len; i++) {
        u8 b = (u8)s.ptr[i];
        if (isalnum(b) || b == '-' || b == '_' || b == '.' || b == '~') out[n++] = (char)b;
        else n += snprintf(out + n, 4, "%%%02X", b);
    }
    out[n] = 0;
    return {out, n};
}

static i32 hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The value of the first `key=value` in a query string, percent-decoded. False if it is not there.
static bool query_param(Arena* a, Str query, const char* key, Str* out) {
    Str k = str(key);
    i64 at = 0;
    while (at <= query.len) {
        i64 end = at;
        while (end < query.len && query.ptr[end] != '&') end++;
        Str kv = str_sub(query, at, end - at);
        if (kv.len > k.len && str_starts(kv, k) && kv.ptr[k.len] == '=') {
            Str raw = str_sub(kv, k.len + 1);
            char* buf = PUSH_ARRAY(a, char, raw.len + 1);
            i64 n = 0;
            for (i64 i = 0; i < raw.len; i++) {
                char c = raw.ptr[i];
                if (c == '%' && i + 2 < raw.len && hex_digit(raw.ptr[i + 1]) >= 0 && hex_digit(raw.ptr[i + 2]) >= 0) {
                    buf[n++] = (char)(hex_digit(raw.ptr[i + 1]) * 16 + hex_digit(raw.ptr[i + 2]));
                    i += 2;
                } else if (c == '+') {
                    buf[n++] = ' ';
                } else {
                    buf[n++] = c;
                }
            }
            buf[n] = 0;
            *out = {buf, n};
            return true;
        }
        at = end + 1;
    }
    *out = {"", 0};
    return false;
}

// ---------------------------------------------------------------------------------------------
// Answers

static void json_names(Buf* b, Names* n) {
    buf_cstr(b, "[");
    for (i32 i = 0; i < n->count; i++) {
        if (i) buf_cstr(b, ",");
        json_quote(b, str(n->items[i].s));
    }
    buf_cstr(b, "]");
}

// A string, or null when it is empty.
static void json_text_or_null(Buf* b, const char* s) {
    if (s && s[0]) json_quote(b, str(s));
    else buf_cstr(b, "null");
}

static void json_wide(Buf* b, const wchar_t* w) {
    Mark m = mark(scratch());
    json_quote(b, narrow(scratch(), w));
    rewind(m);
}

// GET /status, the same fields as the old app's. With the lock.
static void status_json(App* app, Buf* b) {
    Config* c = &app->config;
    char today[11];
    day_of(now_ts(), today);
    const char* today_status = 0;
    for (i32 i = 0; i < app->summary.days.count; i++)
        if (strcmp(app->summary.days.items[i].day, today) == 0) today_status = app->summary.days.items[i].status;

    buf_cstr(b, "{\"excluded_apps\":");
    json_names(b, &c->capture.excluded_apps);
    buf_cstr(b, ",\"game_running\":");
    json_text_or_null(b, app->apps.game_running);
    buf_cstr(b, ",\"games\":");
    json_names(b, &c->processing.games);
    buf_fmt(b, ",\"gpu_3d_percent\":%u,\"gpu_busy_percent\":%u", app->worker.gpu_3d_percent, c->processing.gpu_busy_percent);
    buf_cstr(b, ",\"included_apps\":");
    json_names(b, &c->capture.included_apps);
    buf_cstr(b, ",\"not_games\":");
    json_names(b, &c->processing.not_games);
    buf_cstr(b, ",\"paths\":{\"config\":");
    json_wide(b, config_path());
    buf_cstr(b, ",\"data\":");
    json_quote(b, str(c->data_dir));
    buf_cstr(b, ",\"log\":");
    json_wide(b, log_path());
    buf_fmt(b, "},\"paused\":%s", app->capture.paused ? "true" : "false");
    buf_fmt(b, ",\"queued\":%d,\"queued_minutes\":%.1f", app->worker.queued, round(app->worker.queued_secs / 6.0) / 10.0);
    buf_cstr(b, ",\"running_apps\":[");
    i32 n = 0;
    for (i32 i = 0; i < app->apps.list.count; i++) {
        AppInfo* a = &app->apps.list.items[i];
        if (!a->running || is_own_exe(str(a->exe))) continue;
        if (n++) buf_cstr(b, ",");
        json_quote(b, str(a->exe));
    }
    buf_cstr(b, "],\"streams\":[");
    for (i32 i = 0; i < app->capture.streams.count; i++) {
        Stream* s = app->capture.streams.items[i];
        if (i) buf_cstr(b, ",");
        bool mic = s->source == SOURCE_MIC;
        buf_cstr(b, "{\"device\":");
        json_text_or_null(b, mic ? s->device : 0);
        buf_cstr(b, ",\"exe\":");
        json_text_or_null(b, mic ? 0 : s->exe);
        if (mic) buf_cstr(b, ",\"pid\":null");
        else buf_fmt(b, ",\"pid\":%u", s->pid);
        buf_fmt(b, ",\"source\":\"%s\",\"title\":", mic ? "mic" : "app");
        json_text_or_null(b, mic ? 0 : s->title);
        buf_cstr(b, "}");
    }
    buf_cstr(b, "],\"summary\":{\"ask_at\":");
    json_quote(b, str(c->summary.ask_at));
    buf_fmt(b, ",\"ask_reminders\":%s,\"enabled\":%s,\"max_words\":%u,\"model\":", c->summary.ask_reminders ? "true" : "false",
            c->summary.enabled ? "true" : "false", c->summary.max_words);
    json_quote(b, str(c->summary.model));
    buf_cstr(b, ",\"state\":");
    json_quote(b, str(app->summary.phase[0] ? app->summary.phase : "idle"));
    buf_cstr(b, ",\"today\":");
    json_quote(b, str(today));
    buf_cstr(b, ",\"today_status\":");
    json_text_or_null(b, today_status);
    buf_cstr(b, "},\"transcription\":{\"mode\":");
    json_quote(b, str(mode_name(c->processing.mode)));
    buf_cstr(b, ",\"state\":");
    json_quote(b, str(app->worker.state));
    buf_cstr(b, "},\"setup\":{");
    const char* parts[SETUP_PARTS] = {"transcriber", "summary"};
    for (i32 i = 0; i < SETUP_PARTS; i++) {
        SetupState* st = &app->setup.part[i];
        buf_fmt(b, "%s\"%s\":{\"running\":%s,\"failed\":%s,\"text\":", i ? "," : "", parts[i], st->running ? "true" : "false",
                st->failed ? "true" : "false");
        json_quote(b, str(st->text));
        buf_cstr(b, "}");
    }
    buf_cstr(b, "}}");
}

// "auto", "always", "paused" (or "off"), any case.
static bool mode_from_text(Str s, Mode* out) {
    Mark m = mark(scratch());
    Str t = str_lower(scratch(), str_trim(s));
    bool ok = str_eq(t, str("off")) ? (*out = MODE_PAUSED, true) : mode_parse(t, out);
    rewind(m);
    return ok;
}

// What to do once the answer has gone out: quitting first would cut it off.
enum After : u8 {
    AFTER_NOTHING,
    AFTER_QUIT,
    AFTER_RESTART,
};

struct Answer {
    i32 code;
    Buf body;
    After after;
};

static void answer_error(Answer* r, i32 code, const char* text) {
    buf_free(&r->body);
    r->code = code;
    buf_cstr(&r->body, "{\"error\":");
    json_quote(&r->body, str(text));
    buf_cstr(&r->body, "}");
}

// Does what `method path?query` asks. Takes the lock itself, briefly.
static void api_handle(App* app, Str method, Str path, Str query, Answer* r) {
    Arena* a = scratch();
    r->code = 200;
    bool get = str_eq(method, str("GET")), post = str_eq(method, str("POST"));
    Str exe = {};
    query_param(a, query, "app", &exe);
    exe = str_lower(a, str_trim(exe));

    if (get && str_eq(path, str("/status"))) {
        app_lock(app);
        status_json(app, &r->body);
        app_unlock(app);
        return;
    }
    if (!post) {
        answer_error(r, 404, "not found");
        return;
    }

    // The changes, each answering with the new status.
    bool pause = str_eq(path, str("/pause")), resume = str_eq(path, str("/resume")), toggle = str_eq(path, str("/toggle"));
    bool exclude = str_eq(path, str("/exclude")), include = str_eq(path, str("/include"));
    bool game = str_eq(path, str("/game")), not_game = str_eq(path, str("/not-game"));
    bool transcription = str_eq(path, str("/transcription"));
    if (pause || resume || toggle || exclude || include || game || not_game || transcription) {
        if ((exclude || include || game || not_game) && exe.len == 0) {
            answer_error(r, 400, "say which app, e.g. spotify.exe");
            return;
        }
        Mode mode = MODE_AUTO;
        if (transcription) {
            Str m = {};
            if (!query_param(a, query, "mode", &m) || !mode_from_text(m, &mode)) {
                answer_error(r, 400, "mode must be auto, always or paused");
                return;
            }
        }
        app_lock(app);
        if (pause || resume || toggle) {
            capture_set_paused(app, toggle ? !app->capture.paused : pause);
            tray_update(app);
        } else if (exclude || include) {
            app_set_decision(app, exe, exclude ? DECISION_EXCLUDED : DECISION_INCLUDED);
            wake_window(app);
        } else if (game || not_game) {
            // "not-game" takes the game mark off; an app dismissed as "not a game" stays so.
            AppInfo* info = app_find(app, exe);
            if (game) app_set_game(app, exe, GAME_YES);
            else if (!info || info->game == GAME_YES) app_set_game(app, exe, GAME_NO);
            wake_window(app);
        } else {
            worker_set_mode(app, mode);
            wake_window(app);
        }
        status_json(app, &r->body);
        app_unlock(app);
        return;
    }

    if (str_eq(path, str("/setup"))) {
        Str part = {};
        query_param(a, query, "part", &part);
        bool transcriber = str_eq(part, str("transcriber")), summary = str_eq(part, str("summary"));
        if (!transcriber && !summary) {
            answer_error(r, 400, "part must be transcriber or summary");
            return;
        }
        app_lock(app);
        setup_start(app, transcriber ? SETUP_TRANSCRIBER : SETUP_SUMMARY_AI);
        status_json(app, &r->body);
        app_unlock(app);
        wake_window(app);
        return;
    }

    if (str_eq(path, str("/clip"))) {
        Str range = {};
        if (!query_param(a, query, "range", &range)) range = str("15m");
        f64 now = now_ts(), from = 0;
        char label[64] = {};
        if (!clip_range(range, now, &from, label, sizeof(label))) {
            Str lower = str_lower(a, str_trim(range));
            answer_error(r, 400, str_fmt(a, "range must look like 15m, 1h, 2d or today (got '%s')", lower.ptr).ptr);
            return;
        }
        // Not under the lock: it reads the database and writes a file.
        ClipResult c = clip_copy(app, from, now, 0, CLIP_FILE, label);
        if (!c.ok) {
            answer_error(r, 400, c.error);
            return;
        }
        buf_fmt(&r->body, "{\"lines\":%d,\"path\":", c.lines);
        json_quote(&r->body, str(c.path));
        buf_cstr(&r->body, "}");
        return;
    }

    if (str_eq(path, str("/summary"))) {
        Str action = {}, day = {}, at = {};
        query_param(a, query, "do", &action);
        query_param(a, query, "day", &day);
        query_param(a, query, "at", &at);
        const char* known[] = {"now", "later", "until", "skip", "pick"};
        bool ok = false;
        for (i32 i = 0; i < COUNT(known); i++) ok |= str_eq(action, str(known[i]));
        if (!ok) {
            answer_error(r, 400, "do must be now, later, until or skip");
            return;
        }
        app_lock(app);
        Str result = summary_act(app, a, action.ptr, day.ptr, at.len ? at.ptr : 0);
        app_unlock(app);
        // summary_act answers in words; these ones are failures, as they were errors in the old app.
        const char* failures[] = {"bad day", "time must look like", "could not save", "do must be"};
        for (i32 i = 0; i < COUNT(failures); i++) {
            if (str_starts(result, str(failures[i]))) {
                answer_error(r, 400, result.ptr);
                return;
            }
        }
        buf_cstr(&r->body, "{\"result\":");
        json_quote(&r->body, result);
        buf_cstr(&r->body, "}");
        return;
    }

    if (str_eq(path, str("/remind"))) {
        Str id_text = {}, action = {};
        query_param(a, query, "id", &id_text);
        query_param(a, query, "do", &action);
        char* end = 0;
        i64 id = id_text.len ? _strtoi64(id_text.ptr, &end, 10) : 0;
        if (!id_text.len || *end) {
            answer_error(r, 400, "id must be a number");
            return;
        }
        const char* known[] = {"yes", "no", "done", "snooze"};
        bool ok = false;
        for (i32 i = 0; i < COUNT(known); i++) ok |= str_eq(action, str(known[i]));
        if (!ok) {
            answer_error(r, 400, "do must be yes, no, done or snooze");
            return;
        }
        // Whether there is such a reminder, read before taking the lock.
        bool found = false;
        sqlite3* db = db_open(app);
        if (db) {
            sqlite3_stmt* st = 0;
            if (sqlite3_prepare_v2(db, "SELECT 1 FROM alerts WHERE id = ?1", -1, &st, 0) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, id);
                found = sqlite3_step(st) == SQLITE_ROW;
            }
            sqlite3_finalize(st);
            sqlite3_close(db);
        }
        if (!found) {
            answer_error(r, 400, str_fmt(a, "no reminder %lld", id).ptr);
            return;
        }
        app_lock(app);
        reminder_answer(app, id, action.ptr);
        app_unlock(app);
        buf_cstr(&r->body, "{\"result\":");
        json_quote(&r->body, action);
        buf_cstr(&r->body, "}");
        return;
    }

    bool show = str_eq(path, str("/show")), popup = str_eq(path, str("/popup"));
    if (show || popup) {
        // The window's name with the day it is about, as the old app answered: "summary?day=…".
        Str name = {}, day = {};
        if (!query_param(a, query, show ? "tab" : "page", &name)) name = str("ui");
        bool has_day = query_param(a, query, "day", &day);
        Str shown = has_day ? str_fmt(a, "%s?day=%s", name.ptr, day.ptr) : name;
        const char* d = day.len == 10 ? day.ptr : 0;
        app_lock(app);
        if (show) {
            Tab tab = TAB_TIMELINE;
            if (str_eq(name, str("settings"))) tab = TAB_SETTINGS;
            else if (str_eq(name, str("summary")) || str_eq(name, str("journal"))) tab = TAB_JOURNAL;
            ui_show(app, tab, d);
        } else {
            ui_popup(app, str_eq(name, str("delay")) ? POPUP_DELAY : POPUP_TRAY, d);
        }
        app_unlock(app);
        buf_cstr(&r->body, "{\"shown\":");
        json_quote(&r->body, shown);
        buf_cstr(&r->body, "}");
        return;
    }

    if (str_eq(path, str("/quit"))) {
        buf_cstr(&r->body, "{\"stopping\":true}");
        r->after = AFTER_QUIT;
        return;
    }
    if (str_eq(path, str("/restart"))) {
        buf_cstr(&r->body, "{\"restarting\":true}");
        r->after = AFTER_RESTART;
        return;
    }
    answer_error(r, 404, "not found");
}

// ---------------------------------------------------------------------------------------------
// The server

// A header's value from the request's header lines, trimmed; empty if it is not there.
static Str header_value(Str head, const char* name) {
    Str n = str(name);
    i64 at = str_find(head, str("\r\n"));
    while (at >= 0 && at < head.len) {
        at += 2;
        i64 end = str_find(str_sub(head, at), str("\r\n"));
        Str line = str_sub(head, at, end < 0 ? -1 : end);
        if (line.len > n.len && line.ptr[n.len] == ':' && str_eq_nocase(str_sub(line, 0, n.len), n))
            return str_trim(str_sub(line, n.len + 1));
        if (end < 0) break;
        at += end;
    }
    return {"", 0};
}

// Only non-browser tools may call the API: the Host must be this server (stops DNS rebinding)
// and an Origin, if a browser sent one, must be this server too.
static bool trusted(Str head, u16 port) {
    Arena* a = scratch();
    Str ours[2] = {str_fmt(a, "127.0.0.1:%u", port), str_fmt(a, "localhost:%u", port)};
    Str host = header_value(head, "Host");
    Str origin = header_value(head, "Origin");
    bool host_ok = str_eq(host, ours[0]) || str_eq(host, ours[1]);
    bool origin_ok = origin.len == 0 || str_eq(origin, str_fmt(a, "http://%s", ours[0].ptr)) ||
                     str_eq(origin, str_fmt(a, "http://%s", ours[1].ptr));
    // A header sent without a value still counts as sent.
    if (origin.len == 0 && str_find(head, str("\r\nOrigin:")) >= 0) origin_ok = false;
    return host_ok && origin_ok;
}

static bool send_all(SOCKET s, const char* p, i64 n) {
    while (n > 0) {
        int sent = send(s, p, (int)(n > 65536 ? 65536 : n), 0);
        if (sent <= 0) return false;
        p += sent;
        n -= sent;
    }
    return true;
}

static const char* status_text(i32 code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    }
    return "Error";
}

// Reads one request from `c`, answers it, and returns what is left to do afterwards.
static After api_serve_one(App* app, SOCKET c, u16 port) {
    Mark m = mark(scratch());
    DWORD timeout = 5000;  // a client that sends nothing must not hold up the next one
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    static const i32 CAP = 16384;
    char* buf = PUSH_ARRAY(scratch(), char, CAP + 1);
    i32 got = 0;
    i64 head_end = -1;
    while (got < CAP) {
        int n = recv(c, buf + got, CAP - got, 0);
        if (n <= 0) break;
        got += n;
        buf[got] = 0;
        head_end = str_find({buf, got}, str("\r\n\r\n"));
        if (head_end >= 0) break;
    }
    Answer r = {};
    if (head_end < 0) {
        answer_error(&r, 400, "bad request");
    } else {
        Str head = {buf, head_end};
        // "METHOD /path?query HTTP/1.1"
        i64 sp1 = str_find(head, str(" "));
        Str rest = sp1 >= 0 ? str_sub(head, sp1 + 1) : Str{};
        i64 sp2 = str_find(rest, str(" "));
        if (sp1 <= 0 || sp2 <= 0) {
            answer_error(&r, 400, "bad request");
        } else if (!trusted(head, port)) {
            answer_error(&r, 403, "not allowed from here");
        } else {
            Str method = str_copy(scratch(), str_sub(head, 0, sp1));
            Str target = str_sub(rest, 0, sp2);
            i64 q = str_find(target, str("?"));
            Str path = str_copy(scratch(), q >= 0 ? str_sub(target, 0, q) : target);
            Str query = str_copy(scratch(), q >= 0 ? str_sub(target, q + 1) : Str{"", 0});
            api_handle(app, method, path, query, &r);
        }
    }
    Str body = buf_take(&r.body, scratch());
    Str reply = str_fmt(scratch(),
                        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
                        "Content-Length: %lld\r\nConnection: close\r\n\r\n%s",
                        r.code, status_text(r.code), body.len, body.ptr);
    send_all(c, reply.ptr, reply.len);
    rewind(m);
    return r.after;
}

static DWORD WINAPI api_main(void* param) {
    App* app = (App*)param;
    app_lock(app);
    u16 port = app->config.api_port;
    app_unlock(app);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        ERR("API: Winsock did not start");
        return 0;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // The port can stay busy for a few seconds after a restart or recent connections, so try
    // for up to 30 s before giving up.
    SOCKET s = INVALID_SOCKET;
    i32 error = 0;
    for (i32 attempt = 0; attempt < 30; attempt++) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL exclusive = TRUE;  // no other program may share the port
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
        if (s != INVALID_SOCKET && bind(s, (sockaddr*)&addr, sizeof(addr)) == 0 && listen(s, 16) == 0) break;
        error = WSAGetLastError();
        if (s != INVALID_SOCKET) closesocket(s);
        s = INVALID_SOCKET;
        if (!sleep_unless_stopping(app, 1000)) return 0;
    }
    if (s == INVALID_SOCKET) {
        ERR("API port %u: could not listen (error %d); the CLI and the Stream Deck cannot reach lifelog", port, error);
        return 0;
    }
    LOG("API on http://127.0.0.1:%u", port);
    while (!stopping(app)) {
        // Wakes twice a second to see whether the program is ending.
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(s, &ready);
        timeval wait = {0, 500 * 1000};
        if (select(0, &ready, 0, 0, &wait) <= 0) continue;
        SOCKET c = accept(s, 0, 0);
        if (c == INVALID_SOCKET) continue;
        After after = api_serve_one(app, c, port);
        shutdown(c, SD_SEND);
        closesocket(c);
        if (after != AFTER_NOTHING) {
            LOG("API: %s", after == AFTER_QUIT ? "quit" : "restart");
            app_lock(app);
            if (after == AFTER_QUIT) app_quit(app);
            else app_restart(app);
            app_unlock(app);
        }
    }
    closesocket(s);
    return 0;
}

// Starts the API thread. With the lock.
void api_start(App* app) {
    if (!app->config.api_port) {
        WARN("api_port is 0: no API, so the CLI and the Stream Deck cannot reach lifelog");
        return;
    }
    start_thread(app, "api", api_main);
}

// ---------------------------------------------------------------------------------------------
// The client, for the CLI

// Calls the running lifelog's API and puts the answer's body in `body`. False (with `error`
// set) if nothing answers on the port.
static bool api_call(u16 port, const char* method, Str path, Arena* a, Str* body, char* error, i32 cap) {
    *body = {"", 0};
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        snprintf(error, cap, "lifelog service is not running (nothing on port %u)", port);
        if (s != INVALID_SOCKET) closesocket(s);
        return false;
    }
    DWORD timeout = 120000;  // copying a long clip can take a while
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    Str request = str_fmt(a, "%s %.*s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", method,
                          (int)path.len, path.ptr, port);
    if (!send_all(s, request.ptr, request.len)) {
        snprintf(error, cap, "could not send to lifelog on port %u (error %d)", port, WSAGetLastError());
        closesocket(s);
        return false;
    }
    Buf got = {};
    char chunk[4096];
    for (;;) {
        int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf_add(&got, {chunk, n});
    }
    closesocket(s);
    Str all = buf_take(&got, a);
    i64 split = str_find(all, str("\r\n\r\n"));
    *body = split >= 0 ? str_sub(all, split + 4) : all;
    return true;
}
