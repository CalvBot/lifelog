// summary.cpp: see summary.h. The daily summary, written by a local model (llm.cpp).
//
// It needs the graphics card for a few minutes, so it never just starts: at ask_at a
// notification says it starts in 5 minutes, with buttons to put it off or skip the day (and it
// asks again an hour later if put off). It waits to ask while a game is running. While it
// writes, the transcription worker steps aside (Worker.summary_needs_gpu), so the two models
// never share the GPU.
//
// The day runs 04:00 to 04:00, so a late night belongs to the day before. Long days are done in
// two steps: short notes per part of the day, then the summary from the notes.
//
// Reminders that pop up when they are due live here too (the alerts table): after a summary,
// each reminder that says when ("tomorrow", "at 5") is offered as a notification ("Remind you
// tomorrow at 09:00?"); the journal's bell sets one too. The clock thread shows them on time, or
// at the next start if the PC was off then.
//
// Every database call opens its own short-lived connection, as the old version did, so the
// threads never share one.

static const char* TOAST_TAG = "summary";
// About 4 000 tokens of transcript per request, leaving room for a month of earlier days (~1 500
// tokens) and the answer in the context.
static const i64 CHUNK_CHARS = 16000;
// What one call app (the other side of a conversation) adds after each time I speak.
static const i64 APP_RUN_CHARS = 800;
// What media (a film, a video, music) adds after each time I speak: just enough to know what I
// was reacting to.
static const i64 MEDIA_RUN_CHARS = 200;
static const f64 COUNTDOWN_SECS = 5 * 60;
// After a game, wait this long before asking, so it never pops up between two matches.
static const f64 AFTER_GAME_SECS = 10 * 60;
// Saying fewer words than this in a day makes it a quiet day: no AI, just that.
static const i64 QUIET_DAY_WORDS = 60;
// A part of a long day where I said fewer words than this is only noted as quiet.
static const i64 QUIET_PART_WORDS = 12;
// At most this many "Remind you…?" questions after one summary.
static const i32 MAX_ASKS = 3;

// Apps whose audio is other people talking with me. Everything else is media playing.
static const char* CALL_APPS[] = {"discord", "teams", "ms-teams", "zoom", "slack", "telegram",
                                  "whatsapp", "skype", "signal", "mumble", "teamspeak"};

// ---------------------------------------------------------------------------------------------
// Small text helpers

// Copies into a fixed buffer like SET, but never cuts a UTF-8 character in half.
static void set_utf8(char* dst, i64 cap, Str s) {
    if (s.len >= cap) {
        s.len = cap - 1;
        while (s.len > 0 && ((u8)s.ptr[s.len] & 0xC0) == 0x80) s.len--;
    }
    set(dst, cap, s);
}

// the old app's str::lines: splits on \n, drops a \r before it, no last empty line.
static bool next_line(Str* rest, Str* line) {
    if (rest->len <= 0) return false;
    i64 nl = str_find(*rest, str("\n"));
    *line = nl < 0 ? *rest : str_sub(*rest, 0, nl);
    *rest = nl < 0 ? Str{"", 0} : str_sub(*rest, nl + 1);
    if (line->len && line->ptr[line->len - 1] == '\r') line->len--;
    return true;
}

static Str trim_start(Str s) {
    while (s.len && isspace((u8)s.ptr[0])) s = str_sub(s, 1);
    return s;
}

// Lower case (ASCII), with ’ as ', for matching what was said.
static Str lower_quote(Arena* a, Str s) {
    char* out = PUSH_ARRAY(a, char, s.len + 1);
    i64 n = 0;
    for (i64 i = 0; i < s.len; i++) {
        if (i + 2 < s.len && (u8)s.ptr[i] == 0xE2 && (u8)s.ptr[i + 1] == 0x80 && (u8)s.ptr[i + 2] == 0x99) {
            out[n++] = '\'';
            i += 2;
            continue;
        }
        char c = s.ptr[i];
        out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out[n] = 0;
    return {out, n};
}

static bool has(Str hay, const char* needle) { return str_find(hay, str(needle)) >= 0; }

// `s` with every trailing ".exe" taken off (case as it is, like the old app's trim_end_matches).
static Str trim_exe(Str s) {
    while (str_ends(s, str(".exe"))) s.len -= 4;
    return s;
}

static i64 count_words(Str s) {
    i64 n = 0;
    bool in = false;
    for (i64 i = 0; i < s.len; i++) {
        bool space = isspace((u8)s.ptr[i]) != 0;
        if (!space && !in) n++;
        in = !space;
    }
    return n;
}

static bool is_call_app(Str speaker) {
    Mark m = mark(scratch());
    Str name = str_lower(scratch(), trim_exe(speaker));
    bool call = false;
    for (i32 i = 0; i < COUNT(CALL_APPS); i++) call |= str_starts(name, str(CALL_APPS[i]));
    rewind(m);
    return call;
}

// The text of a Markdown bullet line, if it is one.
static bool bullet_text(Str line, Str* out) {
    Str l = trim_start(line);
    if (str_starts(l, str("- ")) || str_starts(l, str("* "))) l = str_sub(l, 2);
    else if (str_starts(l, str("• "))) l = str_sub(l, (i64)strlen("• "));
    else return false;
    *out = str_trim(l);
    return true;
}

// Removes bold labels ("**Evening**") left with no bullets under them.
static Str drop_empty_labels(Arena* a, Str text) {
    Buf out = {};
    Str rest = text, line = {};
    bool first = true;
    while (next_line(&rest, &line)) {
        Str t = str_trim(line);
        bool is_label = str_starts(t, str("**")) && str_ends(t, str("**")) && t.len > 4;
        if (is_label) {
            Str ahead = rest, next = {};
            bool bullet_next = false;
            while (next_line(&ahead, &next)) {
                Str n = str_trim(next);
                if (!n.len) continue;
                Str b = {};
                bullet_next = bullet_text(n, &b);
                break;
            }
            if (!bullet_next) continue;
        }
        if (!first) buf_cstr(&out, "\n");
        buf_add(&out, line);
        first = false;
    }
    return str_trim(buf_take(&out, a));
}

// Safety net for the never-list: drops bullets that name a blocked topic outright. `topics` are
// lower case.
static Str strip_never(Arena* a, Str text, Str* topics, i32 topic_count) {
    if (!topic_count) return text;
    Buf kept = {};
    Str rest = text, line = {};
    bool first = true;
    while (next_line(&rest, &line)) {
        Str b = {};
        if (bullet_text(line, &b)) {
            Mark m = mark(scratch());
            Str low = str_lower(scratch(), b);
            bool blocked = false;
            for (i32 i = 0; i < topic_count; i++) blocked |= str_find(low, topics[i]) >= 0;
            rewind(m);
            if (blocked) continue;
        }
        if (!first) buf_cstr(&kept, "\n");
        buf_add(&kept, line);
        first = false;
    }
    Mark m = mark(scratch());
    Str joined = buf_take(&kept, scratch());
    Str out = drop_empty_labels(a, joined);
    rewind(m);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Dates and times

static const char* WEEKDAYS[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
static const char* MONTHS[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

// A day as "YYYY-MM-DD", checked.
static bool parse_day(const char* day, i32* y, i32* m, i32* d) {
    if (!day || strlen(day) != 10 || day[4] != '-' || day[7] != '-') return false;
    for (i32 i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdigit((u8)day[i])) return false;
    *y = atoi(day);
    *m = atoi(day + 5);
    *d = atoi(day + 8);
    return *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31;
}

// "HH:MM" (or "H:MM"), checked.
static bool parse_hhmm(Str s, i32* h, i32* m) {
    s = str_trim(s);
    i64 colon = str_find(s, str(":"));
    if (colon < 1 || colon > 2 || s.len != colon + 3) return false;
    for (i64 i = 0; i < s.len; i++)
        if (i != colon && !isdigit((u8)s.ptr[i])) return false;
    *h = atoi(s.ptr);
    *m = atoi(s.ptr + colon + 1);
    return *h < 24 && *m < 60;
}

// The local date `n` days after `day` (noon, so a clock change cannot move it).
static LocalTime day_plus(i32 y, i32 m, i32 d, i32 n) { return local_time(local_ts(y, m, d + n, 12, 0)); }

static void day_text(LocalTime t, char out[11]) { snprintf(out, 11, "%04d-%02d-%02d", t.year, t.month, t.day); }

// "Sunday 15 November 2026" (chrono's "%A %-d %B %Y").
static Str long_date(Arena* a, const char* day) {
    i32 y, m, d;
    if (!parse_day(day, &y, &m, &d)) return str_copy(a, str(day));
    LocalTime t = day_plus(y, m, d, 0);
    return str_fmt(a, "%s %d %s %d", WEEKDAYS[t.weekday], t.day, MONTHS[t.month - 1], t.year);
}

// "Sun 15 Nov" (chrono's "%a %-d %b").
static Str short_date(Arena* a, const char* day) {
    i32 y, m, d;
    if (!parse_day(day, &y, &m, &d)) return str_copy(a, str(day));
    LocalTime t = day_plus(y, m, d, 0);
    return str_fmt(a, "%.3s %d %.3s", WEEKDAYS[t.weekday], t.day, MONTHS[t.month - 1]);
}

static void clock_text(f64 ts, char out[6]) {
    LocalTime t = local_time(ts);
    snprintf(out, 6, "%02d:%02d", t.hour, t.minute);
}

// When to ask about `day`: ask_at on that day (times before 04:00 fall on the next morning).
static f64 ask_moment(const char* day, const char* ask_at) {
    i32 h = 21, mi = 0, y, m, d;
    if (!parse_hhmm(str(ask_at), &h, &mi)) h = 21, mi = 0;
    CHECK(parse_day(day, &y, &m, &d), "a day as YYYY-MM-DD, not \"%s\"", day);
    return local_ts(y, m, d + (h < 4 ? 1 : 0), h, mi);
}

// The next time the clock shows h:m: today, or tomorrow if that has passed.
static f64 next_at(i32 h, i32 m) {
    f64 now = now_ts();
    LocalTime t = local_time(now);
    f64 today = local_ts(t.year, t.month, t.day, h, m);
    return today > now ? today : local_ts(t.year, t.month, t.day + 1, h, m);
}

// Days from today's local date to `ts`'s.
static i32 days_from_today(f64 ts) {
    LocalTime a = local_time(now_ts()), b = local_time(ts);
    f64 diff = local_ts(b.year, b.month, b.day, 12, 0) - local_ts(a.year, a.month, a.day, 12, 0);
    return (i32)floor(diff / 86400.0 + 0.5);
}

// "today at 17:00", "tomorrow at 09:00", "on Tue 17 Nov at 09:00".
static Str when_text(Arena* a, f64 ts) {
    LocalTime t = local_time(ts);
    i32 days = days_from_today(ts);
    if (days == 0) return str_fmt(a, "today at %02d:%02d", t.hour, t.minute);
    if (days == 1) return str_fmt(a, "tomorrow at %02d:%02d", t.hour, t.minute);
    return str_fmt(a, "on %.3s %d %.3s at %02d:%02d", WEEKDAYS[t.weekday], t.day, MONTHS[t.month - 1], t.hour, t.minute);
}

// When `time` ("HH:MM") on `day` was; before 04:00 is the night after.
static f64 said_ts(const char* day, Str time) {
    i32 h = 0, mi = 0, y, m, d;
    if (!parse_hhmm(time, &h, &mi)) h = 0, mi = 0;
    if (!parse_day(day, &y, &m, &d)) return 0;
    return local_ts(y, m, d + (h < 4 ? 1 : 0), h, mi);
}

// When a reminder is due: its `when` ("YYYY-MM-DD HH:MM", local), if that is after it was said
// and within two months. 0 when it did not say when.
static f64 due_of(const char* day, const Json* r) {
    Str when = str_trim(json_text(json_get(r, "when")));
    if (when.len != 16 || when.ptr[10] != ' ') return 0;
    char date[11];
    memcpy(date, when.ptr, 10);
    date[10] = 0;
    i32 y, m, d, h, mi;
    if (!parse_day(date, &y, &m, &d) || !parse_hhmm(str_sub(when, 11), &h, &mi)) return 0;
    f64 due = local_ts(y, m, d, h, mi);
    f64 said = said_ts(day, json_text(json_get(r, "time")));
    return (due > said && due < said + 62.0 * 86400.0) ? due : 0;
}

// The next 09:00, for a reminder that did not say when.
static f64 next_morning() { return next_at(9, 0); }

static Str minutes(Arena* a, f64 secs) {
    u64 m = (u64)floor(secs / 60.0 + 0.5);
    if (m >= 60) return str_fmt(a, "%llu h %llu min", m / 60, m % 60);
    return str_fmt(a, "%llu min", m);
}

// ---------------------------------------------------------------------------------------------
// The window's state

static void phase_set(App* app, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->summary.phase, sizeof(app->summary.phase), fmt, args);
    va_end(args);
    wake_window(app);
}

// For the threads that do not hold the lock.
static void phase_set_unlocked(App* app, const char* fmt, ...) {
    char text[sizeof(app->summary.phase)];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    app_lock(app);
    SET(app->summary.phase, str(text));
    app_unlock(app);
    wake_window(app);
}

static sqlite3_stmt* sql(sqlite3* db, const char* text) {
    sqlite3_stmt* st = 0;
    if (sqlite3_prepare_v2(db, text, -1, &st, 0) != SQLITE_OK) {
        ERR("summary query failed: %s (%s)", sqlite3_errmsg(db), text);
        return 0;
    }
    return st;
}

static Str col_text(sqlite3_stmt* st, int i, Arena* a) {
    const char* t = (const char*)sqlite3_column_text(st, i);
    return str_copy(a, {t ? t : "", t ? sqlite3_column_bytes(st, i) : 0});
}

static void bind_str(sqlite3_stmt* st, int i, Str s) { sqlite3_bind_text(st, i, s.ptr ? s.ptr : "", (int)s.len, SQLITE_TRANSIENT); }

// Runs a statement that changes rows; returns how many, or -1 on failure.
static i32 sql_done(sqlite3* db, sqlite3_stmt* st) {
    if (!st) return -1;
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        ERR("summary write failed: %s", sqlite3_errmsg(db));
        return -1;
    }
    return sqlite3_changes(db);
}

static void lists_read(sqlite3* db, List<SummaryDay>* days, List<NeverTopic>* never) {
    sqlite3_stmt* st = sql(db, "SELECT day, status, created_ts, reminders FROM summaries ORDER BY day DESC LIMIT 400");
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        SummaryDay d = {};
        SET(d.day, str((const char*)sqlite3_column_text(st, 0)));
        SET(d.status, str((const char*)sqlite3_column_text(st, 1)));
        d.created_ts = sqlite3_column_double(st, 2);
        Mark m = mark(scratch());
        Json r = {};
        if (json_parse(scratch(), col_text(st, 3, scratch()), &r, 0, 0)) d.reminders = json_len(&r);
        rewind(m);
        list_push(days, d);
    }
    sqlite3_finalize(st);
    st = sql(db, "SELECT id, topic FROM summary_never ORDER BY id");
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        NeverTopic t = {};
        t.id = sqlite3_column_int64(st, 0);
        set_utf8(t.topic, sizeof(t.topic), str((const char*)sqlite3_column_text(st, 1)));
        list_push(never, t);
    }
    sqlite3_finalize(st);
}

