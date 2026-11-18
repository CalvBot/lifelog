// mcp.cpp: `lifelog mcp`, an MCP server on stdin and stdout, so an AI agent (Claude Code, Codex,
// or anything else that speaks MCP) can read what was heard: the transcript of a stretch, a
// search over it, a day's summary, and the running lifelog's status. It only reads. It works
// straight from the database, so it does not need lifelog to be running (but status does).
//
// The agent starts it as a program (no network port, so nothing else can reach it):
//   claude mcp add lifelog -- "C:\Program Files\lifelog\lifelog.exe" mcp
// A custom voice command can then hand what was said to the agent ("Jarvis, ask Claude to play
// that song"), and the agent looks back at the conversation through these tools.
//
// The messages are JSON-RPC 2.0, one per line (MCP's stdio transport).

static const char* MCP_VERSION = "2025-06-18";
static const i64 MCP_MOST = 120000;  // bytes of text in one answer; older lines are cut first

static const char* MCP_TOOLS =
    "[{\"name\":\"transcript\",\"title\":\"Transcript\","
    "\"description\":\"What was heard (the mic as 'me', and each app or person in a call), one line each: "
    "[time] speaker: text. Give one of: range (the last stretch up to now: 15m, 1h, 90s, 2d or today; 15m when "
    "nothing is given), day (YYYY-MM-DD, a whole day from 04:00 to 04:00), or from and to (local time, "
    "YYYY-MM-DD HH:MM). Long stretches keep their latest lines.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"range\":{\"type\":\"string\",\"description\":\"15m, 1h, 90s, 2d or today\"},"
    "\"day\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD\"},"
    "\"from\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD HH:MM, local time\"},"
    "\"to\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD HH:MM, local time; now when left out\"}}},"
    "\"annotations\":{\"readOnlyHint\":true}},"
    "{\"name\":\"search\",\"title\":\"Search the transcript\","
    "\"description\":\"Lines that have all of the words, in any order (the last word may be the start of one), "
    "newest first, with their date and time. Give a few plain words, such as: song spotify. Use it to find when "
    "something was talked about, then transcript around that time. from and to narrow it to a stretch.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"words to look for\"},"
    "\"from\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD HH:MM, local time\"},"
    "\"to\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD HH:MM, local time\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"most lines to return, 30 when left out, up to 200\"}},"
    "\"required\":[\"query\"]},"
    "\"annotations\":{\"readOnlyHint\":true}},"
    "{\"name\":\"summary\",\"title\":\"Daily summary\","
    "\"description\":\"The summary lifelog wrote of a day (markdown), if it wrote one.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"day\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD; yesterday when left out\"}}},"
    "\"annotations\":{\"readOnlyHint\":true}},"
    "{\"name\":\"status\",\"title\":\"Status\","
    "\"description\":\"What the running lifelog is doing: recording or in private mode, the apps it hears, the "
    "transcriber and the summary, as JSON.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}},"
    "\"annotations\":{\"readOnlyHint\":true}}]";

static void mcp_send(Str line) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD wrote = 0;
    WriteFile(out, line.ptr, (DWORD)line.len, &wrote, 0);
    WriteFile(out, "\n", 1, &wrote, 0);
}

