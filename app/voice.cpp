// voice.cpp: see voice.h.

static const i32 VOICE_RATE = 16000;
static const i32 VOICE_SHORTEST = VOICE_RATE / 4;  // under 250 ms is not a command
static const i32 VOICE_LONGEST = 12 * VOICE_RATE;  // of a longer sentence, only its last 12 s
static const f64 VOICE_WAIT_SECS = 6;              // after the name alone

void voice_heard(App* app, const f32* samples, i32 n, f32 gain) {
    Voice* v = &app->voice;
    if (!v->wake || !atomic_get(&v->listen) || atomic_get(&v->state) != VOICE_READY) return;
    if (n < VOICE_SHORTEST) return;
    if (n > VOICE_LONGEST) {
        samples += n - VOICE_LONGEST;
        n = VOICE_LONGEST;
    }
    f32* copy = (f32*)malloc(sizeof(f32) * n);
    if (!copy) return;
    for (i32 i = 0; i < n; i++) {
        f32 s = samples[i] * gain;
        copy[i] = s < -1 ? -1 : s > 1 ? 1 : s;
    }
    AcquireSRWLockExclusive(&v->lock);
    if (v->count == COUNT(v->queue)) {  // behind: the oldest goes
        free(v->queue[v->head].samples);
        v->head = (v->head + 1) % COUNT(v->queue);
        v->count--;
    }
    v->queue[(v->head + v->count) % COUNT(v->queue)] = {copy, n};
    v->count++;
    ReleaseSRWLockExclusive(&v->lock);
    SetEvent(v->wake);
}

static bool voice_take(Voice* v, VoiceSentence* out) {
    AcquireSRWLockExclusive(&v->lock);
    bool got = v->count > 0;
    if (got) {
        *out = v->queue[v->head];
        v->head = (v->head + 1) % COUNT(v->queue);
        v->count--;
    }
    ReleaseSRWLockExclusive(&v->lock);
    return got;
}

static void voice_drop_all(Voice* v) {
    VoiceSentence s;
    while (voice_take(v, &s)) free(s.samples);
}

void voice_set_listen(App* app, bool on) {
    app->config.voice.listen = on;
    config_save_bool("voice", "listen", on);
    atomic_set(&app->voice.listen, on);
    LOG("voice commands %s", on ? "on" : "off");
    if (app->voice.wake) SetEvent(app->voice.wake);
}

// ---------------------------------------------------------------------------------------------
// What was said, as words: lower case, letters and digits ("Lifelog, clip the last 15 minutes." is
// lifelog clip the last 15 minutes). An apostrophe joins ("today's" is todays).

struct Words {
    Str w[48];
    i32 n;
};

static void words_of(Arena* a, Str text, Words* out) {
    out->n = 0;
    char* buf = PUSH_ARRAY(a, char, text.len + 1);
    i64 len = 0;
    for (i64 i = 0; i <= text.len; i++) {
        char c = i < text.len ? text.ptr[i] : ' ';
        bool keep = isalnum((u8)c) != 0;
        if (keep) {
            buf[len++] = (char)tolower((u8)c);
            continue;
        }
        if (c == '\'' && len) continue;
        if (len && out->n < COUNT(out->w)) out->w[out->n++] = str_copy(a, {buf, len});
        len = 0;
    }
}

static bool word_is(Str w, const char* const* list, i32 count) {
    for (i32 i = 0; i < count; i++)
        if (str_eq(w, str(list[i]))) return true;
    return false;
}
template <typename... T>
static bool word_in(Str w, T... list) {
    const char* const l[] = {list...};
    return word_is(w, l, (i32)sizeof...(list));
}

static bool has(const Words* ws, const char* w) {
    for (i32 i = 0; i < ws->n; i++)
        if (str_eq(ws->w[i], str(w))) return true;
    return false;
}

// Where the word starting with `prefix` is, or -1.
static i32 find_prefix(const Words* ws, const char* prefix) {
    for (i32 i = 0; i < ws->n; i++)
        if (str_starts(ws->w[i], str(prefix))) return i;
    return -1;
}

static bool has_pair(const Words* ws, const char* a, const char* b) {
    for (i32 i = 0; i + 1 < ws->n; i++)
        if (str_eq(ws->w[i], str(a)) && str_eq(ws->w[i + 1], str(b))) return true;
    return false;
}