static void lists_publish(App* app, List<SummaryDay> days, List<NeverTopic> never) {
    list_free(&app->summary.days);
    list_free(&app->summary.never);
    app->summary.days = days;
    app->summary.never = never;
    app->summary.version++;
    wake_window(app);
}

// Reads the days and never-topics again, with the lock held (short reads).
static void lists_refresh_locked(App* app, sqlite3* db) {
    List<SummaryDay> days = {};
    List<NeverTopic> never = {};
    lists_read(db, &days, &never);
    lists_publish(app, days, never);
}

static void lists_refresh(App* app, sqlite3* db) {
    List<SummaryDay> days = {};
    List<NeverTopic> never = {};
    lists_read(db, &days, &never);
    app_lock(app);
    lists_publish(app, days, never);
    app_unlock(app);
}

// ---------------------------------------------------------------------------------------------
// Storage

// 1 if `day` has a summary (or was skipped), 0 if not, -1 if the database could not say.
static i32 status_of(sqlite3* db, const char* day) {
    sqlite3_stmt* st = sql(db, "SELECT status FROM summaries WHERE day = ?1");
    if (!st) return -1;
    bind_str(st, 1, str(day));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

static bool save(sqlite3* db, const char* day, const char* status, const char* model, Str summary, Str reminders) {
    sqlite3_stmt* st = sql(db, "INSERT OR REPLACE INTO summaries (day, status, created_ts, model, summary, reminders) "
                               "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    if (!st) return false;
    bind_str(st, 1, str(day));
    bind_str(st, 2, str(status));
    sqlite3_bind_double(st, 3, now_ts());
    if (model) bind_str(st, 4, str(model));
    if (summary.ptr) bind_str(st, 5, summary);
    bind_str(st, 6, reminders);
    return sql_done(db, st) >= 0;
}

// The alert for `day`'s reminder `text`: false if there is none.
static bool alert_find(sqlite3* db, Arena* a, const char* day, Str text, i64* id, f64* due, Str* state) {
    sqlite3_stmt* st = sql(db, "SELECT id, due_ts, state FROM alerts WHERE day = ?1 AND text = ?2");
    if (!st) return false;
    bind_str(st, 1, str(day));
    bind_str(st, 2, text);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) {
        *id = sqlite3_column_int64(st, 0);
        *due = sqlite3_column_double(st, 1);
        *state = col_text(st, 2, a);
    }
    sqlite3_finalize(st);
    return found;
}

// A day's saved row: its summary text and its reminders as JSON.
struct SavedRow {
    bool found;
    Str status;
    f64 created_ts;
    Str model;
    Str summary;
    Json reminders;
};

static SavedRow saved_row(sqlite3* db, Arena* a, const char* day) {
    SavedRow row = {};
    sqlite3_stmt* st = sql(db, "SELECT status, created_ts, model, summary, reminders FROM summaries WHERE day = ?1");
    if (!st) return row;
    bind_str(st, 1, str(day));
    if (sqlite3_step(st) == SQLITE_ROW) {
        row.found = true;
        row.status = col_text(st, 0, a);
        row.created_ts = sqlite3_column_double(st, 1);
        row.model = col_text(st, 2, a);
        row.summary = col_text(st, 3, a);
        if (!json_parse(a, col_text(st, 4, a), &row.reminders, 0, 0) || row.reminders.type != JSON_ARRAY) {
            row.reminders = {};
            row.reminders.type = JSON_ARRAY;
        }
    }
    sqlite3_finalize(st);
    return row;
}

StoredSummary summary_get(sqlite3* db, Arena* a, const char* day) {
    StoredSummary out = {};
    if (!db) return out;
    SavedRow row = saved_row(db, a, day);
    if (!row.found) return out;
    out.found = true;
    SET(out.day, str(day));
    out.status = row.status;
    out.created_ts = row.created_ts;
    out.model = row.model;
    out.markdown = row.summary;
    out.reminder_count = row.reminders.count;
    out.reminders = PUSH_ARRAY(a, Reminder, out.reminder_count);
    for (i32 i = 0; i < out.reminder_count; i++) {
        const Json* j = &row.reminders.items[i];
        Reminder* r = &out.reminders[i];
        r->time = json_text(json_get(j, "time"));
        r->text = json_text(json_get(j, "text"));
        r->said = json_text(json_get(j, "said"));
        r->when = json_text(json_get(j, "when"));
        // Its notification, if any; else when it would be due, if it said.
        i64 id = 0;
        f64 due = 0;
        Str state = {};
        if (alert_find(db, a, day, str_trim(r->text), &id, &due, &state)) {
            r->alert_state = state;
        } else {
            due = due_of(day, j);
            if (due) r->alert_state = str("none");
        }
        if (due) {
            r->alert_due = due;
            r->alert_when = when_text(a, due);
        }
    }
    return out;
}

// Also kept as summaries\YYYY-MM-DD.md next to the database, readable without the app.
static bool write_markdown(App* app, const char* day, Str summary, const Json* reminders) {
    Mark m = mark(scratch());
    Buf text = {};
    buf_fmt(&text, "# %s\n\n", long_date(scratch(), day).ptr);
    buf_add(&text, summary);
    buf_cstr(&text, "\n");
    if (json_len(reminders)) {
        buf_cstr(&text, "\n## Reminders\n\n");
        for (i32 i = 0; i < reminders->count; i++) {
            Str t = json_text(json_get(&reminders->items[i], "text"));
            Str time = json_text(json_get(&reminders->items[i], "time"));
            buf_fmt(&text, "- [ ] %.*s (%.*s)\n", (int)t.len, t.ptr, (int)time.len, time.ptr);
        }
    }
    Str data = buf_take(&text, scratch());
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\summaries", wide(scratch(), str(app->config.data_dir)));
    SHCreateDirectoryExW(0, dir, 0);
    swprintf(path, MAX_PATH, L"%ls\\%hs.md", dir, day);
    bool ok = write_file(path, data);
    rewind(m);
    return ok;
}

static Str json_string_of(Arena* a, const Json* j) {
    Buf b = {};
    json_write(&b, j);
    return buf_take(&b, a);
}

// ---------------------------------------------------------------------------------------------
// The day's lines, as the summary reads them

struct SLine {
    f64 start_ts;
    f64 end_ts;
    Str source;   // "mic" | "app" | "call" (one person on a call, from lifelog-vencord)
    Str speaker;
    Str title;
    Str text;
};

static bool is_mic(const SLine* l) { return str_eq(l->source, str("mic")); }

static Slice<SLine> day_lines(sqlite3* db, Arena* a, f64 from, f64 to) {
    Slice<SLine> out = {};
    sqlite3_stmt* st = sql(db, "SELECT start_ts, end_ts, source, speaker, title, text FROM segments "
                               "WHERE end_ts >= ?1 AND start_ts <= ?2 ORDER BY start_ts, id LIMIT 20000");
    if (!st) return out;
    sqlite3_bind_double(st, 1, from);
    sqlite3_bind_double(st, 2, to);
    List<SLine> list = {};
    while (sqlite3_step(st) == SQLITE_ROW) {
        SLine l = {};
        l.start_ts = sqlite3_column_double(st, 0);
        l.end_ts = sqlite3_column_double(st, 1);
        l.source = col_text(st, 2, a);
        l.speaker = col_text(st, 3, a);
        l.title = col_text(st, 4, a);
        l.text = col_text(st, 5, a);
        list_push(&list, l);
    }
    sqlite3_finalize(st);
    out.items = PUSH_ARRAY(a, SLine, list.count);
    if (list.count) memcpy(out.items, list.items, sizeof(SLine) * list.count);
    out.count = list.count;
    list_free(&list);
    return out;
}

// Seconds of transcribed speech in a day, split into mine and the apps'.
static bool talk_secs(sqlite3* db, const char* day, f64* mine, f64* apps) {
    *mine = *apps = 0;
    f64 from, to;
    day_bounds(day, &from, &to);
    sqlite3_stmt* st = sql(db, "SELECT COALESCE(SUM(CASE WHEN source = 'mic' THEN end_ts - start_ts END), 0), "
                               "COALESCE(SUM(CASE WHEN source <> 'mic' THEN end_ts - start_ts END), 0) "
                               "FROM segments WHERE start_ts >= ?1 AND start_ts < ?2");
    if (!st) return false;
    sqlite3_bind_double(st, 1, from);
    sqlite3_bind_double(st, 2, to);
    bool ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok) {
        *mine = sqlite3_column_double(st, 0);
        *apps = sqlite3_column_double(st, 1);
    }
    sqlite3_finalize(st);
    return ok;
}