static void mcp_reply(const Json* id, Buf* result) {
    Buf b = {};
    buf_cstr(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_write(&b, id);
    buf_cstr(&b, ",\"result\":");
    buf_add(&b, {result->chars.items, result->chars.count});
    buf_cstr(&b, "}");
    mcp_send({b.chars.items, b.chars.count});
    buf_free(&b);
}

static void mcp_fail(const Json* id, i32 code, const char* message) {
    Buf b = {};
    buf_cstr(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id) json_write(&b, id);
    else buf_cstr(&b, "null");
    buf_fmt(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    json_quote(&b, str(message));
    buf_cstr(&b, "}}");
    mcp_send({b.chars.items, b.chars.count});
    buf_free(&b);
}

// A tool's answer: its text, and whether it is an error the agent should see as one.
static void mcp_text(Buf* result, Str text, bool error) {
    buf_cstr(result, "{\"content\":[{\"type\":\"text\",\"text\":");
    json_quote(result, text);
    buf_fmt(result, "}],\"isError\":%s}", error ? "true" : "false");
}

// "YYYY-MM-DD HH:MM" (or with a T, as ISO writes it), local time. False if it is not one.
static bool mcp_time(Str s, f64* out) {
    char c[40];
    SET(c, str_trim(s));
    i32 y, mo, d, h, mi;
    char sep;
    if (sscanf_s(c, "%d-%d-%d%c%d:%d", &y, &mo, &d, &sep, 1, &h, &mi) != 6 || (sep != ' ' && sep != 'T')) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    *out = local_ts(y, mo, d, h, mi);
    return true;
}

// What the user lets agents see: lines from `earliest` on (0: all of them), and not the speakers
// hidden in the timeline. A reach that does not parse shares nothing rather than everything.
struct McpReach {
    f64 earliest;
    Names* hidden;
};

static McpReach mcp_reach() {
    McpReach r = {0, &g_cli.config.ui.hidden};
    const char* reach = g_cli.config.mcp.reach;
    if (reach[0] && !clip_range(str(reach), now_ts(), &r.earliest)) r.earliest = now_ts();
    return r;
}

// The note an answer carries when the reach cut some of it off.
static Str mcp_reach_note(Arena* a) {
    return str_fmt(a, "(lifelog shares only the last %s with agents; older lines are not shown)", g_cli.config.mcp.reach);
}

// Names for the people in calls. The Discord plugin gives each one a stream of their own, named
// "discord-<user>-<id>.exe"; the window names them from its app list, which this program does not
// have, so they are named "<user>" from that here.
static AppLabels mcp_labels(Arena* a) {
    AppLabels out = {};
    sqlite3* db = db_open_path(true);
    if (!db) return out;
    sqlite3_stmt* st = prepare(db, "SELECT DISTINCT speaker FROM segments WHERE speaker GLOB '*?-?*-[0-9]*.exe' LIMIT 500");
    if (st) {
        out.items = PUSH_ARRAY(a, AppLabel, 500);
        while (sqlite3_step(st) == SQLITE_ROW) {
            Str exe = col_str(st, 0, a);
            Str s = strip_exe(exe);
            i64 first = 0, last = s.len - 1;
            while (first < s.len && s.ptr[first] != '-') first++;
            while (last > first && s.ptr[last] != '-') last--;
            if (last <= first + 1 || exe.len >= (i64)sizeof(out.items[0].exe)) continue;
            AppLabel* l = &out.items[out.count++];
            SET(l->exe, exe);
            Str name = {s.ptr + first + 1, last - first - 1};
            SET(l->name, name);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

static void mcp_transcript(const Json* args, Arena* a, Buf* result) {
    Str range = json_text(json_get(args, "range")), day = json_text(json_get(args, "day"));
    Str from_s = json_text(json_get(args, "from")), to_s = json_text(json_get(args, "to"));
    f64 now = now_ts(), from = 0, to = now;
    if (day.len) {
        char d[16];
        SET(d, str_trim(day));
        i32 y, m, dd;
        if (!parse_day(d, &y, &m, &dd)) return mcp_text(result, str("day must look like 2026-11-18"), true);
        day_bounds(d, &from, &to);
        if (to > now) to = now;
    } else if (from_s.len) {
        if (!mcp_time(from_s, &from)) return mcp_text(result, str("from must look like 2026-11-18 13:54"), true);
        if (to_s.len && !mcp_time(to_s, &to)) return mcp_text(result, str("to must look like 2026-11-18 14:10"), true);
        if (to <= from) return mcp_text(result, str("to must be after from"), true);
    } else if (!clip_range(range.len ? range : str("15m"), now, &from)) {
        return mcp_text(result, str("range must look like 15m, 1h, 90s, 2d or today"), true);
    }
    McpReach reach = mcp_reach();
    bool reached = from < reach.earliest;
    if (reached) from = reach.earliest;
    if (to <= from) return mcp_text(result, mcp_reach_note(a), false);
    char error[256] = {};
    i32 lines = 0;
    AppLabels labels = mcp_labels(a);
    Str text = clip_text(&labels, a, from, to, &lines, error, sizeof(error), reach.hidden);
    if (error[0]) return mcp_text(result, str(error), true);
    if (!lines) {
        Str note = reached ? mcp_reach_note(a) : Str{"", 0};
        return mcp_text(result, str_fmt(a, "%.*s%s%.*s(nothing was heard then)", (int)note.len, note.ptr, note.len ? "\n" : "",
                                        (int)text.len, text.ptr), false);
    }
    if (text.len > MCP_MOST) {
        // Keep the heading line and the latest lines, from the start of a line.
        i64 head = 0;
        while (head < text.len && text.ptr[head] != '\n') head++;
        Str tail = str_tail_lines(text, MCP_MOST);
        text = str_fmt(a, "%.*s\n(earlier lines left out: ask for a shorter stretch to see them)\n%.*s", (int)head, text.ptr,
                       (int)tail.len, tail.ptr);
    }
    if (reached) {
        Str note = mcp_reach_note(a);
        text = str_fmt(a, "%.*s\n%.*s", (int)note.len, note.ptr, (int)text.len, text.ptr);
    }
    mcp_text(result, text, false);
}

static void mcp_search(const Json* args, Arena* a, Buf* result) {
    Str query = str_trim(json_text(json_get(args, "query")));
    const Json* lim = json_get(args, "limit");
    i32 limit = lim && lim->type == JSON_NUMBER ? (i32)lim->num : 30;
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;
    if (!query.len) return mcp_text(result, str("say what to search for"), true);
    f64 from = 0, to = now_ts();
    Str from_s = json_text(json_get(args, "from")), to_s = json_text(json_get(args, "to"));
    if (from_s.len && !mcp_time(from_s, &from)) return mcp_text(result, str("from must look like 2026-11-18 13:54"), true);
    if (to_s.len && !mcp_time(to_s, &to)) return mcp_text(result, str("to must look like 2026-11-18 14:10"), true);
    McpReach reach = mcp_reach();
    bool cut = from < reach.earliest;
    if (cut) from = reach.earliest;

    // Each word quoted (so nothing typed is read as query syntax) and all of them required, in any
    // order; the last one may be the start of a word.
    Buf match = {};
    i32 words = 0;
    for (i64 i = 0; i < query.len;) {
        while (i < query.len && !(isalnum((u8)query.ptr[i]) || (u8)query.ptr[i] >= 0x80)) i++;
        i64 start = i;
        while (i < query.len && (isalnum((u8)query.ptr[i]) || (u8)query.ptr[i] >= 0x80)) i++;
        if (i == start) break;
        buf_fmt(&match, "%s\"%.*s\"", words++ ? " " : "", (int)(i - start), query.ptr + start);
    }
    if (!words) return mcp_text(result, str("say what to search for, in words"), true);
    buf_cstr(&match, " *");
    sqlite3* db = db_open_path(true);
    if (!db) {
        buf_free(&match);
        return mcp_text(result, str("could not open the database"), true);
    }
    sqlite3_stmt* st = prepare(db, "SELECT " LINE_COLUMNS " FROM segments_fts f JOIN segments s ON s.id = f.rowid "
                                   "WHERE segments_fts MATCH ?1 AND s.start_ts >= ?2 AND s.start_ts <= ?3 "
                                   "ORDER BY s.start_ts DESC LIMIT 5000");
    Slice<Line> all = {};
    if (st) {
        sqlite3_bind_text(st, 1, match.chars.items, (int)match.chars.count, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 2, from);
        sqlite3_bind_double(st, 3, to);
        all = read_lines(st, a, 5000);
    }
    sqlite3_close(db);
    buf_free(&match);
    if (!st) return mcp_text(result, str("could not read the database"), true);
    Slice<Line> found = {PUSH_ARRAY(a, Line, all.count), 0};
    for (i32 i = 0; i < all.count && found.count < limit; i++)
        if (!names_has(reach.hidden, all.items[i].speaker)) found.items[found.count++] = all.items[i];
    AppLabels labels = mcp_labels(a);
    Buf t = {};
    if (cut) {
        Str note = mcp_reach_note(a);
        buf_fmt(&t, "%.*s\n", (int)note.len, note.ptr);
    }
    for (i32 i = 0; i < found.count; i++) {
        Line* l = &found.items[i];
        LocalTime lt = local_time(l->start_ts);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d", lt.year, lt.month, lt.day, lt.hour, lt.minute, lt.second);
        line_format(&t, stamp, label_of(&labels, l->speaker), l);
    }
    if (!found.count) buf_cstr(&t, "(no lines have all of those words)");
    mcp_text(result, {t.chars.items, t.chars.count}, false);
    buf_free(&t);
}

static void mcp_summary(const Json* args, Arena* a, Buf* result) {
    char day[16];
    Str asked = str_trim(json_text(json_get(args, "day")));
    if (asked.len) SET(day, asked);
    else day_of(now_ts() - 86400, day);
    i32 y, m, d;
    if (!parse_day(day, &y, &m, &d)) return mcp_text(result, str("day must look like 2026-11-18"), true);
    // A summary tells of its whole day, so it is shared only when all of the day is within reach.
    f64 day_from = 0, day_to = 0;
    day_bounds(day, &day_from, &day_to);
    if (day_from < mcp_reach().earliest)
        return mcp_text(result, str_fmt(a, "lifelog shares only the last %s with agents, so not the summary of %s",
                                        g_cli.config.mcp.reach, day), false);
    sqlite3* db = db_open_path(true);
    if (!db) return mcp_text(result, str("could not open the database"), true);
    sqlite3_stmt* st = prepare(db, "SELECT status, summary FROM summaries WHERE day = ?1");
    Str status = {}, text = {};
    if (st) {
        sqlite3_bind_text(st, 1, day, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            status = col_str(st, 0, a);
            text = col_str(st, 1, a);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (!st) return mcp_text(result, str("could not read the database"), true);
    if (str_eq(status, str("done")) && text.len) return mcp_text(result, text, false);
    mcp_text(result, str_fmt(a, "there is no summary of %s%s", day, str_eq(status, str("skipped")) ? " (it was skipped)" : ""), false);
}

static void mcp_status(Arena* a, Buf* result) {
    Str body = {};
    char error[256] = {};
    if (!api_call(g_cli.config.api_port, "GET", str("/status"), a, &body, error, sizeof(error)))
        return mcp_text(result, str_fmt(a, "lifelog is not running (%s)", error), true);
    mcp_text(result, body, false);
}

// One message. Notifications (no id) get no answer.
static void mcp_handle(Str line, Arena* a) {
    Json msg = {};
    char error[256] = {};
    if (!json_parse(a, line, &msg, error, sizeof(error))) return mcp_fail(0, -32700, error);
    const Json* id = json_get(&msg, "id");
    bool has_id = id && id->type != JSON_NULL;
    Str method = json_text(json_get(&msg, "method"));
    const Json* params = json_get(&msg, "params");
    Buf result = {};
    if (str_eq(method, str("initialize"))) {
        // The client's version when it gives one: these four tools are the same in every version.
        Str want = json_text(json_get(params, "protocolVersion"));
        buf_cstr(&result, "{\"protocolVersion\":");
        json_quote(&result, want.len ? want : str(MCP_VERSION));
        buf_cstr(&result, ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"lifelog\",\"title\":\"lifelog\","
                          "\"version\":\"1\"},\"instructions\":\"lifelog is an always-on transcript of this computer: "
                          "the user's mic (speaker 'me') and the apps and calls it hears. Use search to find when something "
                          "was said, and transcript to read around it. Times are the computer's local time.");
        if (g_cli.config.mcp.reach[0])
            buf_fmt(&result, " The user shares only the last %s with agents.", g_cli.config.mcp.reach);
        buf_cstr(&result, "\"}");
    } else if (str_eq(method, str("ping"))) {
        buf_cstr(&result, "{}");
    } else if (str_eq(method, str("tools/list"))) {
        buf_fmt(&result, "{\"tools\":%s}", MCP_TOOLS);
    } else if (str_eq(method, str("tools/call"))) {
        Str name = json_text(json_get(params, "name"));
        const Json* args = json_get(params, "arguments");
        if (str_eq(name, str("transcript"))) mcp_transcript(args, a, &result);
        else if (str_eq(name, str("search"))) mcp_search(args, a, &result);
        else if (str_eq(name, str("summary"))) mcp_summary(args, a, &result);
        else if (str_eq(name, str("status"))) mcp_status(a, &result);
        else {
            buf_free(&result);
            if (has_id) mcp_fail(id, -32602, "there is no tool by that name");
            return;
        }
    } else {
        buf_free(&result);
        if (has_id) mcp_fail(id, -32601, "lifelog does not know that method");
        return;
    }
    if (has_id) mcp_reply(id, &result);
    buf_free(&result);
}

// `lifelog mcp`: answers messages until stdin closes.
static i32 cli_mcp() {
    if (!cli_config(&g_cli.config)) return 1;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!handle_ok(in)) return cli_error("lifelog mcp is started by an AI agent, which talks to it on stdin and stdout");
    List<char> pending = {};
    char chunk[65536];
    Arena a = {};
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, chunk, sizeof(chunk), &got, 0) || got == 0) break;
        for (DWORD i = 0; i < got; i++) list_push(&pending, chunk[i]);
        // Each whole line is one message.
        i64 start = 0;
        for (i64 i = 0; i < pending.count; i++) {
            if (pending.items[i] != '\n') continue;
            Str line = str_trim({pending.items + start, i - start});
            if (line.len) mcp_handle(line, &a);
            arena_release(&a);
            a = {};
            start = i + 1;
        }
        if (start) {
            memmove(pending.items, pending.items + start, pending.count - start);
            pending.count -= (i32)start;
        }
    }
    list_free(&pending);
    return 0;
}