// How many words the wake name takes at `i` (Whisper writes it a few ways), or 0.
static i32 wake_at(const Words* ws, i32 i) {
    Str w = ws->w[i];
    if (word_in(w, "lifelog", "lifelogs", "lifelock", "lifelocks", "livelog", "lifeblog", "lifeleg", "lifelig")) return 1;
    if (i + 1 < ws->n && word_in(w, "life", "live", "lie") && word_in(ws->w[i + 1], "log", "logs", "lock", "locks", "blog"))
        return 2;
    return 0;
}

// The words after `from` and up to `to`, with the polite and filler ones dropped.
static void words_between(const Words* ws, i32 from, i32 to, Words* out) {
    out->n = 0;
    for (i32 i = from; i < to; i++) {
        Str w = ws->w[i];
        if (word_in(w, "please", "can", "could", "would", "you", "will", "hey", "ok", "okay", "um", "uh", "just", "now")) continue;
        out->w[out->n++] = w;
    }
}

// ---------------------------------------------------------------------------------------------
// Commands

enum VoiceDo : i32 {
    DO_NOTHING,   // not a command
    DO_API,       // through the API: `method`, `path`, `query`
    DO_CANCEL,
    DO_CONFIG,    // open config.toml
    DO_CHECK_AI,
};

struct VoiceCommand {
    VoiceDo what;
    const char* method;
    char path[32];
    char query[256];
    Chime chime;      // when it worked
    bool private_ok;  // carried out in private mode too
    char said[96];    // what it does, for the log and Settings: "clip the last 1h"
};

static void command(VoiceCommand* c, const char* method, const char* path, const char* said) {
    c->what = DO_API;
    c->method = method;
    snprintf(c->path, sizeof(c->path), "%s", path);
    snprintf(c->said, sizeof(c->said), "%s", said);
    c->chime = CHIME_DONE;
}