// The summary is about what I said: a call only goes in within a minute of me speaking (the other
// side of the conversation), media only within seconds of it (what I was reacting to), and
// never a film or album playing on its own.
static Slice<SLine> context_only(Arena* a, Slice<SLine> lines) {
    const f64 CALL_NEAR = 60.0, MEDIA_NEAR = 10.0;
    List<i32> mine = {};  // indexes of my lines, in start order
    for (i32 i = 0; i < lines.count; i++)
        if (is_mic(&lines.items[i])) list_push(&mine, i);
    Slice<SLine> out = {PUSH_ARRAY(a, SLine, lines.count), 0};
    for (i32 i = 0; i < lines.count; i++) {
        SLine* l = &lines.items[i];
        bool keep = is_mic(l);
        if (!keep) {
            f64 close_secs = (str_eq(l->source, str("call")) || is_call_app(l->speaker)) ? CALL_NEAR : MEDIA_NEAR;
            // My lines near this one: start the scan at the first that starts after (start - close_secs - 120).
            i32 lo = 0, hi = mine.count;
            while (lo < hi) {
                i32 mid = (lo + hi) / 2;
                if (lines.items[mine.items[mid]].start_ts < l->start_ts - close_secs - 120.0) lo = mid + 1;
                else hi = mid;
            }
            for (i32 k = lo; k < mine.count && lines.items[mine.items[k]].start_ts <= l->end_ts + close_secs; k++)
                if (lines.items[mine.items[k]].end_ts >= l->start_ts - close_secs) {
                    keep = true;
                    break;
                }
        }
        if (keep) out.items[out.count++] = *l;
    }
    list_free(&mine);
    return out;
}

struct Chunk {
    f64 first;
    f64 last;
    Str text;
};

struct RunCount {
    Str who;
    i64 chars;
};

// The day's lines as compact text, cut into pieces that fit one request. Long stretches of one
// app (a film, an album) are shortened; what I said is always kept in full.
static Slice<Chunk> transcript_chunks(Arena* a, Slice<SLine> all) {
    Slice<SLine> lines = context_only(a, all);
    struct OutLine {
        f64 start, end;
        Str text;
    };
    List<OutLine> out_lines = {};
    // Characters each app has had since I last spoke; past APP_RUN_CHARS the rest is left out.
    List<RunCount> since_me = {};
    Str last_who = {}, last_title = {};
    bool have_last = false;
    for (i32 i = 0; i < lines.count; i++) {
        SLine* line = &lines.items[i];
        Str text = str_trim(line->text);
        if (!text.len) continue;
        char at[6];
        clock_text(line->start_ts, at);
        if (is_mic(line)) {
            since_me.count = 0;
            list_push(&out_lines, {line->start_ts, line->end_ts, str_fmt(a, "[%s] me: %.*s", at, (int)text.len, text.ptr)});
            continue;
        }
        // "call" lines come one person per stream (lifelog-vencord) and are named after the speaker.
        bool call = str_eq(line->source, str("call")) || is_call_app(line->speaker);
        // Media is marked as playing, so it is never read as someone talking.
        Str name = trim_exe(line->speaker);
        Str who = call ? str_copy(a, name) : str_fmt(a, "%.*s playing", (int)name.len, name.ptr);
        RunCount* run = 0;
        for (i32 k = 0; k < since_me.count; k++)
            if (str_eq(since_me.items[k].who, who)) run = &since_me.items[k];
        if (!run) run = list_push(&since_me, {who, 0});
        if (run->chars >= (call ? APP_RUN_CHARS : MEDIA_RUN_CHARS)) continue;
        run->chars += text.len;
        // Name the window once per change of app or title.
        bool same = have_last && str_eq(last_who, who) && str_eq(last_title, line->title);
        Str label = (line->title.len && !same) ? str_fmt(a, "%.*s (%.*s)", (int)who.len, who.ptr, (int)line->title.len, line->title.ptr) : who;
        last_who = who;
        last_title = line->title;
        have_last = true;
        list_push(&out_lines, {line->start_ts, line->end_ts,
                               str_fmt(a, "[%s] %.*s: %.*s", at, (int)label.len, label.ptr, (int)text.len, text.ptr)});
    }
    list_free(&since_me);

    List<Chunk> chunks = {};
    Buf text = {};
    f64 first = 0, last = 0;
    for (i32 i = 0; i < out_lines.count; i++) {
        OutLine* l = &out_lines.items[i];
        if (buf_len(&text) && buf_len(&text) + l->text.len > CHUNK_CHARS) list_push(&chunks, {first, last, buf_take(&text, a)});
        if (!buf_len(&text)) first = l->start;
        last = l->end;
        buf_add(&text, l->text);
        buf_cstr(&text, "\n");
    }
    if (buf_len(&text)) list_push(&chunks, {first, last, buf_take(&text, a)});
    list_free(&out_lines);
    Slice<Chunk> out = {PUSH_ARRAY(a, Chunk, chunks.count), chunks.count};
    if (chunks.count) memcpy(out.items, chunks.items, sizeof(Chunk) * chunks.count);
    list_free(&chunks);
    return out;
}

