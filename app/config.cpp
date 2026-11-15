// config.cpp: reads the part of TOML that config.toml uses (tables, strings, numbers, booleans and
// arrays of strings) and changes single keys in place.

static const char DEFAULT_CONFIG[] = R"toml(# lifelog settings. Restart lifelog after editing.

# Where the database and the transcription queue live.
data_dir = 'G:\lifelog'

# Local API port (127.0.0.1 only).
api_port = 47800

[capture]
mic = true
apps = true
# Every app that makes a sound is recorded, except the excluded ones (exe names, case-insensitive).
# New apps wait on the Settings page for a yes (included) or no (excluded); they are recorded meanwhile.
# Games can be excluded too: they still count as games (for auto mode) but aren't recorded.
included_apps = []
excluded_apps = []
# Loudness (0..1 RMS) above which audio counts as "something is playing".
gate_rms = 0.003
# Days the audio of transcribed lines is kept, to play them back. The text is kept for good.
keep_audio_days = 7

[processing]
# When to transcribe. Capturing never stops; this only controls the GPU work.
#   "auto"   = wait while a game is using the GPU, catch up afterwards
#   "always" = transcribe no matter what
#   "paused" = only capture
mode = "auto"
# 3D load (percent, as in Task Manager) that counts as "a game is running".
gpu_busy_percent = 40
# How long the GPU must stay quiet before transcription starts again.
resume_after_secs = 60
# Apps treated as games (exe names): in "auto" mode nothing is transcribed while one is running,
# whatever the GPU load.
games = []
# Open apps dismissed as "not a game": no longer offered in the game lists.
not_games = []

[summary]
# Each evening a notification asks whether the local AI may write the day's summary (it needs the
# graphics card for a few minutes): write it now, in an hour, or skip today.
enabled = true
ask_at = "21:00"
max_words = 250
# The model file (<model>.gguf in %LOCALAPPDATA%\lifelog\llm). Its server runs only while a summary is written.
model = "qwen3-14b"
# Or use an AI you already run: any OpenAI-compatible endpoint, e.g. "http://127.0.0.1:11434/v1" for
# Ollama. Then `model` is the name that endpoint knows it by. A server on another machine gets the
# day's transcript.
endpoint = ""
api_key = ""

[worker]
# Python that has the worker installed; lifelog starts it and keeps it running.
# Leave empty to use the worker folder next to lifelog.exe, or in the repo it was built from.
python = ""
dir = ""
model = "large-v3-turbo"
device = "cuda"
compute_type = "float16"
language = "en"
)toml";

const char* mode_name(Mode m) {
    switch (m) {
        case MODE_ALWAYS: return "always";
        case MODE_PAUSED: return "paused";
        default: return "auto";
    }
}

bool mode_parse(Str s, Mode* out) {
    if (str_eq(s, str("auto"))) *out = MODE_AUTO;
    else if (str_eq(s, str("always"))) *out = MODE_ALWAYS;
    else if (str_eq(s, str("paused"))) *out = MODE_PAUSED;
    else return false;
    return true;
}

const wchar_t* config_path() {
    static wchar_t buf[MAX_PATH];
    if (!buf[0]) {
        DWORD n = GetEnvironmentVariableW(L"LIFELOG_CONFIG", buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) swprintf(buf, MAX_PATH, L"%s\\config.toml", roaming_dir());
    }
    return buf;
}

// ---------------------------------------------------------------------------------------------
// Reading

struct Toml {
    Str text;
    i64 at;
    i32 line;
    char* error;
    i32 error_cap;
};

static bool toml_fail(Toml* t, const char* what) {
    snprintf(t->error, t->error_cap, "config.toml line %d: %s", t->line, what);
    return false;
}

static char peek(Toml* t) { return t->at < t->text.len ? t->text.ptr[t->at] : 0; }

static void skip_space(Toml* t) {
    while (peek(t) == ' ' || peek(t) == '\t') t->at++;
}

// Spaces, comments and line ends (inside arrays).
static void skip_blank(Toml* t) {
    for (;;) {
        char c = peek(t);
        if (c == ' ' || c == '\t' || c == '\r') t->at++;
        else if (c == '\n') t->at++, t->line++;
        else if (c == '#') {
            while (peek(t) && peek(t) != '\n') t->at++;
        } else return;
    }
}

static bool skip_rest_of_line(Toml* t) {
    skip_space(t);
    if (peek(t) == '#')
        while (peek(t) && peek(t) != '\n') t->at++;
    if (peek(t) == '\r') t->at++;
    if (peek(t) && peek(t) != '\n') return toml_fail(t, "unexpected text after the value");
    if (peek(t) == '\n') t->at++, t->line++;
    return true;
}

