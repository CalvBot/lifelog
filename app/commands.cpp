// commands.cpp: custom voice commands (commands.h, docs/custom-commands.md): reading the folders,
// matching a sentence, running a script, and the secrets.

Commands g_commands;

static const f64 CMD_TIMEOUT_SECS = 20;
static const i32 CMD_OUTPUT_MAX = 64 * 1024;

// A test copy (LIFELOG_CONFIG set) keeps its commands, secrets and data next to its config.
static const wchar_t* test_dir() {
    static wchar_t buf[MAX_PATH];
    static bool looked;
    if (!looked) {
        looked = true;
        if (GetEnvironmentVariableW(L"LIFELOG_CONFIG", buf, MAX_PATH)) {
            wchar_t* slash = wcsrchr(buf, L'\\');
            if (slash) *slash = 0;
            else wcscpy_s(buf, L".");
        }
    }
    return buf[0] ? buf : 0;
}

const wchar_t* commands_dir() {
    static wchar_t buf[MAX_PATH];
    if (!buf[0]) {
        swprintf(buf, MAX_PATH, L"%ls\\commands", test_dir() ? test_dir() : roaming_dir());
        CreateDirectoryW(buf, 0);
    }
    return buf;
}

// %LOCALAPPDATA%\lifelog\<sub>\<id><ext>, its folder made.
static void local_path(const char* sub, const char* id, const wchar_t* ext, wchar_t* out) {
    const wchar_t* base = test_dir() ? test_dir() : local_dir();
    swprintf(out, MAX_PATH, L"%ls\\%hs", base, sub);
    CreateDirectoryW(out, 0);
    swprintf(out, MAX_PATH, L"%ls\\%hs\\%hs%ls", base, sub, id, ext);
}

// ---------------------------------------------------------------------------------------------
// Reading the folders