// Words I said (the 'me' lines) in a transcript chunk.
static i64 my_words(Str chunk) {
    i64 n = 0;
    Str rest = chunk, line = {};
    while (next_line(&rest, &line)) {
        i64 at = str_find(line, str("] me: "));
        if (at >= 0) n += count_words(str_sub(line, at + 6));
    }
    return n;
}

// What a quiet day's summary says, without asking the model.
static const char* quiet_summary(i64 words) { return words == 0 ? "You were quiet all day." : "You were quiet most of the day."; }

// ---------------------------------------------------------------------------------------------
// The prompts (word for word as the old version's)

static const char* SYSTEM =
    "You help one person keep a diary from an automatic transcript of their day. "
    "Lines marked 'me' are what they said into their microphone: to people in the room, on calls, or to themselves. "
    "Call apps (like 'discord') are other people talking with them. Lines marked 'playing' are media on their PC "
    "(films, videos, music, games): never people they talked to, never their words, only there to show what they reacted "
    "to. The window title is in brackets. "
    "The diary is only about what 'me' said and talked about. Other speakers are there only to make sense of it "
    "(what a question was, who a reply went to); never summarise what others said or what played on its own. "
    "Only 'me' lines are the person: a video, stream or call in an app is never something they said or did, even when "
    "it says 'I'. Feelings, opinions, worries and plans come only from their own 'me' words: never say they felt, "
    "thought or wanted something because a video, film or song said it (a video about cringe is not them feeling "
    "cringe). If they said little, say so plainly (for example 'You were quiet most of the day') rather than "
    "filling the diary from app audio. "
    "The speech recognition makes mistakes, and song lyrics or video dialogue are not the person's own words. "
    "Be specific, never vague: name the people, games, projects, places, things and numbers that came up, and say what "
    "was actually said, decided or found out, not just that something was 'discussed'. Vague: 'Talked about work "
    "frustrations.' Specific: 'Said the build server broke again and you plan to move it to the NAS.' "
    "Be factual and brief, never invent details, and write to the person as 'you'.";

static const char* REMEMBER_RULE =
    "remember = one item per flagged moment, in order: time (HH:MM of the flag) and "
    "point (what you wanted to remember, one short sentence from your own words around the flag; keep names, numbers "
    "and dates exact). An empty list when there are no flagged moments.";

static const char* REMINDERS_RULE =
    "reminders = only clear plans or promises you made out loud for later: "
    "\"I'll…\", \"I need to…\", \"I have to…\", \"I'm going to…\", \"remind me to…\", \"don't let me forget…\", \"we should…\", "
    "\"let's… tomorrow\". For each: time (HH:MM), said (your exact words from a 'me' line), text (a short to-do, "
    "like \"Call the dentist\"), when (\"YYYY-MM-DD HH:MM\" if you said when to do it: \"tomorrow\", \"tonight\", "
    "\"at 5\", \"on Monday\", \"in an hour\"; counted from the time you said it, using the date given; tomorrow with no "
    "time is 09:00, tonight is 20:00; \"\" if you did not say when). Comments, questions, feelings, game chatter and anything from videos or songs are NOT "
    "reminders. Most days have none or one or two; an empty list is right when nothing qualifies.";

#define REMINDERS_SCHEMA                                                                                          \
    "{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"time\":{\"type\":\"string\"},\"said\":" \
    "{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"when\":{\"type\":\"string\"}},\"required\":"          \
    "[\"time\",\"said\",\"text\",\"when\"]}}"
static const char* SUMMARY_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"summary\":{\"type\":\"string\"},\"reminders\":" REMINDERS_SCHEMA
    ",\"remember\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"time\":{\"type\":\"string\"},"
    "\"point\":{\"type\":\"string\"}},\"required\":[\"time\",\"point\"]}}},\"required\":[\"summary\",\"reminders\",\"remember\"]}";
static const char* NOTES_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"notes\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"reminders\":" REMINDERS_SCHEMA
    "},\"required\":[\"notes\",\"reminders\"]}";

// Tells the model what it must leave out, from the never-list.
static Str never_rule(Arena* a, Str* never, i32 count) {
    if (!count) return {"", 0};
    Buf b = {};
    buf_cstr(&b, " Never mention these topics, not even in passing or reworded: ");
    for (i32 i = 0; i < count; i++) {
        if (i) buf_cstr(&b, ", ");
        buf_fmt(&b, "\"%.*s\"", (int)never[i].len, never[i].ptr);
    }
    buf_cstr(&b, ".");
    return buf_take(&b, a);
}

static Str summary_prompt(Arena* a, const char* day, u32 max_words, Str material, Str history, Str never) {
    Buf b = {};
    if (history.len) {
        buf_cstr(&b, "Your earlier days, newest first (context only: use it to recognise recurring people and projects and "
                     "to notice follow-ups, like finishing something from earlier; never retell those days):\n");
        buf_add(&b, history);
        buf_cstr(&b, "\n");
    }
    buf_add(&b, material);
    buf_fmt(&b,
            "\n\nReturn JSON. summary = %s only, in at most %u words, in Markdown: short "
            "bullet points grouped under bold labels for the parts of the day (Morning, Afternoon, Evening, Night — only "
            "those with something in them, each label once and in order), about what you said: topics you talked about, plans, what you worked on, the "
            "important things first, each bullet with its concrete details. Leave out what others said or what played "
            "unless you talked about it, and no list of songs, videos or artists. Do not repeat the flagged moments in the "
            "summary; they get their own list. Write less rather than pad: a quiet day can be two bullets.",
            long_date(scratch(), day).ptr, max_words);
    buf_add(&b, never);
    buf_cstr(&b, " ");
    buf_cstr(&b, REMINDERS_RULE);
    buf_cstr(&b, " Merge duplicates. ");
    buf_cstr(&b, REMEMBER_RULE);
    return buf_take(&b, a);
}

// The month before `day`, for context: the last week's summaries in full (up to ~500 characters
// each), older days as one line.
static Str history(sqlite3* db, Arena* a, const char* day, Str* never, i32 never_count) {
    i32 y, m, d;
    if (!parse_day(day, &y, &m, &d)) return {"", 0};
    char from[11];
    day_text(day_plus(y, m, d, -30), from);
    sqlite3_stmt* st = sql(db, "SELECT day, summary FROM summaries WHERE status = 'done' AND day < ?1 AND day >= ?2 ORDER BY day DESC");
    if (!st) return {"", 0};
    bind_str(st, 1, str(day));
    bind_str(st, 2, str(from));
    Buf out = {};
    for (i32 i = 0; sqlite3_step(st) == SQLITE_ROW; i++) {
        Mark mk = mark(scratch());
        Str row_day = col_text(st, 0, scratch());
        Str text = strip_never(scratch(), col_text(st, 1, scratch()), never, never_count);
        // Bullets only (no labels or "To remember" heading), one per line.
        Buf line = {};
        Str rest = text, l = {};
        bool first = true;
        while (next_line(&rest, &l)) {
            Str b = {};
            if (!bullet_text(l, &b)) continue;
            if (!first) buf_cstr(&line, "; ");
            buf_add(&line, b);
            first = false;
        }
        i64 budget = i < 7 ? 500 : 110;
        Str joined = buf_take(&line, scratch());
        if (joined.len > budget) {
            i64 cut = budget;
            while (cut > 0 && ((u8)joined.ptr[cut] & 0xC0) == 0x80) cut--;
            joined = str_fmt(scratch(), "%.*s…", (int)cut, joined.ptr);
        }
        if (joined.len) buf_fmt(&out, "- %s: %s\n", short_date(scratch(), row_day.ptr).ptr, joined.ptr);
        rewind(mk);
    }
    sqlite3_finalize(st);
    return buf_take(&out, a);
}

// ---------------------------------------------------------------------------------------------
// "need to remember" and reminders

// A moment I flagged with "need to remember": when, my words, and the talk around it.
struct Flag {
    f64 ts;
    Str said;
    Str context;
};

// Finds the flagged moments in what I said (never in apps' audio), with ~30 s before and 45 s
// after as context, since the thing to remember can come either side of the phrase.
static Slice<Flag> flagged_moments(Arena* a, Slice<SLine> lines) {
    List<Flag> flags = {};
    for (i32 i = 0; i < lines.count; i++) {
        SLine* line = &lines.items[i];
        if (!is_mic(line)) continue;
        Mark m = mark(scratch());
        bool cue = has(lower_quote(scratch(), line->text), "need to remember");
        rewind(m);
        if (!cue) continue;
        if (flags.count && line->start_ts - flags.items[flags.count - 1].ts < 60.0) continue;  // the same moment said twice
        Buf context = {};
        for (i32 k = 0; k < lines.count; k++) {
            SLine* l = &lines.items[k];
            if (!(l->end_ts >= line->start_ts - 30.0 && l->start_ts <= line->end_ts + 45.0)) continue;
            if (buf_len(&context) >= 2000) continue;
            Str who = is_mic(l) ? str("me") : trim_exe(l->speaker);
            Str text = str_trim(l->text);
            char at[6];
            clock_text(l->start_ts, at);
            buf_fmt(&context, "[%s] %.*s: %.*s\n", at, (int)who.len, who.ptr, (int)text.len, text.ptr);
        }
        list_push(&flags, {line->start_ts, str_copy(a, str_trim(line->text)), buf_take(&context, a)});
    }
    Slice<Flag> out = {PUSH_ARRAY(a, Flag, flags.count), flags.count};
    if (flags.count) memcpy(out.items, flags.items, sizeof(Flag) * flags.count);
    list_free(&flags);
    return out;
}