// One string, '...' (as is) or "..." (with \ escapes), into `out`.
static bool read_string(Toml* t, char* out, i64 cap) {
    char quote = peek(t);
    t->at++;
    i64 n = 0;
    for (;;) {
        char c = peek(t);
        if (!c || c == '\n') return toml_fail(t, "a string is not closed");
        t->at++;
        if (c == quote) break;
        if (c == '\\' && quote == '"') {
            char e = peek(t);
            t->at++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                default: return toml_fail(t, "unknown escape in a string");
            }
        }
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = 0;
    return true;
}

// A value: a string, a number, true/false, or an array of strings.
struct Value {
    enum { NONE, STRING, NUMBER, BOOL, ARRAY } kind;
    char s[MAX_PATH * 2];
    f64 number;
    bool on;
    Names array;  // freed by the caller when not kept
};

static bool read_value(Toml* t, Value* v) {
    char c = peek(t);
    if (c == '"' || c == '\'') {
        v->kind = Value::STRING;
        return read_string(t, v->s, sizeof(v->s));
    }
    if (c == '[') {
        v->kind = Value::ARRAY;
        t->at++;
        for (;;) {
            skip_blank(t);
            if (peek(t) == ']') {
                t->at++;
                return true;
            }
            if (peek(t) != '"' && peek(t) != '\'') return toml_fail(t, "lists may only hold strings");
            Name item = {};
            if (!read_string(t, item.s, sizeof(item.s))) return false;
            list_push(&v->array, item);
            skip_blank(t);
            if (peek(t) == ',') t->at++;
            else if (peek(t) != ']') return toml_fail(t, "expected , or ] in a list");
        }
    }
    i64 start = t->at;
    while (peek(t) && peek(t) != ' ' && peek(t) != '\t' && peek(t) != '\r' && peek(t) != '\n' && peek(t) != '#')
        t->at++;
    Str word = str_sub(t->text, start, t->at - start);
    if (str_eq(word, str("true")) || str_eq(word, str("false"))) {
        v->kind = Value::BOOL;
        v->on = word.ptr[0] == 't';
        return true;
    }
    char buf[64];
    SET(buf, word);
    char* end = nullptr;
    v->number = strtod(buf, &end);
    if (!word.len || *end) return toml_fail(t, "expected a value");
    v->kind = Value::NUMBER;
    return true;
}

static void take_string(char* dst, i64 cap, Value* v) {
    if (v->kind == Value::STRING) set(dst, cap, str(v->s));
}
static void take_bool(bool* dst, Value* v) {
    if (v->kind == Value::BOOL) *dst = v->on;
}
static void take_u32(u32* dst, Value* v) {
    if (v->kind == Value::NUMBER && v->number >= 0) *dst = (u32)v->number;
}
static void take_names(Names* dst, Value* v) {
    if (v->kind != Value::ARRAY) return;
    list_free(dst);
    for (i32 i = 0; i < v->array.count; i++) names_set(dst, str(v->array.items[i].s), true);
}

static void config_defaults(Config* cfg) {
    *cfg = {};
    SET(cfg->data_dir, str("G:\\lifelog"));
    cfg->api_port = 47800;
    cfg->capture.mic = true;
    cfg->capture.apps = true;
    cfg->capture.gate_rms = 0.003f;
    cfg->capture.keep_audio_days = 7;
    cfg->processing.mode = MODE_AUTO;
    cfg->processing.gpu_busy_percent = 40;
    cfg->processing.resume_after_secs = 60;
    cfg->summary.enabled = true;
    SET(cfg->summary.ask_at, str("21:00"));
    cfg->summary.max_words = 250;
    SET(cfg->summary.model, str("qwen3-14b"));
    cfg->summary.ask_reminders = true;
}