// A number said as words or digits, at `*i` (moved past it): "fifteen", "twenty five", "15", "an".
// 0 if there is none.
static i32 number_at(const Words* ws, i32* i) {
    static const char* ones[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                 "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
    static const char* tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    if (*i >= ws->n) return 0;
    Str w = ws->w[*i];
    if (isdigit((u8)w.ptr[0])) {
        i32 n = 0;
        for (i64 k = 0; k < w.len && isdigit((u8)w.ptr[k]); k++) n = n * 10 + (w.ptr[k] - '0');
        // "15m" or "2h", as Whisper sometimes writes it, leaves the unit for unit_at.
        (*i)++;
        return n > 999 ? 999 : n;
    }
    if (word_in(w, "a", "an")) {
        (*i)++;
        return 1;
    }
    if (word_in(w, "couple")) {
        (*i)++;
        if (*i < ws->n && str_eq(ws->w[*i], str("of"))) (*i)++;
        return 2;
    }
    if (word_in(w, "few")) {
        (*i)++;
        return 3;
    }
    for (i32 k = 1; k < COUNT(ones); k++)
        if (str_eq(w, str(ones[k]))) {
            (*i)++;
            return k;
        }
    for (i32 k = 2; k < COUNT(tens); k++)
        if (str_eq(w, str(tens[k]))) {
            (*i)++;
            i32 n = k * 10;
            if (*i < ws->n)
                for (i32 j = 1; j < 10; j++)
                    if (str_eq(ws->w[*i], str(ones[j]))) {
                        (*i)++;
                        n += j;
                        break;
                    }
            return n;
        }
    return 0;
}

// The unit at `i`: 'm', 'h' or 'd', or 0.
static char unit_of(Str w) {
    if (word_in(w, "minute", "minutes", "min", "mins")) return 'm';
    if (word_in(w, "hour", "hours", "hr", "hrs")) return 'h';
    if (word_in(w, "day", "days")) return 'd';
    return 0;
}

// The stretch said for a clip, as clip_range takes it: "15m", "2h", "today". Default 15m.
static void clip_range_said(const Words* ws, char* out, i32 cap) {
    snprintf(out, cap, "15m");
    if (has(ws, "today") || has(ws, "todays")) {
        snprintf(out, cap, "today");
        return;
    }
    if (has_pair(ws, "half", "an") || has_pair(ws, "half", "hour")) {
        snprintf(out, cap, "30m");
        return;
    }
    for (i32 i = 0; i < ws->n; i++) {
        // "15m" and "2h" in one word.
        Str w = ws->w[i];
        if (w.len >= 2 && isdigit((u8)w.ptr[0]) && strchr("mhd", w.ptr[w.len - 1])) {
            bool digits = true;
            for (i64 k = 0; k < w.len - 1; k++) digits &= isdigit((u8)w.ptr[k]) != 0;
            if (digits) {
                snprintf(out, cap, "%.*s", (int)w.len, w.ptr);
                return;
            }
        }
        char unit = unit_of(w);
        if (!unit) continue;
        i32 n = 0;
        // The number before the unit: one word ("fifteen", "15") or two ("twenty five").
        for (i32 back = 2; back >= 1 && !n; back--) {
            i32 from = i - back;
            if (from < 0) continue;
            i32 j = from;
            i32 got = number_at(ws, &j);
            if (got && j == i) n = got;
        }
        if (!n) n = 1;  // "the last hour"
        bool and_a_half = i + 3 < ws->n && str_eq(ws->w[i + 1], str("and")) && str_eq(ws->w[i + 2], str("a")) &&
                          str_eq(ws->w[i + 3], str("half"));
        if (and_a_half && unit == 'h') snprintf(out, cap, "%dm", n * 60 + 30);
        else snprintf(out, cap, "%d%c", n, unit);
        return;
    }
}

// The app named in `ws` from word `from` on: "spotify", "visual studio code".
static void app_said(const Words* ws, i32 from, i32 to, char* out, i32 cap) {
    out[0] = 0;
    i32 len = 0;
    for (i32 i = from; i < to && i < ws->n; i++) {
        Str w = ws->w[i];
        if (word_in(w, "the", "app", "application", "program", "game", "as", "a", "mark", "from", "to", "recording", "list", "not",
                    "no", "isnt", "is", "its", "an")) continue;
        len += snprintf(out + len, cap - len, "%s%.*s", len ? " " : "", (int)w.len, w.ptr);
        if (len >= cap) break;
    }
}

// What the words after the wake name ask for.
static void parse_command(const Words* ws, VoiceCommand* c) {
    *c = {};
    if (!ws->n) return;
    Str first = ws->w[0];
    Arena* a = scratch();

    if (word_in(first, "cancel", "nevermind", "nothing", "stop") && ws->n == 1) {
        c->what = DO_CANCEL;
        c->private_ok = true;
        return;
    }
    if (has_pair(ws, "never", "mind")) {
        c->what = DO_CANCEL;
        c->private_ok = true;
        return;
    }
    if ((has(ws, "check") || has(ws, "test")) && (has(ws, "ai") || has_pair(ws, "a", "i"))) {
        c->what = DO_CHECK_AI;
        snprintf(c->said, sizeof(c->said), "check the AI");
        return;
    }
    if (find_prefix(ws, "transcri") >= 0) {
        Mode mode;
        if (has(ws, "auto") || has(ws, "automatic") || has(ws, "on") || has(ws, "resume") || has(ws, "start")) mode = MODE_AUTO;
        else if (has(ws, "always")) mode = MODE_ALWAYS;
        else if (has(ws, "pause") || has(ws, "paused") || has(ws, "off") || has(ws, "stop")) mode = MODE_PAUSED;
        else return;
        command(c, "POST", "/transcription", "");
        snprintf(c->query, sizeof(c->query), "mode=%s", mode_name(mode));
        snprintf(c->said, sizeof(c->said), "transcription %s", mode_name(mode));
        return;
    }
    bool open = word_in(first, "open", "show");
    if ((open || ws->n == 1) && (has(ws, "journal") || (open && find_prefix(ws, "summar") >= 0))) {
        command(c, "POST", "/show", "open the journal");
        snprintf(c->query, sizeof(c->query), "tab=journal");
        return;
    }
    if (find_prefix(ws, "summar") >= 0) {
        const char* action = "now";
        if (has(ws, "later") || has(ws, "postpone") || has(ws, "delay") || has(ws, "tomorrow")) action = "later";
        else if (has(ws, "skip")) action = "skip";
        char day[11] = {};
        if (has(ws, "yesterday") || has(ws, "yesterdays")) day_of(now_ts() - 86400, day);
        command(c, "POST", "/summary", "");
        snprintf(c->query, sizeof(c->query), "do=%s&day=%s", action, day);
        snprintf(c->said, sizeof(c->said), "summary %s%s%s", action, day[0] ? " for " : "", day);
        return;
    }
    if (has(ws, "clip") || has(ws, "clips") || word_in(first, "copy", "save")) {
        char range[16];
        clip_range_said(ws, range, sizeof(range));
        command(c, "POST", "/clip", "");
        snprintf(c->query, sizeof(c->query), "range=%s", range);
        snprintf(c->said, sizeof(c->said), "clip %s", range);
        c->chime = CHIME_CLIP_SAVED;
        return;
    }

    // An app's decision or game mark: "exclude spotify", "spotify is a game", "valorant isn't a game".
    char app[128];
    i32 is_at = -1;
    for (i32 i = 0; i < ws->n; i++)
        if (word_in(ws->w[i], "is", "isnt", "its")) is_at = i;
    i32 game_at = -1;
    for (i32 i = 0; i < ws->n; i++)
        if (str_eq(ws->w[i], str("game"))) game_at = i;
    if (word_in(first, "exclude", "ignore", "include") || (str_eq(first, str("record")) && ws->n > 1 && !has(ws, "again"))) {
        bool exclude = !word_in(first, "include", "record");
        app_said(ws, 1, ws->n, app, sizeof(app));
        if (!app[0]) return;
        command(c, "POST", exclude ? "/exclude" : "/include", "");
        snprintf(c->query, sizeof(c->query), "app=%s", url_enc(a, str(app)).ptr);
        snprintf(c->said, sizeof(c->said), "%s %s", exclude ? "exclude" : "include", app);
        return;
    }
    if (has_pair(ws, "stop", "recording") && ws->n > 2) {
        app_said(ws, 2, ws->n, app, sizeof(app));
        if (app[0]) {
            command(c, "POST", "/exclude", "");
            snprintf(c->query, sizeof(c->query), "app=%s", url_enc(a, str(app)).ptr);
            snprintf(c->said, sizeof(c->said), "exclude %s", app);
            return;
        }
    }
    if (game_at >= 0) {
        bool not_game = has(ws, "not") || has(ws, "isnt") || has(ws, "no");
        // "X is (not) a game", or "game X" / "not a game X".
        if (is_at > 0) app_said(ws, 0, is_at, app, sizeof(app));
        else app_said(ws, game_at + 1, ws->n, app, sizeof(app));
        if (!app[0]) app_said(ws, 0, game_at, app, sizeof(app));  // "mark valorant as a game"
        if (!app[0]) return;
        command(c, "POST", not_game ? "/not-game" : "/game", "");
        snprintf(c->query, sizeof(c->query), "app=%s", url_enc(a, str(app)).ptr);
        snprintf(c->said, sizeof(c->said), "%s %s", app, not_game ? "is not a game" : "is a game");
        return;
    }

    // Private mode. Resume comes first: "stop private mode" is a resume.
    bool private_word = has(ws, "private") || has(ws, "privacy");
    bool off = has(ws, "off") || has(ws, "stop") || has(ws, "end") || has(ws, "leave") || has(ws, "exit") || has(ws, "disable");
    if (word_in(first, "resume", "unpause", "continue") || has_pair(ws, "start", "recording") ||
        has_pair(ws, "keep", "recording") || (has(ws, "record") && has(ws, "again")) || (private_word && off)) {
        command(c, "POST", "/resume", "resume recording");
        c->private_ok = true;
        return;
    }
    if (word_in(first, "pause") || private_word || has_pair(ws, "stop", "recording")) {
        command(c, "POST", "/pause", "private mode on");
        return;
    }
    if (word_in(first, "toggle")) {
        command(c, "POST", "/toggle", "toggle private mode");
        c->private_ok = true;
        return;
    }
    if (word_in(first, "restart", "reboot", "reload")) {
        command(c, "POST", "/restart", "restart");
        return;
    }
    if (word_in(first, "quit", "exit", "shutdown") || has_pair(ws, "shut", "down")) {
        command(c, "POST", "/quit", "quit");
        return;
    }
    if (has(ws, "config") || has(ws, "configuration")) {
        c->what = DO_CONFIG;
        snprintf(c->said, sizeof(c->said), "open config.toml");
        return;
    }
    if (has(ws, "status")) {
        command(c, "POST", "/popup", "show the status");
        snprintf(c->query, sizeof(c->query), "page=tray");
        return;
    }
    if (open || has(ws, "settings") || has(ws, "timeline") || has(ws, "window")) {
        const char* tab = has(ws, "settings") ? "settings" : "ui";
        command(c, "POST", "/show", has(ws, "settings") ? "open Settings" : "open lifelog");
        snprintf(c->query, sizeof(c->query), "tab=%s", tab);
        return;
    }
}

// What a sentence is: for us when it starts with the name (after a "hey" or "ok"), or comes
// within the wait after the name alone (`armed`). The name said in the middle of a sentence ("I
// was working on lifelog") is someone talking about it, not to it.
struct Understood {
    bool for_us;
    bool name_only;  // the name and nothing else: chime and wait for the command
    VoiceCommand c;  // DO_NOTHING when the rest is not a command
};

static void voice_understand(Str text, bool armed, Understood* u) {
    *u = {};
    Arena* a = scratch();
    Words ws;
    words_of(a, text, &ws);
    i32 at = 0;
    while (at < ws.n && word_in(ws.w[at], "hey", "hi", "ok", "okay", "so", "um", "uh", "oh", "yo", "and")) at++;
    i32 wake_n = at < ws.n ? wake_at(&ws, at) : 0;
    if (!wake_n && !armed) return;
    u->for_us = true;
    Words rest;
    words_between(&ws, wake_n ? at + wake_n : 0, ws.n, &rest);
    u->name_only = wake_n && rest.n == 0;
    parse_command(&rest, &u->c);
}

// ---------------------------------------------------------------------------------------------
// Results, on the main thread

struct VoiceDone {
    Chime chime;
    bool keep;  // a command: Settings shows it
    char heard[160];
    char result[160];
};

static void voice_done_apply(App* app, Event* e) {
    VoiceDone* d = event_record<VoiceDone>(e);
    chime_play(app, d->chime);
    if (!d->keep) return;
    Voice* v = &app->voice;
    SET(v->heard, str(d->heard));
    SET(v->result, str(d->result));
    v->heard_ts = now_ts();
    wake_window(app);
}

static void voice_say(App* app, Chime chime, const char* heard, const char* result) {
    VoiceDone d = {};
    d.chime = chime;
    d.keep = heard != 0;
    if (heard) SET(d.heard, str(heard));
    if (result) SET(d.result, str(result));
    event_send(app, voice_done_apply, d);
}

static volatile LONG g_voice_checking;  // a check of the AI is running

static DWORD WINAPI voice_check_ai_main(void* p) {
    App* app = (App*)p;
    char said[512] = {};
    bool ok = summary_check_ai(app, said, sizeof(said));
    // The first line is enough for Settings: "loaded qwen3-14b in 12s".
    char* nl = strchr(said, '\n');
    if (nl) *nl = 0;
    LOG("voice: check the AI: %s", said);
    voice_say(app, ok ? CHIME_DONE : CHIME_ERROR, "check the AI", said);
    atomic_set(&g_voice_checking, 0);
    return 0;
}

// The main thread: the summary may be writing, and then it has the AI already.
static void voice_check_ai_apply(App* app, Event*) {
    if (app->summary.writing) {
        voice_say(app, CHIME_ERROR, "check the AI", "the summary is being written: it is running");
        return;
    }
    if (InterlockedCompareExchange(&g_voice_checking, 1, 0) != 0) return;
    Voice* v = &app->voice;
    SET(v->heard, str("check the AI"));
    SET(v->result, str("checking..."));
    v->heard_ts = now_ts();
    wake_window(app);
    start_thread(app, "voice-check-ai", voice_check_ai_main);
}

// ---------------------------------------------------------------------------------------------
// The voice thread

// The API's answer as a line for Settings: its "error", or what it did.
static void answer_text(Arena* a, const VoiceCommand* c, Answer* r, char* out, i32 cap) {
    Str body = buf_take(&r->body, a);
    Json j = {};
    char error[64];
    json_parse(a, body, &j, error, sizeof(error));
    Str err = json_text(json_get(&j, "error"));
    if (r->code != 200) {
        snprintf(out, cap, "%.*s", (int)err.len, err.len ? err.ptr : "it did not work");
        return;
    }
    Str result = json_text(json_get(&j, "result"));
    Str path = json_text(json_get(&j, "path"));
    if (result.len) snprintf(out, cap, "%.*s", (int)result.len, result.ptr);
    else if (path.len) snprintf(out, cap, "%d lines on the clipboard", (i32)json_get(&j, "lines")->num);
    else snprintf(out, cap, "done");
    (void)c;
}

static void voice_do(App* app, const VoiceCommand* c, const char* heard) {
    Arena* a = scratch();
    Mark m = mark(a);
    bool paused = atomic_get(&app->capture.paused) != 0;
    if (c->what == DO_CANCEL) {
        voice_say(app, CHIME_CANCELLED, 0, 0);
    } else if (paused && !c->private_ok) {
        // Nothing about what was said is kept in private mode.
        voice_say(app, CHIME_ERROR, "(in private mode)", "only \"resume\" works in private mode");
    } else if (c->what == DO_CHECK_AI) {
        LOG("voice: check the AI");
        event_send(app, voice_check_ai_apply);
    } else if (c->what == DO_CONFIG) {
        const wchar_t* path = config_path();
        HINSTANCE got = ShellExecuteW(0, L"open", path, 0, 0, SW_SHOWNORMAL);
        if ((INT_PTR)got <= 32) got = ShellExecuteW(0, L"open", L"notepad.exe", path, 0, SW_SHOWNORMAL);
        bool ok = (INT_PTR)got > 32;
        LOG("voice: \"%s\": open config.toml%s", heard, ok ? "" : " (failed)");
        voice_say(app, ok ? CHIME_DONE : CHIME_ERROR, c->said, ok ? "opened" : "could not open config.toml");
    } else if (c->what == DO_API) {
        Answer r = {};
        api_ask(app, str(c->method), str(c->path), str(c->query), &r);
        if (r.code == 200 && r.after) event_send(app, api_after_apply, r.after);  // quit, restart
        char result[160];
        answer_text(a, c, &r, result, sizeof(result));
        buf_free(&r.body);
        bool ok = r.code == 200;
        LOG("voice: \"%s\": %s: %s", heard, c->said, result);
        voice_say(app, ok ? c->chime : CHIME_ERROR, c->said, result);
    }
    rewind(m);
}

// Loads ggml-base.en.bin for the processor. Null, with the state set, if it could not.
static whisper_context* voice_open(App* app, const char* ai_dir) {
    wchar_t bins[MAX_PATH], dll[MAX_PATH], bin[MAX_PATH];
    whisper_bin_dir(ai_dir, bins);
    swprintf(dll, MAX_PATH, L"%ls\\whisper.dll", bins);
    voice_model_path(ai_dir, bin);
    if (!exists(dll) || !exists(bin)) {
        atomic_set(&app->voice.state, VOICE_NEEDS_SETUP);
        return 0;
    }
    atomic_set(&app->voice.state, VOICE_LOADING);
    char error[256] = {};
    if (!whisper_load(bins, error, sizeof(error))) {
        WARN("voice commands: %s", error);
        atomic_set(&app->voice.state, VOICE_FAILED);
        return 0;
    }
    whisper_context_params cp = p_whisper_context_default_params();
    cp.use_gpu = false;  // the GPU is the transcriber's and the games'
    cp.flash_attn = false;
    cp.dtw_token_timestamps = false;
    Arena* a = scratch();
    Mark m = mark(a);
    f64 began = now_ts();
    whisper_context* ctx = p_whisper_init_from_file_with_params(narrow(a, bin).ptr, cp);
    rewind(m);
    if (!ctx) {
        WARN("voice commands: could not load ggml-base.en.bin");
        atomic_set(&app->voice.state, VOICE_FAILED);
        return 0;
    }
    LOG("voice commands: listening for \"lifelog\" (base.en loaded in %.1f s)", now_ts() - began);
    atomic_set(&app->voice.state, VOICE_READY);
    return ctx;
}

// What was said, or empty if it was not speech.
static Str voice_text(Arena* a, whisper_context* ctx, const VoiceSentence* s) {
    const i32 pad = VOICE_RATE * 3 / 10;
    i32 total = s->n + 2 * pad;
    f32* samples = PUSH_ARRAY(a, f32, total);  // zeroed: the silence around it
    memcpy(samples + pad, s->samples, sizeof(f32) * s->n);
    whisper_full_params wp = p_whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.n_threads = 4;
    wp.language = "en";
    wp.no_context = true;
    wp.n_max_text_ctx = 0;
    wp.single_segment = true;
    wp.no_timestamps = true;
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    wp.print_special = false;
    // So the name comes out as one word (with no prompt it is often "Lifeleg"); a prompt of just
    // the name makes it leave a first "Lifelog" out, as if already said.
    wp.initial_prompt = "Commands for Lifelog, the app.";
    if (p_whisper_full(ctx, wp, samples, total) != 0) return {"", 0};
    Buf b = {};
    i32 n = p_whisper_full_n_segments(ctx);
    for (i32 i = 0; i < n; i++) {
        if (p_whisper_full_get_segment_no_speech_prob(ctx, i) > 0.6f) continue;
        buf_cstr(&b, p_whisper_full_get_segment_text(ctx, i));
        buf_cstr(&b, " ");
    }
    return buf_take(&b, a);
}

static DWORD WINAPI voice_main(void* p) {
    App* app = (App*)p;
    Voice* v = &app->voice;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(0, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // for ShellExecute
    WorkerOptions o = {};
    LONG version = -1;
    char loaded_dir[MAX_PATH] = {};
    whisper_context* ctx = 0;
    f64 armed_until = 0;  // the name was said alone: the next sentence is the command
    f64 tried = 0;
    char failed_dir[MAX_PATH] = {};  // the model would not load from there: not tried again
    while (!stopping(app)) {
        DWORD wait = 10000;
        if (armed_until) {
            f64 left = armed_until - now_ts();
            wait = left > 0 ? (DWORD)(left * 1000) + 1 : 0;
        }
        HANDLE waits[2] = {app->stop, v->wake};
        if (WaitForMultipleObjects(2, waits, FALSE, wait) == WAIT_OBJECT_0) break;
        worker_options_take(app, &o, &version);
        bool listen = atomic_get(&v->listen) != 0;
        if (ctx && (!listen || strcmp(loaded_dir, o.ai_dir) != 0)) {
            p_whisper_free(ctx);
            ctx = 0;
        }
        if (!listen) {
            atomic_set(&v->state, VOICE_OFF);
            voice_drop_all(v);
            armed_until = 0;
            continue;
        }
        // Not there yet (set up from Settings, or the folder moved): look again every 10 s.
        if (!ctx && o.ai_dir[0] && now_ts() - tried >= 9 && strcmp(failed_dir, o.ai_dir) != 0) {
            tried = now_ts();
            ctx = voice_open(app, o.ai_dir);
            if (ctx) SET(loaded_dir, str(o.ai_dir));
            else if (atomic_get(&v->state) == VOICE_FAILED) SET(failed_dir, str(o.ai_dir));
        }
        if (!ctx) {
            voice_drop_all(v);
            continue;
        }
        if (armed_until && now_ts() >= armed_until) {
            armed_until = 0;
            voice_say(app, CHIME_CANCELLED, 0, 0);
        }
        VoiceSentence s;
        while (voice_take(v, &s)) {
            Arena* a = scratch();
            Mark m = mark(a);
            Str text = voice_text(a, ctx, &s);
            free(s.samples);
            bool armed = armed_until && now_ts() < armed_until;
            Understood u;
            voice_understand(text, armed, &u);
            if (!u.for_us) {  // not for us: dropped, never logged
                rewind(m);
                continue;
            }
            Str heard = str_trim(text);
            char said[160];
            snprintf(said, sizeof(said), "%.*s", (int)(heard.len < 159 ? heard.len : 159), heard.ptr);
            if (u.c.what != DO_NOTHING) {
                armed_until = 0;
                voice_do(app, &u.c, said);
            } else if (u.name_only) {
                armed_until = now_ts() + VOICE_WAIT_SECS;
                voice_say(app, CHIME_LISTENING, 0, 0);
            } else {
                armed_until = 0;
                bool paused = atomic_get(&app->capture.paused) != 0;
                if (!paused) LOG("voice: \"%s\": not a command", said);
                voice_say(app, CHIME_DIDNT_CATCH, paused ? 0 : said, paused ? 0 : "not a command");
            }
            rewind(m);
        }
    }
    if (ctx) p_whisper_free(ctx);
    voice_drop_all(v);
    CoUninitialize();
    return 0;
}

void voice_start(App* app) {
    Voice* v = &app->voice;
    v->wake = CreateEventW(0, FALSE, FALSE, 0);
    CHECK(v->wake, "the voice thread's event (error %lu)", GetLastError());
    atomic_set(&v->listen, app->config.voice.listen);
    atomic_set(&v->state, app->config.voice.listen ? VOICE_LOADING : VOICE_OFF);
    start_thread(app, "voice", voice_main);
    SetEvent(v->wake);
}