static Str flags_material(Arena* a, Slice<Flag> flags) {
    if (!flags.count) return {"", 0};
    Buf b = {};
    buf_cstr(&b, "\n\nMoments where you said \"need to remember\" (each MUST be in `remember`):\n");
    for (i32 i = 0; i < flags.count; i++) {
        char at[6];
        clock_text(flags.items[i].ts, at);
        buf_fmt(&b, "\nFlag at %s:\n", at);
        buf_add(&b, flags.items[i].context);
    }
    return buf_take(&b, a);
}

// The "To remember" part of the summary: the model's point for each flag, or my own words if it
// missed one, so a flagged moment is never lost.
static Str remember_section(Arena* a, Slice<Flag> flags, const Json* from_model) {
    if (!flags.count) return {"", 0};
    i32 n = json_len(from_model);
    Buf b = {};
    buf_cstr(&b, "\n\n**To remember**\n");
    for (i32 i = 0; i < flags.count; i++) {
        char at[6];
        clock_text(flags.items[i].ts, at);
        const Json* item = 0;
        for (i32 k = 0; k < n && !item; k++)
            if (str_eq(json_text(json_get(json_at(from_model, k), "time")), str(at))) item = json_at(from_model, k);
        if (!item && n == flags.count) item = json_at(from_model, i);
        Str point = item ? str_trim(json_text(json_get(item, "point"))) : Str{"", 0};
        if (point.len) buf_fmt(&b, "- %.*s (%s)\n", (int)point.len, point.ptr, at);
        else buf_fmt(&b, "- \"%.*s\" (%s)\n", (int)flags.items[i].said.len, flags.items[i].said.ptr, at);
    }
    return buf_take(&b, a);
}

// Words that mark a plan or promise. A "reminder" whose quote has none of them is the model
// over-reaching, and is dropped.
static bool is_commitment(Str said) {
    static const char* CUES[] = {"i'll", "i will", "i need to", "i have to", "i've got to", "i gotta", "i'm going to",
                                 "i'm gonna", "i am going to", "remind me", "don't forget", "don't let me forget",
                                 "remember to", "we should", "we need to", "we'll", "let's", "i should", "i must",
                                 "i want to", "tomorrow", "tonight"};
    Mark m = mark(scratch());
    Str low = lower_quote(scratch(), said);
    bool any = false;
    for (i32 i = 0; i < COUNT(CUES) && !any; i++) any = has(low, CUES[i]);
    rewind(m);
    return any;
}

// The next UTF-8 character of `s` at `*i`; moves `*i` past it.
static u32 next_char(Str s, i64* i) {
    u8 c = (u8)s.ptr[*i];
    i32 n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    u32 cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (i32 k = 1; k < n && *i + k < s.len; k++) cp = (cp << 6) | ((u8)s.ptr[*i + k] & 0x3F);
    *i += n;
    return cp;
}