bool config_load(Config* cfg, char* error, i32 error_cap) {
    Arena* a = scratch();
    Mark m = mark(a);
    if (GetFileAttributesW(config_path()) == INVALID_FILE_ATTRIBUTES) {
        write_file(config_path(), str(DEFAULT_CONFIG));
        LOG("wrote the default config");
    }
    Str text = read_file(a, config_path());
    if (!text.ptr) {
        snprintf(error, error_cap, "could not read config.toml");
        rewind(m);
        return false;
    }
    config_defaults(cfg);
    Toml t = {text, 0, 1, error, error_cap};
    char table[64] = "";
    bool ok = true;
    while (ok) {
        skip_blank(&t);
        if (!peek(&t)) break;
        if (peek(&t) == '[') {
            t.at++;
            i64 start = t.at;
            while (peek(&t) && peek(&t) != ']' && peek(&t) != '\n') t.at++;
            if (peek(&t) != ']') {
                ok = toml_fail(&t, "a [table] is not closed");
                break;
            }
            SET(table, str_trim(str_sub(text, start, t.at - start)));
            t.at++;
            ok = skip_rest_of_line(&t);
            continue;
        }
        i64 start = t.at;
        while (peek(&t) && peek(&t) != '=' && peek(&t) != '\n') t.at++;
        if (peek(&t) != '=') {
            ok = toml_fail(&t, "expected key = value");
            break;
        }
        char key[64];
        SET(key, str_trim(str_sub(text, start, t.at - start)));
        t.at++;
        skip_space(&t);
        Value v = {};
        ok = read_value(&t, &v) && skip_rest_of_line(&t);
        if (!ok) {
            list_free(&v.array);
            break;
        }
        Str tb = str(table), k = str(key);
        if (!table[0]) {
            if (str_eq(k, str("data_dir"))) take_string(cfg->data_dir, sizeof(cfg->data_dir), &v);
            if (str_eq(k, str("api_port")) && v.kind == Value::NUMBER) cfg->api_port = (u16)v.number;
        } else if (str_eq(tb, str("capture"))) {
            if (str_eq(k, str("mic"))) take_bool(&cfg->capture.mic, &v);
            if (str_eq(k, str("apps"))) take_bool(&cfg->capture.apps, &v);
            if (str_eq(k, str("included_apps"))) take_names(&cfg->capture.included_apps, &v);
            // Old name of included_apps, merged into it.
            if (str_eq(k, str("locked_apps")) && v.kind == Value::ARRAY)
                for (i32 i = 0; i < v.array.count; i++) names_set(&cfg->capture.included_apps, str(v.array.items[i].s), true);
            if (str_eq(k, str("excluded_apps"))) take_names(&cfg->capture.excluded_apps, &v);
            if (str_eq(k, str("gate_rms")) && v.kind == Value::NUMBER) cfg->capture.gate_rms = (f32)v.number;
            if (str_eq(k, str("keep_audio_days"))) take_u32(&cfg->capture.keep_audio_days, &v);
        } else if (str_eq(tb, str("processing"))) {
            if (str_eq(k, str("mode")) && v.kind == Value::STRING) mode_parse(str(v.s), &cfg->processing.mode);
            if (str_eq(k, str("gpu_busy_percent"))) take_u32(&cfg->processing.gpu_busy_percent, &v);
            if (str_eq(k, str("resume_after_secs"))) take_u32(&cfg->processing.resume_after_secs, &v);
            if (str_eq(k, str("games"))) take_names(&cfg->processing.games, &v);
            if (str_eq(k, str("not_games"))) take_names(&cfg->processing.not_games, &v);
        } else if (str_eq(tb, str("summary"))) {
            if (str_eq(k, str("enabled"))) take_bool(&cfg->summary.enabled, &v);
            if (str_eq(k, str("ask_at"))) take_string(cfg->summary.ask_at, sizeof(cfg->summary.ask_at), &v);
            if (str_eq(k, str("max_words"))) take_u32(&cfg->summary.max_words, &v);
            if (str_eq(k, str("model"))) take_string(cfg->summary.model, sizeof(cfg->summary.model), &v);
            if (str_eq(k, str("endpoint"))) take_string(cfg->summary.endpoint, sizeof(cfg->summary.endpoint), &v);
            if (str_eq(k, str("api_key"))) take_string(cfg->summary.api_key, sizeof(cfg->summary.api_key), &v);
            if (str_eq(k, str("ask_reminders"))) take_bool(&cfg->summary.ask_reminders, &v);
        } else if (str_eq(tb, str("worker"))) {
            if (str_eq(k, str("python"))) take_string(cfg->worker.python, sizeof(cfg->worker.python), &v);
            if (str_eq(k, str("dir"))) take_string(cfg->worker.dir, sizeof(cfg->worker.dir), &v);
        } else if (str_eq(tb, str("ui"))) {
            if (str_eq(k, str("hidden"))) take_names(&cfg->ui.hidden, &v);
            if (str_eq(k, str("show_dismissed"))) take_bool(&cfg->ui.show_dismissed, &v);
        }
        list_free(&v.array);
    }
    rewind(m);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Writing one key

static SRWLOCK save_lock = SRWLOCK_INIT;

// Where the line holding `pos` ends (past its newline).
static i64 line_end(Str s, i64 pos) {
    while (pos < s.len && s.ptr[pos] != '\n') pos++;
    return pos < s.len ? pos + 1 : pos;
}

bool config_save_value(const char* table, const char* key, Str value) {
    AcquireSRWLockExclusive(&save_lock);
    Arena* a = scratch();
    Mark m = mark(a);
    Str text = read_file(a, config_path());
    bool ok = false;
    if (text.ptr) {
        // The table's lines: from after its header (or the top of the file) to the next header.
        i64 from = table[0] ? -1 : 0, to = text.len;
        for (i64 at = 0; at < text.len; at = line_end(text, at)) {
            Str line = str_trim(str_sub(text, at, line_end(text, at) - at));
            if (!line.len || line.ptr[0] != '[') continue;
            if (from >= 0) {
                to = at;
                break;
            }
            i64 close = str_find(line, str("]"));
            if (close > 0 && str_eq(str_trim(str_sub(line, 1, close - 1)), str(table))) from = line_end(text, at);
        }
        bool found_table = from >= 0;
        Str out = {};
        if (!found_table) {
            out = str_fmt(a, "%.*s%s\n[%s]\n%s = %.*s\n", (int)text.len, text.ptr,
                          text.len && text.ptr[text.len - 1] != '\n' ? "\n" : "", table, key, (int)value.len, value.ptr);
        } else {
            // The key's line, if the table has it.
            i64 key_at = -1, key_end = -1, last_key_end = from;
            for (i64 at = from; at < to; at = line_end(text, at)) {
                Str line = str_trim(str_sub(text, at, line_end(text, at) - at));
                if (!line.len || line.ptr[0] == '#') continue;
                i64 eq = str_find(line, str("="));
                if (eq < 0) continue;
                last_key_end = line_end(text, at);
                if (str_eq(str_trim(str_sub(line, 0, eq)), str(key))) {
                    key_at = at;
                    key_end = line_end(text, at);
                    // A list may run over several lines, to its ']'.
                    Str rest = str_trim(str_sub(line, eq + 1));
                    if (rest.len && rest.ptr[0] == '[') {
                        i64 close = key_at + (rest.ptr - text.ptr - key_at);
                        while (close < text.len && text.ptr[close] != ']') close++;
                        key_end = line_end(text, close);
                    }
                    break;
                }
            }
            Str line = str_fmt(a, "%s = %.*s\n", key, (int)value.len, value.ptr);
            i64 cut_from = key_at >= 0 ? key_at : last_key_end;
            i64 cut_to = key_at >= 0 ? key_end : last_key_end;
            out = str_fmt(a, "%.*s%.*s%.*s", (int)cut_from, text.ptr, (int)line.len, line.ptr, (int)(text.len - cut_to),
                          text.ptr + cut_to);
        }
        ok = write_file(config_path(), out);
    }
    if (!ok) ERR("could not save %s.%s in config.toml", table, key);
    rewind(m);
    ReleaseSRWLockExclusive(&save_lock);
    return ok;
}

bool config_save_names(const char* table, const char* key, Names* names) {
    Arena* a = scratch();
    Mark m = mark(a);
    Str list = str("[");
    for (i32 i = 0; i < names->count; i++)
        list = str_fmt(a, "%.*s%s\"%s\"", (int)list.len, list.ptr, i ? ", " : "", names->items[i].s);
    list = str_fmt(a, "%.*s]", (int)list.len, list.ptr);
    bool ok = config_save_value(table, key, list);
    rewind(m);
    return ok;
}

bool config_save_bool(const char* table, const char* key, bool on) {
    return config_save_value(table, key, str(on ? "true" : "false"));
}

bool config_save_u32(const char* table, const char* key, u32 n) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", n);
    return config_save_value(table, key, str(buf));
}

