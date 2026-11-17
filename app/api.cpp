// api.cpp: the small HTTP API on 127.0.0.1:<api_port> for the CLI, the Stream Deck and scripts.
// The window never uses it.
//
//   GET  /status            what is being captured, paused or not, queue length
//   POST /pause | /resume | /toggle     private mode
//   POST /exclude | /include | /game | /not-game ?app=X.exe
//   POST /transcription?mode=auto|always|paused     when the GPU work may run
//   POST /setup?part=transcriber|summary    download and set up that AI (as Settings' "Set up")
//   POST /clip?range=15m    write the clip file and put it on the clipboard
//   POST /summary?do=now|later|skip|until|pick[&day=][&at=]     pick opens the delay box
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

static void json_wide(Buf* b, const wchar_t* w) {
    Mark m = mark(scratch());
    json_quote(b, narrow(scratch(), w));
    rewind(m);
}

// GET /status, the same fields as the old app's. The main thread.
static void status_json(App* app, Buf* b) {
    Config* c = &app->config;
    char today[11];
    day_of(now_ts(), today);
    const SummaryDay* today_row = summary_day_find(app, today);
    const char* today_status = today_row ? today_row->status : 0;

    buf_cstr(b, "{\"excluded_apps\":");
    json_names(b, &c->capture.excluded_apps);
    buf_cstr(b, ",\"game_running\":");
    json_quote_or_null(b, app->apps.game_running);
    buf_cstr(b, ",\"games\":");
    json_names(b, &c->processing.games);
    buf_fmt(b, ",\"gpu_3d_percent\":%u,\"gpu_busy_percent\":%u", app->worker.gpu_3d_percent, c->processing.gpu_busy_percent);
    // The rest of what the GPU monitor reads (worker.h); free_mb is -1 while the adapter is unknown.
    buf_fmt(b, ",\"gpu_compute_percent\":%ld,\"gpu_free_mb\":%ld,\"gpu_need_mb\":%ld,\"gpu_short\":%s",
            atomic_get(&app->worker.gpu_compute_percent), atomic_get(&app->worker.gpu_free_mb),
            atomic_get(&app->worker.gpu_need_mb), atomic_get(&app->worker.gpu_short) ? "true" : "false");
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
        char device[128], title[256];
        AcquireSRWLockShared(&s->lock);
        SET(device, str(s->device));
        SET(title, str(s->title));
        ReleaseSRWLockShared(&s->lock);
        buf_cstr(b, "{\"device\":");
        json_quote_or_null(b, mic ? device : 0);
        buf_cstr(b, ",\"exe\":");
        json_quote_or_null(b, mic ? 0 : s->exe);
        if (mic) buf_cstr(b, ",\"pid\":null");
        else buf_fmt(b, ",\"pid\":%u", s->pid);
        buf_fmt(b, ",\"source\":\"%s\",\"title\":", mic ? "mic" : "app");
        json_quote_or_null(b, mic ? 0 : title);
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
    json_quote_or_null(b, today_status);
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

// A request handed to the main thread. The API thread waits for `done`; both threads hold the
// call (`refs`) and the last to let go frees it, so an answer that comes after the API thread
// gave up waiting is just dropped.
struct ApiCall {
    volatile LONG refs;
    HANDLE done;  // set once `r` is the answer
    Arena arena;  // method, path and query
    Str method, path, query;
    Answer r;
};

static void api_release(ApiCall* c) {
    if (InterlockedDecrement(&c->refs) != 0) return;
    buf_free(&c->r.body);
    arena_release(&c->arena);
    CloseHandle(c->done);
    free(c);
}

// The main thread: `c->r` is the answer.
static void api_finish(ApiCall* c) {
    SetEvent(c->done);
    api_release(c);
}

static void api_clip_done(App*, ClipResult* r, ClipKind, i64 ctx) {
    ApiCall* c = (ApiCall*)ctx;
    if (!r->ok) {
        answer_error(&c->r, 400, r->error);
    } else {
        buf_fmt(&c->r.body, "{\"lines\":%d,\"path\":", r->lines);
        json_quote(&c->r.body, str(r->path));
        buf_cstr(&c->r.body, "}");
    }
    api_finish(c);
}

// Whether there is such a reminder, asked of the database thread before answering it.
struct RemindCheck {
    ApiCall* call;
    i64 id;
    char action[16];
    bool found;
};

static void remind_check_run(sqlite3* db, Job* job) {
    RemindCheck* k = (RemindCheck*)job->data;
    sqlite3_stmt* st = db ? prepare(db, "SELECT 1 FROM alerts WHERE id = ?1") : 0;
    if (st) {
        sqlite3_bind_int64(st, 1, k->id);
        k->found = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
}

static void remind_check_apply(App* app, Job* job) {
    RemindCheck* k = (RemindCheck*)job->data;
    Answer* r = &k->call->r;
    if (!k->found) {
        answer_error(r, 400, str_fmt(scratch(), "no reminder %lld", k->id).ptr);
    } else {
        reminder_answer(app, k->id, k->action);
        buf_cstr(&r->body, "{\"result\":");
        json_quote(&r->body, str(k->action));
        buf_cstr(&r->body, "}");
    }
    api_finish(k->call);
}

// The main thread: does what the call asks. True when `c->r` is the answer now; false when a job
// will answer it (api_finish) once it is done.
static bool api_handle(App* app, ApiCall* c) {
    Arena* a = scratch();
    Str method = c->method, path = c->path, query = c->query;
    Answer* r = &c->r;
    r->code = 200;
    bool get = str_eq(method, str("GET")), post = str_eq(method, str("POST"));
    Str exe = {};
    query_param(a, query, "app", &exe);
    exe = str_lower(a, str_trim(exe));

    if (get && str_eq(path, str("/status"))) {
        status_json(app, &r->body);
        return true;
    }
    if (!post) {
        answer_error(r, 404, "not found");
        return true;
    }

    // The changes, each answering with the new status.
    bool pause = str_eq(path, str("/pause")), resume = str_eq(path, str("/resume")), toggle = str_eq(path, str("/toggle"));
    bool exclude = str_eq(path, str("/exclude")), include = str_eq(path, str("/include"));
    bool game = str_eq(path, str("/game")), not_game = str_eq(path, str("/not-game"));
    bool transcription = str_eq(path, str("/transcription"));
    if (pause || resume || toggle || exclude || include || game || not_game || transcription) {
        if ((exclude || include || game || not_game) && exe.len == 0) {
            answer_error(r, 400, "say which app, e.g. spotify.exe");
            return true;
        }
        Mode mode = MODE_AUTO;
        if (transcription) {
            Str m = {};
            if (!query_param(a, query, "mode", &m) || !mode_from_text(m, &mode)) {
                answer_error(r, 400, "mode must be auto, always or paused");
                return true;
            }
        }
        if (pause || resume || toggle) {
            capture_set_paused(app, toggle ? !app->capture.paused : pause);
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
        return true;
    }

    if (str_eq(path, str("/setup"))) {
        Str part = {};
        query_param(a, query, "part", &part);
        bool transcriber = str_eq(part, str("transcriber")), summary = str_eq(part, str("summary"));
        if (!transcriber && !summary) {
            answer_error(r, 400, "part must be transcriber or summary");
            return true;
        }
        setup_start(app, transcriber ? SETUP_TRANSCRIBER : SETUP_SUMMARY_AI, transcriber ? app->config.worker.model : app->config.summary.model);
        status_json(app, &r->body);
        wake_window(app);
        return true;
    }

    if (str_eq(path, str("/clip"))) {
        Str range = {};
        if (!query_param(a, query, "range", &range)) range = str("15m");
        f64 now = now_ts(), from = 0;
        char label[64] = {};
        if (!clip_range(range, now, &from, label, sizeof(label))) {
            Str lower = str_lower(a, str_trim(range));
            answer_error(r, 400, str_fmt(a, "range must look like 15m, 1h, 2d or today (got '%s')", lower.ptr).ptr);
            return true;
        }
        // On the pool: it reads the database and writes a file.
        clip_copy_later(app, from, now, Names{}, CLIP_FILE, label, api_clip_done, (i64)c);
        return false;
    }

    if (str_eq(path, str("/summary"))) {
        Str action = {}, day = {}, at = {};
        query_param(a, query, "do", &action);
        query_param(a, query, "day", &day);
        query_param(a, query, "at", &at);
        // summary_act checks the action (ACTED_BAD_ACTION).
        SummaryActResult acted = summary_act(app, action.ptr, day.ptr, at.len ? at.ptr : 0);
        char said[64];
        const char* result = summary_acted_text(acted, said, sizeof(said));
        if (acted_failed(acted)) {
            answer_error(r, 400, result);
            return true;
        }
        buf_cstr(&r->body, "{\"result\":");
        json_quote(&r->body, str(result));
        buf_cstr(&r->body, "}");
        return true;
    }

    if (str_eq(path, str("/remind"))) {
        Str id_text = {}, action = {};
        query_param(a, query, "id", &id_text);
        query_param(a, query, "do", &action);
        char* end = 0;
        i64 id = id_text.len ? _strtoi64(id_text.ptr, &end, 10) : 0;
        if (!id_text.len || *end) {
            answer_error(r, 400, "id must be a number");
            return true;
        }
        const char* known[] = {"yes", "no", "done", "snooze"};
        bool ok = false;
        for (i32 i = 0; i < COUNT(known); i++) ok |= str_eq(action, str(known[i]));
        if (!ok) {
            answer_error(r, 400, "do must be yes, no, done or snooze");
            return true;
        }
        Job* job = job_new(remind_check_apply);
        RemindCheck* k = PUSH(&job->arena, RemindCheck);
        k->call = c;
        k->id = id;
        SET(k->action, action);
        job->data = k;
        db_post(job, remind_check_run);
        return false;
    }

    bool show = str_eq(path, str("/show")), popup = str_eq(path, str("/popup"));
    if (show || popup) {
        // The window's name with the day it is about, as the old app answered: "summary?day=…".
        Str name = {}, day = {};
        if (!query_param(a, query, show ? "tab" : "page", &name)) name = str("ui");
        bool has_day = query_param(a, query, "day", &day);
        Str shown = has_day ? str_fmt(a, "%s?day=%s", name.ptr, day.ptr) : name;
        const char* d = day.len == 10 ? day.ptr : 0;
        if (show) {
            Tab tab = TAB_TIMELINE;
            if (str_eq(name, str("settings"))) tab = TAB_SETTINGS;
            else if (str_eq(name, str("summary")) || str_eq(name, str("journal"))) tab = TAB_JOURNAL;
            ui_show(app, tab, d);
        } else {
            ui_popup(app, str_eq(name, str("delay")) ? POPUP_DELAY : POPUP_TRAY, d);
        }
        buf_cstr(&r->body, "{\"shown\":");
        json_quote(&r->body, shown);
        buf_cstr(&r->body, "}");
        return true;
    }

    if (str_eq(path, str("/quit"))) {
        buf_cstr(&r->body, "{\"stopping\":true}");
        r->after = AFTER_QUIT;
        return true;
    }
    if (str_eq(path, str("/restart"))) {
        buf_cstr(&r->body, "{\"restarting\":true}");
        r->after = AFTER_RESTART;
        return true;
    }
    answer_error(r, 404, "not found");
    return true;
}

static void api_apply(App* app, Event* e) {
    ApiCall* c = *event_record<ApiCall*>(e);
    Mark m = mark(scratch());
    if (api_handle(app, c)) api_finish(c);
    rewind(m);
}

// The main thread: an answer has gone out that asked to quit or restart.
static void api_after_apply(App* app, Event* e) {
    After after = *event_record<After>(e);
    LOG("API: %s", after == AFTER_QUIT ? "quit" : "restart");
    if (after == AFTER_QUIT) app_quit(app);
    else app_restart(app);
}

// The API thread: hands the request to the main thread and waits for its answer.
static void api_ask(App* app, Str method, Str path, Str query, Answer* out) {
    ApiCall* c = (ApiCall*)calloc(1, sizeof(ApiCall));
    CHECK(c, "memory for an API call");
    c->refs = 2;
    c->done = CreateEventW(0, TRUE, FALSE, 0);
    CHECK(c->done, "an API call's event (error %lu)", GetLastError());
    c->method = str_copy(&c->arena, method);
    c->path = str_copy(&c->arena, path);
    c->query = str_copy(&c->arena, query);
    if (!event_send(app, api_apply, c)) {
        c->refs = 1;
        api_release(c);
        answer_error(out, 503, "lifelog is busy, try again");
        return;
    }
    // Copying audio can take a while; anything else is answered at once.
    HANDLE waits[2] = {c->done, app->stop};
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, 120000);
    if (w == WAIT_OBJECT_0) {
        out->code = c->r.code;
        out->after = c->r.after;
        out->body = c->r.body;
        c->r.body = {};
    } else {
        answer_error(out, 503, w == WAIT_OBJECT_0 + 1 ? "lifelog is stopping" : "lifelog did not answer in time");
    }
    api_release(c);
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
    case 503: return "Service Unavailable";
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
            api_ask(app, method, path, query, &r);
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
    u16 port = api_port();
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
    // Sleeps until a connection comes or the program ends.
    WSAEVENT ready = WSACreateEvent();
    if (ready == WSA_INVALID_EVENT || WSAEventSelect(s, ready, FD_ACCEPT) != 0) {
        ERR("API port %u: could not wait for connections (error %d)", port, WSAGetLastError());
        if (ready != WSA_INVALID_EVENT) WSACloseEvent(ready);
        closesocket(s);
        return 0;
    }
    LOG("API on http://127.0.0.1:%u", port);
    while (!stopping(app)) {
        HANDLE waits[2] = {app->stop, ready};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
        WSANETWORKEVENTS seen;
        WSAEnumNetworkEvents(s, ready, &seen);  // resets `ready`
        for (;;) {
            SOCKET c = accept(s, 0, 0);
            if (c == INVALID_SOCKET) break;  // none left (WSAEWOULDBLOCK)
            // It inherits the listening socket's event and non-blocking mode: api_serve_one
            // wants a plain blocking socket.
            WSAEventSelect(c, 0, 0);
            u_long nonblocking = 0;
            ioctlsocket(c, FIONBIO, &nonblocking);
            After after = api_serve_one(app, c, port);
            shutdown(c, SD_SEND);
            closesocket(c);
            if (after != AFTER_NOTHING) event_send(app, api_after_apply, after);
            if (stopping(app)) break;
        }
    }
    WSACloseEvent(ready);
    closesocket(s);
    return 0;
}

// Starts the API thread. The main thread.
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