// Letters and digits; outside ASCII, anything but the punctuation blocks.
static bool is_word_char(u32 c) {
    if (c < 0x80) return isalnum((int)c) || c == '\'';
    if (c <= 0xBF || c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return false;
    if (c >= 0x3000 && c <= 0x303F) return false;
    return true;
}

// Words of 3+ letters, lower case, for matching a quote to what was said.
static Slice<Str> word_set(Arena* a, Str s) {
    Str low = lower_quote(a, s);
    List<Str> words = {};
    i64 i = 0;
    while (i < low.len) {
        i64 start = i, chars = 0;
        i64 at = i;
        while (at < low.len) {
            i64 next = at;
            if (!is_word_char(next_char(low, &next))) break;
            at = next;
            chars++;
        }
        if (at == start) {
            next_char(low, &i);
            continue;
        }
        i = at;
        if (chars < 3) continue;
        Str w = str_sub(low, start, at - start);
        bool dup = false;
        for (i32 k = 0; k < words.count && !dup; k++) dup = str_eq(words.items[k], w);
        if (!dup) list_push(&words, w);
    }
    Slice<Str> out = {PUSH_ARRAY(a, Str, words.count), words.count};
    if (words.count) memcpy(out.items, words.items, sizeof(Str) * words.count);
    list_free(&words);
    return out;
}

// Whether `said` is really (most of) one of my lines, not words the model made up or took from
// a film or a friend.
static bool said_by_me(Str said, Slice<Slice<Str>> mine) {
    Mark m = mark(scratch());
    Slice<Str> words = word_set(scratch(), said);
    bool found = false;
    for (i32 l = 0; l < mine.count && words.count && !found; l++) {
        i64 hits = 0;
        for (i32 w = 0; w < words.count; w++)
            for (i32 k = 0; k < mine.items[l].count; k++)
                if (str_eq(mine.items[l].items[k], words.items[w])) {
                    hits++;
                    break;
                }
        found = hits * 10 >= (i64)words.count * 7;
    }
    rewind(m);
    return found;
}

// The reminders worth keeping, as a JSON array in `a`.
static Json keep_real_reminders(Arena* a, const Json* list, Slice<Slice<Str>> mine) {
    Json out = {};
    out.type = JSON_ARRAY;
    i32 n = json_len(list);
    out.items = PUSH_ARRAY(a, Json, n);
    for (i32 i = 0; i < n; i++) {
        const Json* r = json_at(list, i);
        Str said = json_text(json_get(r, "said"));
        Mark m = mark(scratch());
        Str text = lower_quote(scratch(), str_trim(json_text(json_get(r, "text"))));
        bool keep = is_commitment(said) && said_by_me(said, mine) && count_words(text) >= 2 && !has(text, "something");
        rewind(m);
        if (keep) out.items[out.count++] = *r;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Reminder notifications

static void remind_link(char* out, i32 cap, const char* action, i64 id) { make_link(out, cap, "remind/%s?id=%lld", action, id); }

// After a summary: asks about each new reminder that said when, if that is still to come.
static void alerts_offer(App* app, sqlite3* db, const char* day, const Json* reminders) {
    i32 asked = 0;
    for (i32 i = 0; i < json_len(reminders); i++) {
        const Json* r = json_at(reminders, i);
        f64 due = due_of(day, r);
        if (!due) continue;
        Str text = str_trim(json_text(json_get(r, "text")));
        if (due <= now_ts() + 60 || !text.len || asked >= MAX_ASKS) continue;
        // Already asked, set or turned down (an earlier run of this day's summary): leave it.
        Mark m = mark(scratch());
        i64 id = 0;
        f64 old_due = 0;
        Str state = {};
        bool known = alert_find(db, scratch(), day, text, &id, &old_due, &state);
        rewind(m);
        if (known) continue;
        Str said = str_trim(json_text(json_get(r, "said")));
        sqlite3_stmt* st = sql(db, "INSERT INTO alerts (day, text, said, said_time, due_ts, state) VALUES (?1, ?2, ?3, ?4, ?5, 'asked')");
        if (!st) return;
        bind_str(st, 1, str(day));
        bind_str(st, 2, text);
        bind_str(st, 3, said);
        bind_str(st, 4, json_text(json_get(r, "time")));
        sqlite3_bind_double(st, 5, due);
        if (sql_done(db, st) < 0) return;
        id = sqlite3_last_insert_rowid(db);

        ToastJob t = {};
        snprintf(t.tag, sizeof(t.tag), "remind-%lld", id);
        set_utf8(t.title, sizeof(t.title), str_fmt(scratch(), "Remind you %s?", when_text(scratch(), due).ptr));
        set_utf8(t.body, sizeof(t.body), str_fmt(scratch(), "%.*s. You said: “%.*s”", (int)text.len, text.ptr, (int)said.len, said.ptr));
        char yes[160], no[160];
        remind_link(yes, sizeof(yes), "yes", id);
        remind_link(no, sizeof(no), "no", id);
        toast_button(&t, "Remind me", yes);
        toast_button(&t, "No thanks", no);
        t.sticky = true;
        rewind(m);
        notify_show(app, &t);
        asked++;
    }
}

// Shows the reminders that have come due. Unlocked; on the summary thread.
static void alerts_show_due(App* app, sqlite3* db) {
    sqlite3_stmt* st = sql(db, "SELECT id, day, text, said, said_time FROM alerts WHERE state = 'set' AND due_ts <= ?1");
    if (!st) return;
    sqlite3_bind_double(st, 1, now_ts());
    struct Due {
        i64 id;
        char day[11];
        Str text, said, time;
    };
    Mark m = mark(scratch());
    List<Due> due = {};
    while (sqlite3_step(st) == SQLITE_ROW) {
        Due d = {};
        d.id = sqlite3_column_int64(st, 0);
        SET(d.day, str((const char*)sqlite3_column_text(st, 1)));
        d.text = col_text(st, 2, scratch());
        d.said = col_text(st, 3, scratch());
        d.time = col_text(st, 4, scratch());
        list_push(&due, d);
    }
    sqlite3_finalize(st);
    for (i32 i = 0; i < due.count; i++) {
        Due* d = &due.items[i];
        sqlite3_stmt* up = sql(db, "UPDATE alerts SET state = 'shown' WHERE id = ?1");
        if (!up) break;
        sqlite3_bind_int64(up, 1, d->id);
        if (sql_done(db, up) < 0) break;
        i32 y, mo, dd;
        Str when = parse_day(d->day, &y, &mo, &dd) ? str(WEEKDAYS[day_plus(y, mo, dd, 0).weekday]) : str(d->day);
        ToastJob t = {};
        snprintf(t.tag, sizeof(t.tag), "remind-%lld", d->id);
        set_utf8(t.title, sizeof(t.title), d->text);
        if (!d->said.len) set_utf8(t.body, sizeof(t.body), str_fmt(scratch(), "From %s", when.ptr));
        else
            set_utf8(t.body, sizeof(t.body),
                     str_fmt(scratch(), "You said on %s at %s: “%s”", when.ptr, d->time.ptr, d->said.ptr));
        char done[160], snooze[160];
        remind_link(done, sizeof(done), "done", d->id);
        remind_link(snooze, sizeof(snooze), "snooze", d->id);
        toast_button(&t, "Done", done);
        toast_button(&t, "In an hour", snooze);
        make_link(t.launch, sizeof(t.launch), "summary/open?day=%s", d->day);
        t.sticky = true;
        notify_show(app, &t);
        LOG("reminder shown: %s", d->text.ptr);
    }
    bool any = due.count > 0;
    list_free(&due);
    rewind(m);
    if (any) {
        app_lock(app);
        app->summary.version++;
        app_unlock(app);
        wake_window(app);
    }
}

void reminder_answer(App* app, i64 id, const char* action) {
    sqlite3* db = db_open(app);
    if (!db) return;
    char tag[32];
    snprintf(tag, sizeof(tag), "remind-%lld", id);
    notify_remove(app, tag);
    const char* text = 0;
    if (strcmp(action, "yes") == 0) text = "UPDATE alerts SET state = 'set' WHERE id = ?1";
    else if (strcmp(action, "no") == 0) text = "UPDATE alerts SET state = 'no' WHERE id = ?1";
    else if (strcmp(action, "done") == 0) text = "UPDATE alerts SET state = 'shown' WHERE id = ?1";
    else if (strcmp(action, "snooze") == 0) text = "UPDATE alerts SET state = 'set', due_ts = ?2 WHERE id = ?1";
    if (!text) {
        WARN("reminder answer must be yes, no, done or snooze, not \"%s\"", action);
        sqlite3_close(db);
        return;
    }
    sqlite3_stmt* st = sql(db, text);
    if (st) {
        sqlite3_bind_int64(st, 1, id);
        if (strcmp(action, "snooze") == 0) sqlite3_bind_double(st, 2, now_ts() + 3600.0);
    }
    i32 changed = sql_done(db, st);
    sqlite3_close(db);
    if (changed == 0) WARN("no reminder %lld", id);
    if (changed > 0) {
        LOG("reminder %lld: %s", id, action);
        app->summary.version++;
        wake_window(app);
    }
}

Str reminder_set(App* app, Arena* a, const char* day, i32 n, bool on) {
    sqlite3* db = db_open(app);
    if (!db) return {"", 0};
    Mark m = mark(scratch());
    SavedRow row = saved_row(db, scratch(), day);
    Str result = {"", 0};
    i32 y, mo, d;
    if (!row.found) {
        ui_say(app, "No summary for that day");
    } else if (n < 0 || n >= row.reminders.count) {
        ui_say(app, "No such reminder");
    } else if (!parse_day(day, &y, &mo, &d)) {
        ui_say(app, "Bad day");
    } else {
        const Json* r = &row.reminders.items[n];
        Str text = str_trim(json_text(json_get(r, "text")));
        i64 id = 0;
        f64 old_due = 0;
        Str state = {};
        bool known = alert_find(db, scratch(), day, text, &id, &old_due, &state);
        if (!on) {
            if (known) {
                sqlite3_stmt* st = sql(db, "UPDATE alerts SET state = 'no' WHERE id = ?1");
                if (st) sqlite3_bind_int64(st, 1, id);
                sql_done(db, st);
            }
        } else {
            f64 due = due_of(day, r);
            if (due <= now_ts()) due = next_morning();
            sqlite3_stmt* st = 0;
            if (known) {
                st = sql(db, "UPDATE alerts SET state = 'set', due_ts = ?2 WHERE id = ?1");
                if (st) {
                    sqlite3_bind_int64(st, 1, id);
                    sqlite3_bind_double(st, 2, due);
                }
            } else {
                st = sql(db, "INSERT INTO alerts (day, text, said, said_time, due_ts, state) VALUES (?1, ?2, ?3, ?4, ?5, 'set')");
                if (st) {
                    bind_str(st, 1, str(day));
                    bind_str(st, 2, text);
                    bind_str(st, 3, json_text(json_get(r, "said")));
                    bind_str(st, 4, json_text(json_get(r, "time")));
                    sqlite3_bind_double(st, 5, due);
                }
            }
            if (sql_done(db, st) >= 0) result = when_text(a, due);
        }
        app->summary.version++;
        wake_window(app);
    }
    rewind(m);
    sqlite3_close(db);
    return result;
}

// ---------------------------------------------------------------------------------------------
// Dismissing and the never-list

void summary_dismiss(App* app, const char* day, Str bullet, i32 reminder) {
    sqlite3* db = db_open(app);
    if (!db) return;
    Mark m = mark(scratch());
    SavedRow row = saved_row(db, scratch(), day);
    if (!row.found) {
        ui_say(app, "No summary for that day");
    } else {
        Str text = row.summary;
        bullet = str_trim(bullet);
        if (bullet.len) {
            Buf kept = {};
            Str rest = text, line = {};
            bool first = true;
            while (next_line(&rest, &line)) {
                Str b = {};
                if (bullet_text(line, &b) && str_eq(b, bullet)) continue;
                if (!first) buf_cstr(&kept, "\n");
                buf_add(&kept, line);
                first = false;
            }
            text = drop_empty_labels(scratch(), buf_take(&kept, scratch()));
        }
        Json* list = &row.reminders;
        bool ok = true;
        if (reminder >= 0) {
            if (reminder < list->count) {
                memmove(&list->items[reminder], &list->items[reminder + 1], sizeof(Json) * (list->count - reminder - 1));
                list->count--;
            } else {
                ui_say(app, "No such reminder");
                ok = false;
            }
        }
        if (ok) {
            sqlite3_stmt* st = sql(db, "UPDATE summaries SET summary = ?1, reminders = ?2 WHERE day = ?3");
            if (st) {
                bind_str(st, 1, text);
                bind_str(st, 2, json_string_of(scratch(), list));
                bind_str(st, 3, str(day));
            }
            if (sql_done(db, st) >= 0) {
                i32 y, mo, d;
                if (parse_day(day, &y, &mo, &d)) write_markdown(app, day, text, list);
                lists_refresh_locked(app, db);
            }
        }
    }
    rewind(m);
    sqlite3_close(db);
}

void summary_add_never(App* app, Str topic, const char* day, Str bullet) {
    topic = str_trim(topic);
    if (!topic.len) {
        ui_say(app, "Say what to leave out");
        return;
    }
    sqlite3* db = db_open(app);
    if (!db) return;
    sqlite3_stmt* st = sql(db, "INSERT INTO summary_never (topic, created_ts) VALUES (?1, ?2)");
    if (st) {
        bind_str(st, 1, topic);
        sqlite3_bind_double(st, 2, now_ts());
    }
    if (sql_done(db, st) >= 0) {
        LOG("the summary will never mention: %.*s", (int)topic.len, topic.ptr);
        lists_refresh_locked(app, db);
    }
    sqlite3_close(db);
    if (day && day[0] && bullet.len) summary_dismiss(app, day, bullet, -1);
}

void summary_remove_never(App* app, i64 id) {
    sqlite3* db = db_open(app);
    if (!db) return;
    sqlite3_stmt* st = sql(db, "DELETE FROM summary_never WHERE id = ?1");
    if (st) sqlite3_bind_int64(st, 1, id);
    if (sql_done(db, st) >= 0) lists_refresh_locked(app, db);
    sqlite3_close(db);
}

// ---------------------------------------------------------------------------------------------
// Settings

void summary_set_enabled(App* app, bool on) {
    app->config.summary.enabled = on;
    config_save_bool("summary", "enabled", on);
}

void summary_set_ask_at(App* app, Str hhmm) {
    i32 h, m;
    if (!parse_hhmm(hhmm, &h, &m)) {
        ui_say(app, "Time must look like 21:00 (got '%.*s')", (int)hhmm.len, hhmm.ptr);
        return;
    }
    snprintf(app->config.summary.ask_at, sizeof(app->config.summary.ask_at), "%02d:%02d", h, m);
    config_save_string("summary", "ask_at", app->config.summary.ask_at);
}

void summary_set_max_words(App* app, u32 words) {
    words = words < 50 ? 50 : words > 2000 ? 2000 : words;
    app->config.summary.max_words = words;
    config_save_u32("summary", "max_words", words);
}

void summary_set_ask_reminders(App* app, bool on) {
    app->config.summary.ask_reminders = on;
    config_save_bool("summary", "ask_reminders", on);
}

// ---------------------------------------------------------------------------------------------
// Writing

// Waits `ms`, false if the program is ending.
static bool pause_ms(App* app, u32 ms) { return sleep_unless_stopping(app, ms); }

static bool write_day(App* app, const char* day, char* error, i32 cap) {
    app_lock(app);
    Config cfg = app->config;  // a copy: the settings as they are now
    app_unlock(app);
    LOG("writing the summary for %s with %s", day, cfg.summary.model);

    // 1. Let transcription catch up, so the end of the day is in it.
    phase_set_unlocked(app, "waiting for transcription to catch up");
    f64 started = now_ts(), idle_since = now_ts();
    while (now_ts() - started < 20 * 60) {
        app_lock(app);
        i32 queued = app->worker.queued;
        bool running = app->worker.running;
        app_unlock(app);
        if (queued == 0) break;
        if (running) idle_since = now_ts();
        else if (now_ts() - idle_since > 60) break;  // transcription is paused or waiting for a game: sum up what there is
        if (!pause_ms(app, 2000)) break;
    }

    // 2. Take the GPU from the worker.
    app_lock(app);
    app->worker.summary_needs_gpu = true;
    app_unlock(app);
    f64 freed = now_ts();
    for (;;) {
        app_lock(app);
        bool running = app->worker.running;
        app_unlock(app);
        if (!running || now_ts() - freed > 30 || !pause_ms(app, 250)) break;
    }
    if (stopping(app)) {
        snprintf(error, cap, "lifelog is closing");
        return false;
    }

    sqlite3* db = db_open(app);
    if (!db) {
        snprintf(error, cap, "could not open the database");
        return false;
    }
    Arena work = {};
    Arena* a = &work;
    bool ok = false;
    Llm llm = {};
    f64 from, to;
    day_bounds(day, &from, &to);
    Slice<SLine> lines = day_lines(db, a, from, to < now_ts() ? to : now_ts());
    Slice<Chunk> chunks = transcript_chunks(a, lines);
    Slice<Flag> flags = flagged_moments(a, lines);
    Slice<Slice<Str>> mine = {PUSH_ARRAY(a, Slice<Str>, lines.count), 0};
    for (i32 i = 0; i < lines.count; i++)
        if (is_mic(&lines.items[i])) mine.items[mine.count++] = word_set(a, lines.items[i].text);
    i64 words = 0;
    for (i32 i = 0; i < chunks.count; i++) words += my_words(chunks.items[i].text);
    Str flagged = flags_material(a, flags);
    List<Str> never = {};
    {
        sqlite3_stmt* st = sql(db, "SELECT topic FROM summary_never ORDER BY id");
        while (st && sqlite3_step(st) == SQLITE_ROW) list_push(&never, col_text(st, 0, a));
        sqlite3_finalize(st);
    }
    List<Str> never_low = {};
    for (i32 i = 0; i < never.count; i++) list_push(&never_low, str_lower(a, never.items[i]));
    Str past = history(db, a, day, never_low.items, never_low.count);
    Str rule = never_rule(a, never.items, never.count);
    Json result = {};

    if (!chunks.count) {
        snprintf(error, cap, "nothing was transcribed that day");
        goto done;
    }
    phase_set_unlocked(app, "starting the AI");
    if (words < QUIET_DAY_WORDS) {
        // Barely a word said: that is the summary; the apps' audio is not the day.
        Str quiet = str_fmt(a, "{\"summary\":\"%s\",\"reminders\":[],\"remember\":[]}", quiet_summary(words));
        json_parse(a, quiet, &result, 0, 0);
    } else {
        if (!llm_open(app, &cfg, 12288, &llm, error, cap)) goto done;
        if (chunks.count == 1) {
            phase_set_unlocked(app, "writing the summary");
            Str material = str_fmt(a, "Transcript of your day (you said %lld words in it):\n\n%s%s", words, chunks.items[0].text.ptr, flagged.ptr);
            Str prompt = summary_prompt(a, day, cfg.summary.max_words, material, past, rule);
            if (!llm_json(&llm, a, str(SYSTEM), prompt, str(SUMMARY_SCHEMA), &result, error, cap)) goto done;
        } else {
            // Long day: notes for each part, then the summary from the notes.
            Buf material = {};
            buf_fmt(&material, "Notes from your day, in order (you said %lld words in all):\n", words);
            List<Json> candidates = {};
            bool parts_ok = true;
            for (i32 i = 0; i < chunks.count && parts_ok; i++) {
                Chunk* c = &chunks.items[i];
                char t0[6], t1[6];
                clock_text(c->first, t0);
                clock_text(c->last, t1);
                if (my_words(c->text) < QUIET_PART_WORDS) {
                    buf_fmt(&material, "\n%s–%s\n- You said almost nothing (only apps playing).\n", t0, t1);
                    continue;
                }
                phase_set_unlocked(app, "reading the day (part %d of %d)", i + 1, chunks.count);
                Buf prompt = {};
                buf_fmt(&prompt, "Transcript from %s to %s on %s (times after midnight are the next morning):\n\n", t0, t1,
                        long_date(a, day).ptr);
                buf_add(&prompt, c->text);
                buf_cstr(&prompt,
                         "\n\nReturn JSON. notes = 1 to 6 short bullet points on what you "
                         "talked about and did in this part, from your own lines (others only as context; skip small talk and "
                         "trivia), each with its concrete details (names, things, numbers, what was decided). Every note must come "
                         "from your own 'me' lines; a topic only others or media brought up is left out, and the plot of a "
                         "film or video is never a note (at most: \"Watched Avatar with friends on Discord\").");
                buf_add(&prompt, rule);
                buf_cstr(&prompt, " ");
                buf_cstr(&prompt, REMINDERS_RULE);
                Json part = {};
                if (!llm_json(&llm, a, str(SYSTEM), buf_take(&prompt, a), str(NOTES_SCHEMA), &part, error, cap)) {
                    parts_ok = false;
                    break;
                }
                buf_fmt(&material, "\n%s–%s\n", t0, t1);
                const Json* notes = json_get(&part, "notes");
                for (i32 k = 0; k < json_len(notes); k++) {
                    const Json* note = json_at(notes, k);
                    if (note->type == JSON_STRING) buf_fmt(&material, "- %s\n", note->str.ptr);
                }
                Json kept = keep_real_reminders(a, json_get(&part, "reminders"), mine);
                for (i32 k = 0; k < kept.count; k++) list_push(&candidates, kept.items[k]);
            }
            if (parts_ok) {
                if (candidates.count) {
                    buf_cstr(&material, "\nPossible reminders (time, your words):\n");
                    for (i32 k = 0; k < candidates.count; k++) {
                        Str t = json_text(json_get(&candidates.items[k], "time"));
                        Str s = json_text(json_get(&candidates.items[k], "said"));
                        buf_fmt(&material, "- %.*s \"%.*s\"\n", (int)t.len, t.ptr, (int)s.len, s.ptr);
                    }
                }
                buf_add(&material, flagged);
                phase_set_unlocked(app, "writing the summary");
                Str prompt = summary_prompt(a, day, cfg.summary.max_words, buf_take(&material, a), past, rule);
                parts_ok = llm_json(&llm, a, str(SYSTEM), prompt, str(SUMMARY_SCHEMA), &result, error, cap);
            }
            buf_free(&material);
            list_free(&candidates);
            if (!parts_ok) goto done;
        }
    }
    // The server is stopped here, and the graphics card is free again.
    llm_close(&llm);
    app_lock(app);
    app->worker.summary_needs_gpu = false;
    app_unlock(app);

    {
        Str summary = strip_never(a, str_trim(json_text(json_get(&result, "summary"))), never_low.items, never_low.count);
        if (!summary.len && !flags.count) {
            snprintf(error, cap, "the model returned an empty summary");
            goto done;
        }
        Str section = remember_section(a, flags, json_get(&result, "remember"));
        while (section.len && isspace((u8)section.ptr[section.len - 1])) section.len--;
        summary = str_trim(str_fmt(a, "%.*s%.*s", (int)summary.len, summary.ptr, (int)section.len, section.ptr));
        Json reminders = keep_real_reminders(a, json_get(&result, "reminders"), mine);
        if (!save(db, day, "done", cfg.summary.model, summary, json_string_of(a, &reminders))) {
            snprintf(error, cap, "could not save the summary: %s", sqlite3_errmsg(db));
            goto done;
        }
        lists_refresh(app, db);
        if (cfg.summary.ask_reminders) alerts_offer(app, db, day, &reminders);
        if (!write_markdown(app, day, summary, &reminders)) WARN("could not write the summary file for %s", day);
        i32 count = reminders.count;
        LOG("summary for %s done (%lld words, %d reminders)", day, count_words(summary), count);
        phase_set_unlocked(app, "done");

        Buf plain = {};
        Str rest = summary, line = {};
        i32 taken = 0;
        while (taken < 3 && next_line(&rest, &line)) {
            if (!str_trim(line).len) continue;
            if (taken++) buf_cstr(&plain, " ");
            for (i64 i = 0; i < line.len; i++) {
                if (i + 1 < line.len && line.ptr[i] == '*' && line.ptr[i + 1] == '*') {
                    i++;
                    continue;
                }
                list_push(&plain.chars, line.ptr[i]);
            }
        }
        Str text = buf_take(&plain, a);
        ToastJob t = {};
        SET(t.tag, str(TOAST_TAG));
        SET(t.title, str("Your day, summed up"));
        if (count == 0) set_utf8(t.body, sizeof(t.body), text);
        else if (count == 1) set_utf8(t.body, sizeof(t.body), str_fmt(a, "1 reminder. %s", text.ptr));
        else set_utf8(t.body, sizeof(t.body), str_fmt(a, "%d reminders. %s", count, text.ptr));
        char open[160];
        make_link(open, sizeof(open), "summary/open?day=%s", day);
        toast_button(&t, "Read it", open);
        SET(t.launch, str(open));
        notify_show(app, &t);
        ok = true;
    }

done:
    llm_close(&llm);
    list_free(&never);
    list_free(&never_low);
    arena_release(&work);
    sqlite3_close(db);
    return ok;
}

static DWORD WINAPI writer_main(void* param) {
    App* app = (App*)param;
    app_lock(app);
    char day[11];
    SET(day, str(app->summary.writing_day));
    app_unlock(app);
    char error[512] = {};
    bool ok = write_day(app, day, error, sizeof(error));
    app_lock(app);
    app->worker.summary_needs_gpu = false;
    app->summary.writing = false;
    app->summary.writing_day[0] = 0;
    if (!ok) {
        ERR("summary for %s: %s", day, error);
        phase_set(app, "failed: %s", error);
    }
    app_unlock(app);
    wake_window(app);
    if (!ok && !stopping(app)) {
        ToastJob t = {};
        SET(t.tag, str(TOAST_TAG));
        SET(t.title, str("The summary could not be written"));
        set_utf8(t.body, sizeof(t.body), str(error));
        char again[160];
        make_link(again, sizeof(again), "summary/now?day=%s", day);
        toast_button(&t, "Try again", again);
        notify_show(app, &t);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Asking

static void ask(App* app, sqlite3* db, const char* day) {
    f64 mine = 0, apps = 0;
    talk_secs(db, day, &mine, &apps);
    Mark m = mark(scratch());
    ToastJob t = {};
    SET(t.tag, str(TOAST_TAG));
    SET(t.title, str("Your summary starts in 5 minutes"));
    set_utf8(t.body, sizeof(t.body),
             str_fmt(scratch(), "It uses the graphics card for a few minutes. Today: %s of you talking, %s from apps.",
                     minutes(scratch(), mine).ptr, minutes(scratch(), apps).ptr));
    static const char* ACTIONS[][2] = {{"Start now", "now"}, {"In an hour", "later"}, {"Delay until…", "pick"}, {"Skip today", "skip"}};
    for (i32 i = 0; i < COUNT(ACTIONS); i++) {
        char link[160];
        make_link(link, sizeof(link), "summary/%s?day=%s", ACTIONS[i][1], day);
        toast_button(&t, ACTIONS[i][0], link);
    }
    t.sticky = true;
    rewind(m);
    char at[6];
    clock_text(now_ts() + COUNTDOWN_SECS, at);
    app_lock(app);
    app->summary.countdown_ts = now_ts() + COUNTDOWN_SECS;
    SET(app->summary.countdown_day, str(day));
    phase_set(app, "starts at %s", at);
    app_unlock(app);
    notify_show(app, &t);
    LOG("the summary for %s starts at %s unless delayed", day, at);
}

Str summary_act(App* app, Arena* a, const char* action, const char* day_in, const char* at) {
    char day[11];
    i32 y, m, d;
    if (day_in && day_in[0]) {
        if (!parse_day(day_in, &y, &m, &d)) return str_fmt(a, "bad day '%s'", day_in);
        SET(day, str(day_in));
    } else {
        day_of(now_ts(), day);
    }
    // "Delay until…" only opens the delay box; the countdown goes on until a time is picked.
    if (strcmp(action, "pick") == 0) {
        ui_popup(app, POPUP_DELAY, day);
        return str_copy(a, str("pick a time"));
    }
    notify_remove(app, TOAST_TAG);
    app->summary.countdown_ts = 0;
    if (strcmp(action, "now") == 0) {
        if (app->summary.writing) return str_copy(a, str("already writing"));
        app->summary.writing = true;
        SET(app->summary.writing_day, str(day));
        start_thread(app, "summary writer", writer_main);
        wake_window(app);
        return str_copy(a, str("writing"));
    }
    if (strcmp(action, "later") == 0) {
        app->summary.snooze_ts = now_ts() + 3600;
        LOG("summary for %s: asking again in an hour", day);
        phase_set(app, "asking again in an hour");
        return str_copy(a, str("asking again in an hour"));
    }
    if (strcmp(action, "until") == 0) {
        i32 h, mi;
        if (!parse_hhmm(str(at), &h, &mi)) return str_fmt(a, "time must look like 23:00 (got '%s')", at ? at : "");
        f64 when = next_at(h, mi);
        char text[6];
        clock_text(when, text);
        app->summary.snooze_ts = when;
        LOG("summary for %s: asking again at %s", day, text);
        phase_set(app, "asking again at %s", text);
        return str_fmt(a, "asking again at %s", text);
    }
    if (strcmp(action, "skip") == 0) {
        sqlite3* db = db_open(app);
        bool saved = db && save(db, day, "skipped", 0, {}, str("[]"));
        if (saved) lists_refresh_locked(app, db);
        if (db) sqlite3_close(db);
        if (!saved) return str_copy(a, str("could not save that"));
        LOG("summary for %s: skipped", day);
        phase_set(app, "skipped today");
        return str_copy(a, str("skipped"));
    }
    return str_copy(a, str("do must be now, later, until or skip"));
}

// Watches the clock: asks once it is time, starts the summary when the countdown ends, and shows
// reminders as they come due. Runs until the program ends.
static DWORD WINAPI summary_main(void* param) {
    App* app = (App*)param;
    for (;;) {
        sqlite3* db = db_open(app);
        if (db) alerts_show_due(app, db);
        if (!sleep_unless_stopping(app, 15000)) {
            if (db) sqlite3_close(db);
            break;
        }
        char day[11];
        day_of(now_ts(), day);
        app_lock(app);
        bool gaming = app->apps.game_running[0] != 0 || app->worker.gpu_busy;
        if (gaming) app->summary.last_game_ts = now_ts();
        // Counting down to a summary: a game starting puts it off; otherwise it starts on time.
        bool counting = app->summary.countdown_ts != 0;
        if (counting) {
            if (gaming) {
                app->summary.countdown_ts = 0;
                notify_remove(app, TOAST_TAG);
                LOG("a game started: the summary waits until it is over");
                phase_set(app, "waiting until you stop playing");
            } else if (now_ts() >= app->summary.countdown_ts) {
                char countdown_day[11];
                SET(countdown_day, str(app->summary.countdown_day));
                Mark m = mark(scratch());
                summary_act(app, scratch(), "now", countdown_day, 0);
                rewind(m);
            }
        }
        bool look = !counting && app->config.summary.enabled && !app->summary.writing &&
                    now_ts() >= ask_moment(day, app->config.summary.ask_at) && now_ts() >= app->summary.snooze_ts;
        SET(app->summary.ask_day, str(day));
        app->summary.ask_ts = ask_moment(day, app->config.summary.ask_at);
        f64 last_game = app->summary.last_game_ts;
        app_unlock(app);

        // done, skipped, or the database is unavailable
        if (look && db && status_of(db, day) == 0) {
            if (last_game && now_ts() - last_game < AFTER_GAME_SECS) {
                // Never during a game: wait until it has been closed for a while.
                phase_set_unlocked(app, "waiting until you stop playing");
            } else {
                // Too little to sum up: look again in an hour.
                app_lock(app);
                app->summary.snooze_ts = now_ts() + 3600;
                app_unlock(app);
                f64 mine = 0, apps = 0;
                if (talk_secs(db, day, &mine, &apps) && mine + apps >= 120.0) ask(app, db, day);
                else phase_set_unlocked(app, "too little recorded today to sum up");
            }
        }
        if (db) sqlite3_close(db);
    }
    return 0;
}

void summary_start(App* app) {
    app->summary.notify.lock = SRWLOCK_INIT;
    sqlite3* db = app->storage.ready ? db_open(app) : 0;
    if (db) {
        lists_refresh_locked(app, db);
        sqlite3_close(db);
    }
    if (app->config.summary.enabled) phase_set(app, "asks at %s", app->config.summary.ask_at);
    start_thread(app, "notify", notify_main);
    start_thread(app, "summary", summary_main);
}

// ---------------------------------------------------------------------------------------------
// check-ai

bool summary_check_ai(App* app, char* said, i32 cap) {
    app_lock(app);
    Config cfg = app->config;
    app->worker.summary_needs_gpu = true;
    app_unlock(app);
    f64 started = now_ts();
    Llm llm = {};
    char error[512] = {};
    bool ok = llm_open(app, &cfg, 4096, &llm, error, sizeof(error));
    f64 loaded = now_ts() - started;
    Arena a = {};
    Json reply = {};
    if (ok) {
        ok = llm_json(&llm, &a, str("Answer briefly."), str("Say hello in five words. Return JSON."),
                      str("{\"type\":\"object\",\"properties\":{\"reply\":{\"type\":\"string\"}},\"required\":[\"reply\"]}"), &reply,
                      error, sizeof(error));
    }
    llm_close(&llm);
    app_lock(app);
    app->worker.summary_needs_gpu = false;
    app_unlock(app);
    if (ok) {
        Str text = json_string_of(&a, &reply);
        snprintf(said, cap, "loaded %s in %.0fs\n%s\nstopped after %.0fs", cfg.summary.model, loaded, text.ptr, now_ts() - started);
    } else {
        snprintf(said, cap, "%s", error);
    }
    arena_release(&a);
    return ok;
}