bool config_save_string(const char* table, const char* key, const char* s) {
    Arena* a = scratch();
    Mark m = mark(a);
    Str out = str("\"");
    for (const char* c = s; *c; c++) {
        if (*c == '"' || *c == '\\') out = str_fmt(a, "%.*s\\%c", (int)out.len, out.ptr, *c);
        else out = str_fmt(a, "%.*s%c", (int)out.len, out.ptr, *c);
    }
    out = str_fmt(a, "%.*s\"", (int)out.len, out.ptr);
    bool ok = config_save_value(table, key, out);
    rewind(m);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Paths

static void under_data(Config* cfg, const wchar_t* name, wchar_t out[MAX_PATH]) {
    Arena* a = scratch();
    Mark m = mark(a);
    swprintf(out, MAX_PATH, L"%s\\%s", wide(a, str(cfg->data_dir)), name);
    rewind(m);
}

void spool_dir(Config* cfg, wchar_t out[MAX_PATH]) { under_data(cfg, L"spool", out); }
void audio_dir(Config* cfg, wchar_t out[MAX_PATH]) { under_data(cfg, L"audio", out); }
void files_dir(Config* cfg, wchar_t out[MAX_PATH]) { under_data(cfg, L"files", out); }
void db_path(Config* cfg, wchar_t out[MAX_PATH]) { under_data(cfg, L"lifelog.db", out); }