static u64 fnv(u64 h, const void* p, i64 n) {
    const u8* b = (const u8*)p;
    for (i64 i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static int wcs_cmp(const void* a, const void* b) { return _wcsicmp((const wchar_t*)a, (const wchar_t*)b); }

// A hash of the files in `dir` (not its subfolders), names and contents: it changes when any does.
static u64 folder_hash(const wchar_t* dir) {
    Arena* a = scratch();
    Mark m = mark(a);
    struct FileName {
        wchar_t s[MAX_PATH];
    };
    List<FileName> names = {};
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            FileName n = {};
            wcscpy_s(n.s, fd.cFileName);
            list_push(&names, n);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (names.count) qsort(names.items, names.count, sizeof(FileName), wcs_cmp);
    u64 h = 14695981039346656037ull;
    for (i32 i = 0; i < names.count; i++) {
        h = fnv(h, names.items[i].s, (i64)(wcslen(names.items[i].s) * sizeof(wchar_t)));
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\%ls", dir, names.items[i].s);
        Str data = read_file(a, path);
        h = fnv(h, &data.len, sizeof(data.len));
        if (data.ptr) h = fnv(h, data.ptr, data.len);
    }
    list_free(&names);
    rewind(m);
    return h;
}

static const char* cmd_fail(CustomCommand* c, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(c->error, sizeof(c->error), fmt, args);
    va_end(args);
    return c->error;
}

// The phrase `say` is usable: words and {slots}, at least one word, no two slots side by side.
static bool phrase_ok(const char* say, char* why, i32 cap) {
    i32 words = 0;
    bool last_slot = false;
    for (const char* p = say; *p;) {
        if (*p == ' ') {
            p++;
            continue;
        }
        if (*p == '{') {
            const char* end = strchr(p, '}');
            if (!end || end == p + 1) {
                snprintf(why, cap, "\"%s\": a {slot} is not closed or has no name", say);
                return false;
            }
            if (last_slot) {
                snprintf(why, cap, "\"%s\": two slots need a word between them", say);
                return false;
            }
            last_slot = true;
            p = end + 1;
            continue;
        }
        while (*p && *p != ' ' && *p != '{') p++;
        words++;
        last_slot = false;
    }
    if (!words) snprintf(why, cap, "\"%s\": a phrase needs at least one word besides its slots", say);
    return words > 0;
}

static i32 cmd_action(CustomCommand* c, const char* name) {
    for (i32 i = 0; i < c->action_count; i++)
        if (strcmp(c->actions[i].name, name) == 0) return i;
    if (c->action_count == CMD_ACTIONS) return -1;
    CmdAction* a = &c->actions[c->action_count];
    SET(a->name, str(name));
    return c->action_count++;
}

// Reads <dir>\command.toml into `c` (its id and dir set). False, with c->error, if it is unusable.
static bool command_read(CustomCommand* c) {
    Arena* a = scratch();
    Mark m = mark(a);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\command.toml", c->dir);
    Str text = read_file(a, path);
    if (!text.ptr) {
        cmd_fail(c, "it has no command.toml");
        rewind(m);
        return false;
    }
    char error[200] = "";
    Toml t = {text, 0, 1, error, sizeof(error), "command.toml"};
    char table[64] = "";
    for (;;) {
        char key[64];
        Value v = {};
        bool got = false;
        bool ok = toml_next(&t, table, sizeof(table), key, sizeof(key), &v, &got);
        if (!ok || !got) {
            list_free(&v.array);
            if (!ok) cmd_fail(c, "%s", error);
            break;
        }
        Str k = str(key);
        if (!table[0]) {
            if (str_eq(k, str("name"))) take_string(c->name, sizeof(c->name), &v);
            if (str_eq(k, str("description"))) take_string(c->description, sizeof(c->description), &v);
            if (str_eq(k, str("run"))) take_string(c->run, sizeof(c->run), &v);
            if (str_eq(k, str("secrets")) && v.kind == Value::ARRAY)
                for (i32 i = 0; i < v.array.count && c->secret_count < CMD_SECRETS; i++)
                    SET(c->secrets[c->secret_count++], str(v.array.items[i].s));
        } else {
            i32 act = cmd_action(c, table);
            if (act < 0) {
                cmd_fail(c, "more than %d actions", CMD_ACTIONS);
            } else if (str_eq(k, str("description"))) {
                take_string(c->actions[act].description, sizeof(c->actions[act].description), &v);
            } else if (str_eq(k, str("say"))) {
                Value one = {};
                if (v.kind == Value::STRING) {
                    PathName item = {};
                    SET(item.s, str(v.s));
                    list_push(&one.array, item);
                }
                Folders* list = v.kind == Value::ARRAY ? &v.array : &one.array;
                for (i32 i = 0; i < list->count && !c->error[0]; i++) {
                    if (c->phrase_count == CMD_PHRASES) {
                        cmd_fail(c, "more than %d phrases", CMD_PHRASES);
                        break;
                    }
                    char why[200];
                    if (!phrase_ok(list->items[i].s, why, sizeof(why))) {
                        cmd_fail(c, "[%s] %s", table, why);
                        break;
                    }
                    CmdPhrase* p = &c->phrases[c->phrase_count++];
                    p->action = act;
                    SET(p->say, str(list->items[i].s));
                }
                list_free(&one.array);
            }
        }
        list_free(&v.array);
        if (c->error[0]) break;
    }
    if (!c->error[0] && !c->run[0]) cmd_fail(c, "command.toml has no run = \"...\"");
    if (!c->error[0] && !c->phrase_count) cmd_fail(c, "no action has a say = [...]");
    if (!c->name[0]) SET(c->name, str(c->id));
    rewind(m);
    return !c->error[0];
}

// The "folder:hash" kept in config.commands.allowed.
static void allowed_entry(const CustomCommand* c, char* out, i32 cap) {
    snprintf(out, cap, "%s:%016llx", c->id, (unsigned long long)c->hash);
    for (char* p = out; *p; p++) *p = (char)tolower((u8)*p);
}

static CustomCommand g_loading[CMD_MAX];  // the main thread's (or the CLI's), while loading

void commands_load(Config* cfg) {
    Arena* a = scratch();
    Mark m = mark(a);
    i32 n = 0;
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", commands_dir());
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            if (n == CMD_MAX) {
                WARN("commands: only the first %d folders are read", CMD_MAX);
                break;
            }
            CustomCommand* c = &g_loading[n++];
            *c = {};
            SET(c->id, narrow(a, fd.cFileName));
            swprintf(c->dir, MAX_PATH, L"%ls\\%ls", commands_dir(), fd.cFileName);
            if (strlen(c->id) > 40) {
                cmd_fail(c, "the folder's name is longer than 40 letters");
                continue;
            }
            command_read(c);
            c->hash = folder_hash(c->dir);
            char entry[64];
            allowed_entry(c, entry, sizeof(entry));
            c->allowed = names_has(&cfg->commands.allowed, str(entry));
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "%s:", c->id);
            for (i32 i = 0; i < cfg->commands.allowed.count && !c->allowed; i++)
                if (str_starts(str(cfg->commands.allowed.items[i].s), str_lower(a, str(prefix)))) c->changed = true;
            c->enabled = !names_has(&cfg->commands.disabled, str(c->id));
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    AcquireSRWLockExclusive(&g_commands.lock);
    memcpy(g_commands.items, g_loading, sizeof(CustomCommand) * n);
    g_commands.count = n;
    g_commands.version++;
    ReleaseSRWLockExclusive(&g_commands.lock);
    rewind(m);
}

// ---------------------------------------------------------------------------------------------
// Matching

// Words said that carry nothing ("um", "please"), one or two at a time.
static i32 filler_at(const Str* w, i32 n, i32 i) {
    static const char* one[] = {"um", "umm", "uh", "uhh", "uhm", "ah", "ahh", "oh", "er", "erm", "hmm", "hm", "please", "okay", "ok", "hey", "yo"};
    static const char* two[][2] = {{"can", "you"}, {"could", "you"}, {"would", "you"}, {"will", "you"}, {"i", "think"}, {"you", "know"}, {"i", "mean"}};
    if (word_is(w[i], one, COUNT(one))) return 1;
    for (i32 k = 0; k < COUNT(two) && i + 1 < n; k++)
        if (str_eq(w[i], str(two[k][0])) && str_eq(w[i + 1], str(two[k][1]))) return 2;
    return 0;
}

// Left at the end of a slot: "... for me", "... thanks".
static i32 trailer_at_end(const Str* w, i32 from, i32 to) {
    if (to - from >= 2 && ((str_eq(w[to - 2], str("for")) && str_eq(w[to - 1], str("me"))) ||
                           (str_eq(w[to - 2], str("thank")) && str_eq(w[to - 1], str("you"))) ||
                           (str_eq(w[to - 2], str("right")) && str_eq(w[to - 1], str("now")))))
        return 2;
    if (to - from >= 1 && word_in(w[to - 1], "thanks", "cheers")) return 1;
    return 0;
}

// `word` is the phrase's `lit`, or it with an ending: "playing" for "play".
static bool lit_match(Str lit, Str word) {
    if (str_eq(lit, word)) return true;
    return lit.len >= 3 && word.len > lit.len && word.len - lit.len <= 3 && str_starts(word, lit);
}

struct PhraseToken {
    Str s;      // the word, or the slot's name
    bool slot;
};

// A phrase as tokens, lower-cased like the words said, with the same fillers dropped.
static i32 phrase_tokens(Arena* a, const char* say, PhraseToken* out, i32 cap) {
    i32 n = 0;
    Str raw[32];
    bool slot[32];
    i32 rn = 0;
    for (const char* p = say; *p && rn < 32;) {
        if (*p == '{') {
            const char* end = strchr(p, '}');
            if (!end) break;
            raw[rn] = str_copy(a, {p + 1, end - p - 1});
            slot[rn++] = true;
            p = end + 1;
            continue;
        }
        if (!isalnum((u8)*p)) {
            p++;
            continue;
        }
        char* buf = PUSH_ARRAY(a, char, strlen(p) + 1);
        i64 len = 0;
        while (*p && *p != '{' && (isalnum((u8)*p) || *p == '\'')) {
            if (*p != '\'') buf[len++] = (char)tolower((u8)*p);
            p++;
        }
        raw[rn] = {buf, len};
        slot[rn++] = false;
    }
    for (i32 i = 0; i < rn && n < cap;) {
        if (!slot[i]) {
            i32 f = filler_at(raw, rn, i);
            if (f && (f == 1 || !slot[i + 1])) {
                i += f;
                continue;
            }
        }
        out[n].s = raw[i];
        out[n++].slot = slot[i++];
    }
    return n;
}

struct SlotValue {
    Str name;
    i32 from, to;  // words [from, to)
};

// Matches tokens T against words W with the first fixed word at `start`. The slots found, or -1.
static i32 match_at(const PhraseToken* T, i32 tn, const Str* W, i32 wn, i32 start, SlotValue* slots) {
    i32 sn = 0;
    i32 pos = 0;          // the next word not yet used
    i32 open = -1;        // a slot waiting for the next fixed word to end it
    for (i32 k = 0; k < tn; k++) {
        if (T[k].slot) {
            slots[sn].name = T[k].s;
            slots[sn].from = k == 0 ? 0 : pos;
            open = sn++;
            continue;
        }
        i32 at = -1;
        if (k == 0 || (k == 1 && T[0].slot && pos == 0)) {
            at = start;
            if (!lit_match(T[k].s, W[at])) return -1;
        } else {
            for (i32 i = pos; i < wn && at < 0; i++)
                if (lit_match(T[k].s, W[i])) at = i;
            if (at < 0) return -1;
        }
        if (open >= 0) {
            slots[open].to = at;
            if (slots[open].to <= slots[open].from) return -1;
            open = -1;
        }
        pos = at + 1;
    }
    if (open >= 0) {
        i32 to = wn;
        to -= trailer_at_end(W, slots[open].from, to);
        slots[open].from = slots[open].from < pos ? pos : slots[open].from;
        slots[open].to = to;
        if (to <= slots[open].from) return -1;
    }
    return sn;
}

bool commands_match(const Str* w, i32 n, bool leading, CmdMatch* out) {
    Arena* a = scratch();
    Mark m = mark(a);
    // The words with fillers dropped.
    Str* W = PUSH_ARRAY(a, Str, n + 1);
    i32 wn = 0;
    for (i32 i = 0; i < n;) {
        i32 f = filler_at(w, n, i);
        if (f) {
            i += f;
            continue;
        }
        W[wn++] = w[i++];
    }
    bool found = false;
    *out = {};
    AcquireSRWLockShared(&g_commands.lock);
    for (i32 ci = 0; ci < g_commands.count; ci++) {
        const CustomCommand* c = &g_commands.items[ci];
        if (!c->allowed || !c->enabled || c->error[0]) continue;
        for (i32 pi = 0; pi < c->phrase_count; pi++) {
            PhraseToken T[32];
            i32 tn = phrase_tokens(a, c->phrases[pi].say, T, COUNT(T));
            i32 fixed = 0, first = -1;
            for (i32 k = 0; k < tn; k++)
                if (!T[k].slot) {
                    fixed++;
                    if (first < 0) first = k;
                }
            if (!fixed || fixed < out->score || (found && fixed == out->score)) continue;
            // Where its first fixed word could be: a phrase that leads starts the sentence (a slot
            // before the word means it is the second); otherwise the last place it fits wins, as a
            // ramble tends to come before what is wanted.
            SlotValue slots[16], best[16];
            i32 got = -1;
            for (i32 start = 0; start < wn; start++) {
                if (leading && start > first + 1) break;
                if (first == 1 && start == 0) continue;
                i32 sn = match_at(T, tn, W, wn, start, slots);
                if (sn < 0) continue;
                got = sn;
                memcpy(best, slots, sizeof(SlotValue) * sn);
                if (leading) break;
            }
            if (got < 0) continue;
            found = true;
            SET(out->id, str(c->id));
            SET(out->action, str(c->actions[c->phrases[pi].action].name));
            out->score = fixed;
            Buf b = {};
            buf_cstr(&b, "{");
            i32 len = snprintf(out->label, sizeof(out->label), "%s %s", c->id, out->action);
            for (i32 s = 0; s < got; s++) {
                Buf v = {};
                for (i32 i = best[s].from; i < best[s].to; i++) {
                    if (i > best[s].from) buf_cstr(&v, " ");
                    buf_add(&v, W[i]);
                }
                Str value = buf_take(&v, a);
                buf_free(&v);
                if (s) buf_cstr(&b, ",");
                json_quote(&b, best[s].name);
                buf_cstr(&b, ":");
                json_quote(&b, value);
                if (len < (i32)sizeof(out->label))
                    len += snprintf(out->label + len, sizeof(out->label) - len, "%s %.*s = %.*s", s ? "," : ":", (int)best[s].name.len,
                                    best[s].name.ptr, (int)value.len, value.ptr);
            }
            buf_cstr(&b, "}");
            SET(out->slots, buf_take(&b, a));
            buf_free(&b);
        }
    }
    ReleaseSRWLockShared(&g_commands.lock);
    if (found) {
        i32 len = 0;
        for (i32 i = 0; i < n && len < (i32)sizeof(out->said); i++)
            len += snprintf(out->said + len, sizeof(out->said) - len, "%s%.*s", i ? " " : "", (int)w[i].len, w[i].ptr);
    }
    rewind(m);
    return found;
}

bool commands_match_text(Str text, CmdMatch* out) {
    Arena* a = scratch();
    Mark m = mark(a);
    Words ws;
    words_of(a, text, &ws);
    bool found = commands_match(ws.w, ws.n, false, out);
    rewind(m);
    return found;
}

// ---------------------------------------------------------------------------------------------
// Secrets: a JSON object per command, encrypted with DPAPI for this Windows account.

// The command's secrets as JSON text ("{}" when there are none), in `a`.
static Str secrets_read(Arena* a, const char* id) {
    wchar_t path[MAX_PATH];
    local_path("secrets", id, L".bin", path);
    Str data = read_file(a, path);
    if (!data.ptr || !data.len) return str("{}");
    DATA_BLOB in = {(DWORD)data.len, (BYTE*)data.ptr}, out = {};
    if (!CryptUnprotectData(&in, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        WARN("commands: %s's secrets could not be read (error %lu)", id, GetLastError());
        return str("{}");
    }
    Str text = str_copy(a, {(const char*)out.pbData, (i64)out.cbData});
    LocalFree(out.pbData);
    return text;
}

bool secret_set(const char* id, const char* key, Str value) {
    Arena* a = scratch();
    Mark m = mark(a);
    Json j = {};
    char error[64];
    json_parse(a, secrets_read(a, id), &j, error, sizeof(error));
    Buf b = {};
    buf_cstr(&b, "{");
    i32 n = 0;
    for (i32 i = 0; j.type == JSON_OBJECT && i < j.count; i++) {
        if (str_eq(j.keys[i], str(key)) || j.items[i].type != JSON_STRING) continue;
        if (n++) buf_cstr(&b, ",");
        json_quote(&b, j.keys[i]);
        buf_cstr(&b, ":");
        json_quote(&b, j.items[i].str);
    }
    if (value.len) {
        if (n++) buf_cstr(&b, ",");
        json_quote(&b, str(key));
        buf_cstr(&b, ":");
        json_quote(&b, value);
    }
    buf_cstr(&b, "}");
    Str text = buf_take(&b, a);
    buf_free(&b);
    DATA_BLOB in = {(DWORD)text.len, (BYTE*)text.ptr}, out = {};
    bool ok = CryptProtectData(&in, L"lifelog command secrets", 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out) != 0;
    if (ok) {
        wchar_t path[MAX_PATH];
        local_path("secrets", id, L".bin", path);
        ok = write_file(path, {(const char*)out.pbData, (i64)out.cbData});
        LocalFree(out.pbData);
    }
    if (!ok) WARN("commands: %s's secret %s could not be saved", id, key);
    rewind(m);
    return ok;
}

bool secret_is_set(const char* id, const char* key) {
    Arena* a = scratch();
    Mark m = mark(a);
    Json j = {};
    char error[64];
    json_parse(a, secrets_read(a, id), &j, error, sizeof(error));
    bool set = json_text(json_get(&j, key)).len > 0;
    rewind(m);
    return set;
}

// ---------------------------------------------------------------------------------------------
// Running

// Runs `c`'s script for `m` and waits for it (up to CMD_TIMEOUT_SECS). Not on the main thread.
static bool command_exec(const CustomCommand* c, const CmdMatch* m, char* message, i32 message_cap, char* banner, i32 banner_cap) {
    message[0] = banner[0] = 0;
    Arena* a = scratch();
    Mark mk = mark(a);
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");

    wchar_t data_dir[MAX_PATH];
    local_path("command-data", c->id, L"", data_dir);
    CreateDirectoryW(data_dir, 0);
    Buf in = {};
    buf_cstr(&in, "{\"action\":");
    json_quote(&in, str(m->action));
    buf_cstr(&in, ",\"slots\":");
    buf_cstr(&in, m->slots[0] ? m->slots : "{}");
    buf_cstr(&in, ",\"said\":");
    json_quote(&in, str(m->said));
    buf_cstr(&in, ",\"secrets\":");
    buf_add(&in, secrets_read(a, c->id));
    buf_cstr(&in, ",\"data_dir\":");
    json_quote(&in, narrow(a, data_dir));
    buf_cstr(&in, "}\n");
    Str input = buf_take(&in, a);
    buf_free(&in);

    SECURITY_ATTRIBUTES sa = {sizeof(sa), 0, TRUE};
    HANDLE in_r = 0, in_w = 0, out_r = 0, out_w = 0;
    CreatePipe(&in_r, &in_w, &sa, 64 * 1024);
    CreatePipe(&out_r, &out_w, &sa, 1 << 20);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi = {};
    // Its job ends everything it started when it is stopped (or lifelog ends). A script may start
    // something that outlives it on purpose, such as an AI agent that takes minutes, by starting it
    // with CREATE_BREAKAWAY_FROM_JOB.
    HANDLE job = CreateJobObjectW(0, 0);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
    wchar_t* line = wide(a, str(c->run));
    bool started = CreateProcessW(0, line, 0, 0, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, 0, c->dir, &si, &pi) != 0;
    DWORD start_error = GetLastError();
    CloseHandle(in_r);
    CloseHandle(out_w);
    bool ok = false;
    if (!started) {
        snprintf(message, message_cap, "could not start \"%s\" (error %lu)", c->run, start_error);
        CloseHandle(in_w);
    } else {
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        DWORD wrote = 0;
        WriteFile(in_w, input.ptr, (DWORD)input.len, &wrote, 0);
        CloseHandle(in_w);

        char* out = PUSH_ARRAY(a, char, CMD_OUTPUT_MAX + 1);
        i64 out_len = 0;
        f64 deadline = now_ts() + CMD_TIMEOUT_SECS;
        bool ended = false, timed_out = false;
        for (;;) {
            DWORD avail = 0;
            while (PeekNamedPipe(out_r, 0, 0, 0, &avail, 0) && avail) {
                char chunk[4096];
                DWORD got = 0;
                if (!ReadFile(out_r, chunk, sizeof(chunk), &got, 0) || !got) break;
                // Keep the end: the answer is the last line.
                if (out_len + got > CMD_OUTPUT_MAX) {
                    i64 drop = out_len + got - CMD_OUTPUT_MAX;
                    if (drop > out_len) drop = out_len;
                    memmove(out, out + drop, (size_t)(out_len - drop));
                    out_len -= drop;
                }
                memcpy(out + out_len, chunk, got < (DWORD)(CMD_OUTPUT_MAX - out_len) ? got : (DWORD)(CMD_OUTPUT_MAX - out_len));
                out_len += got < (DWORD)(CMD_OUTPUT_MAX - out_len) ? got : (DWORD)(CMD_OUTPUT_MAX - out_len);
            }
            if (ended) break;
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                ended = true;  // once more round, for what it wrote last
                continue;
            }
            if (now_ts() > deadline) {
                timed_out = true;
                TerminateJobObject(job, 1);
                break;
            }
        }
        out[out_len] = 0;
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        // The answer: the last line that is a JSON object, else the exit code and the last line.
        Str text = {out, out_len};
        Str last = {}, answer = {};
        i64 end = text.len;
        while (end > 0 && (!answer.len || !last.len)) {
            i64 start = end;
            while (start > 0 && text.ptr[start - 1] != '\n') start--;
            Str ln = str_trim(str_sub(text, start, end - start));
            if (ln.len && !last.len) last = ln;
            if (ln.len && ln.ptr[0] == '{' && !answer.len) answer = ln;
            end = start > 0 ? start - 1 : 0;
            if (start == 0) break;
        }
        if (timed_out) {
            snprintf(message, message_cap, "it took longer than %.0f seconds and was stopped", CMD_TIMEOUT_SECS);
        } else if (answer.len) {
            Json j = {};
            char error[64];
            if (json_parse(a, answer, &j, error, sizeof(error))) {
                const Json* okj = json_get(&j, "ok");
                ok = okj->type == JSON_BOOL ? okj->b : code == 0;
                Str msg = json_text(json_get(&j, "message")), ban = json_text(json_get(&j, "banner"));
                snprintf(message, message_cap, "%.*s", (int)msg.len, msg.ptr);
                snprintf(banner, banner_cap, "%.*s", (int)ban.len, ban.ptr);
            } else {
                snprintf(message, message_cap, "its answer is not JSON: %s", error);
            }
        } else {
            ok = code == 0;
            snprintf(message, message_cap, "%.*s", (int)last.len, last.ptr);
        }
        if (!ok && !message[0]) snprintf(message, message_cap, "it ended with code %lu", code);
        if (!ok) WARN("commands: %s %s: %s; its output: %.400s", c->id, m->action, message, out);
    }
    CloseHandle(out_r);
    CloseHandle(job);
    rewind(mk);
    return ok;
}

// Copies out the command `id` if it may run; else why not.
static bool command_get(const char* id, CustomCommand* out, char* why, i32 cap) {
    bool found = false;
    AcquireSRWLockShared(&g_commands.lock);
    for (i32 i = 0; i < g_commands.count && !found; i++)
        if (strcmp(g_commands.items[i].id, id) == 0) {
            *out = g_commands.items[i];
            found = true;
        }
    ReleaseSRWLockShared(&g_commands.lock);
    if (!found) snprintf(why, cap, "there is no command %s", id);
    else if (out->error[0]) snprintf(why, cap, "%s", out->error);
    else if (!out->allowed) snprintf(why, cap, "%s is not allowed to run yet (Settings)", out->name);
    else if (!out->enabled) snprintf(why, cap, "%s is switched off", out->name);
    else return true;
    return false;
}

bool commands_run_here(const CmdMatch* m, char* message, i32 message_cap, char* banner, i32 banner_cap) {
    CustomCommand* c = (CustomCommand*)calloc(1, sizeof(CustomCommand));
    CHECK(c, "memory for a command");
    banner[0] = 0;
    bool ok = command_get(m->id, c, message, message_cap) && command_exec(c, m, message, message_cap, banner, banner_cap);
    free(c);
    return ok;
}

struct CmdRun {
    App* app;
    CmdMatch m;
    CmdDone done;
};
static volatile LONG g_cmd_running;

static DWORD WINAPI command_run_main(void* p) {
    CmdRun* r = (CmdRun*)p;
    char message[256], banner[160];
    bool ok = commands_run_here(&r->m, message, sizeof(message), banner, sizeof(banner));
    LOG("commands: %s: %s%s%s", r->m.label, ok ? "done" : "failed", message[0] ? ": " : "", message);
    r->done(r->app, &r->m, ok, message, banner);
    free(r);
    atomic_set(&g_cmd_running, 0);
    return 0;
}

void commands_run(App* app, const CmdMatch* m, CmdDone done) {
    if (InterlockedCompareExchange(&g_cmd_running, 1, 0) != 0) {
        done(app, m, false, "another command is still running", "");
        return;
    }
    CmdRun* r = (CmdRun*)calloc(1, sizeof(CmdRun));
    CHECK(r, "memory for a command run");
    r->app = app;
    r->m = *m;
    r->done = done;
    HANDLE t = CreateThread(0, 0, command_run_main, r, 0, 0);
    if (!t) {
        free(r);
        atomic_set(&g_cmd_running, 0);
        done(app, m, false, "its thread would not start", "");
        return;
    }
    SetThreadDescription(t, L"command");
    CloseHandle(t);
}

// ---------------------------------------------------------------------------------------------
// Settings

void commands_allow(App* app, const char* id) {
    Names* allowed = &app->config.commands.allowed;
    char entry[64] = "";
    AcquireSRWLockShared(&g_commands.lock);
    for (i32 i = 0; i < g_commands.count; i++)
        if (strcmp(g_commands.items[i].id, id) == 0) allowed_entry(&g_commands.items[i], entry, sizeof(entry));
    ReleaseSRWLockShared(&g_commands.lock);
    if (!entry[0]) return;
    // The entry for its old files goes.
    Arena* a = scratch();
    Mark m = mark(a);
    Str prefix = str_lower(a, str_fmt(a, "%s:", id));
    for (i32 i = 0; i < allowed->count; i++)
        if (str_starts(str(allowed->items[i].s), prefix)) list_remove(allowed, i--);
    names_set(allowed, str(entry), true);
    config_save_names("commands", "allowed", allowed);
    rewind(m);
    LOG("commands: %s allowed", id);
    commands_load(&app->config);
}

void commands_enable(App* app, const char* id, bool on) {
    names_set(&app->config.commands.disabled, str(id), !on);
    config_save_names("commands", "disabled", &app->config.commands.disabled);
    LOG("commands: %s switched %s", id, on ? "on" : "off");
    commands_load(&app->config);
}
