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
// The window's changes run on the database thread (db_post). The writer and the clock open
// their own connection (db_open) for as long as they need it, so no two threads share one.

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


// Splits on \n, drops a \r before it, no last empty line.
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
    Str name = str_lower(scratch(), strip_exe(speaker));
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

// A bold label line ("**Evening**") that groups the points under it.
static bool label_text(Str t) { return t.len > 4 && str_starts(t, str("**")) && str_ends(t, str("**")); }

// Removes bold labels left with no bullets under them.
static Str drop_empty_labels(Arena* a, Str text) {
    Buf out = {};
    Str rest = text, line = {};
    bool first = true;
    while (next_line(&rest, &line)) {
        Str t = str_trim(line);
        if (label_text(t)) {
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

// WEEKDAYS, MONTHS, parse_day and hm_of are in base.cpp.


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


// The next time the clock shows h:m: today, or tomorrow if that has passed.
static f64 next_at(i32 h, i32 m) {
    f64 now = now_ts();
    LocalTime t = local_time(now);
    f64 today = local_ts(t.year, t.month, t.day, h, m);
    return today > now ? today : local_ts(t.year, t.month, t.day + 1, h, m);
}

// Calendar days from today's local date to `ts`'s (midnight to midnight; ui_common's days_ago
// counts journal days, 04:00 to 04:00).
static i32 calendar_days_from_today(f64 ts) {
    LocalTime a = local_time(now_ts()), b = local_time(ts);
    f64 diff = local_ts(b.year, b.month, b.day, 12, 0) - local_ts(a.year, a.month, a.day, 12, 0);
    return (i32)floor(diff / 86400.0 + 0.5);
}

// "today at 17:00", "tomorrow at 09:00", "on Tue 17 Nov at 09:00".
static Str when_text(Arena* a, f64 ts) {
    LocalTime t = local_time(ts);
    i32 days = calendar_days_from_today(ts);
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

// When to ask about `day`: ask_at on that day (21:00 if it is not a time).
static f64 ask_moment(const char* day, const char* ask_at) {
    i32 h, mi;
    return said_ts(day, str(parse_hhmm(str(ask_at), &h, &mi) ? ask_at : "21:00"));
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


// ---------------------------------------------------------------------------------------------
// The window's state

static void phase_set(App* app, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->summary.phase, sizeof(app->summary.phase), fmt, args);
    va_end(args);
    wake_window(app);
}

struct SummaryPhase {
    char text[sizeof(Summary::phase)];
};

static void phase_apply(App* app, Event* e) {
    SummaryPhase p = latest_take<SummaryPhase>(e);
    SET(app->summary.phase, str(p.text));
    wake_window(app);
}

// From the summary threads: posted to the main thread (events.h).
static void phase_post(App* app, const char* fmt, ...) {
    SummaryPhase p;
    va_list args;
    va_start(args, fmt);
    vsnprintf(p.text, sizeof(p.text), fmt, args);
    va_end(args);
    latest_send(app, &app->summary.phase_next, phase_apply, p);
}

static void version_apply(App* app, Event*) {
    app->summary.version++;
    wake_window(app);
}


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
    sqlite3_stmt* st = prepare(db, "SELECT day, status, (SELECT COUNT(*) FROM summary_items i WHERE i.day = s.day AND "
                               "i.kind = 'reminder' AND i.dismissed = 0) FROM summaries s ORDER BY day DESC LIMIT 400");
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        SummaryDay d = {};
        SET(d.day, str((const char*)sqlite3_column_text(st, 0)));
        SET(d.status, str((const char*)sqlite3_column_text(st, 1)));
        d.reminders = sqlite3_column_int(st, 2);
        list_push(days, d);
    }
    sqlite3_finalize(st);
    st = prepare(db, "SELECT id, topic FROM summary_never ORDER BY id");
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        NeverTopic t = {};
        t.id = sqlite3_column_int64(st, 0);
        set(t.topic, sizeof(t.topic), str((const char*)sqlite3_column_text(st, 1)));
        list_push(never, t);
    }
    sqlite3_finalize(st);
}

struct ListsRead {
    List<SummaryDay> days;
    List<NeverTopic> never;
};

// At start: the lists are the main thread's now.
static void lists_apply(App* app, Job* job) {
    ListsRead* r = (ListsRead*)job->data;
    Summary* s = &app->summary;
    list_free(&s->days);
    list_free(&s->never);
    s->days = r->days;
    s->never = r->never;
    s->lists_ready = true;
    s->version++;
    wake_window(app);
}

// At start, on the database thread: summaries saved before summary_items get their rows, made
// from the markdown and reminders they were saved with (the old columns stay as they are).
static void items_migrate_run(sqlite3* db, Job* job);

// At start, on the database thread.
static void lists_run(sqlite3* db, Job* job) {
    ListsRead* r = (ListsRead*)job->data;
    if (db) lists_read(db, &r->days, &r->never);
}

// The main thread: a day's row as a change saved it, put in Summary.days (newest first).
static void day_put(App* app, const SummaryDay* row) {
    List<SummaryDay>* days = &app->summary.days;
    for (i32 i = 0; i < days->count; i++) {
        if (strcmp(days->items[i].day, row->day) == 0) {
            days->items[i] = *row;
            return;
        }
    }
    list_push(days, *row);
    for (i32 i = days->count - 1; i > 0 && strcmp(days->items[i - 1].day, days->items[i].day) < 0; i--) {
        SummaryDay t = days->items[i - 1];
        days->items[i - 1] = days->items[i];
        days->items[i] = t;
    }
}

const SummaryDay* summary_day_find(App* app, const char* day) {
    for (i32 i = 0; i < app->summary.days.count; i++)
        if (strcmp(app->summary.days.items[i].day, day) == 0) return &app->summary.days.items[i];
    return 0;
}

static SummaryDay day_row(const char* day, const char* status, i32 reminders) {
    SummaryDay d = {};
    SET(d.day, str(day));
    SET(d.status, str(status));
    d.reminders = reminders;
    return d;
}

// Any thread, after a reminder is set to show at `due`: the clock wakes for it then.
static void alert_due_at(App* app, f64 due) {
    atomic_min64(&app->summary.next_alert_ms, (LONG64)(due * 1000.0));
    if (app->summary.wake) SetEvent(app->summary.wake);
}

// ---------------------------------------------------------------------------------------------
// Storage

static bool save(sqlite3* db, const char* day, const char* status, f64 ts, const char* model, Str summary, Str reminders) {
    sqlite3_stmt* st = prepare(db, "INSERT OR REPLACE INTO summaries (day, status, created_ts, model, summary, reminders) "
                               "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    if (!st) return false;
    bind_str(st, 1, str(day));
    bind_str(st, 2, str(status));
    sqlite3_bind_double(st, 3, ts);
    if (model) bind_str(st, 4, str(model));
    if (summary.ptr) bind_str(st, 5, summary);
    bind_str(st, 6, reminders);
    return sql_done(db, st) >= 0;
}

// A day's saved row (its lines are in summary_items).
struct SavedRow {
    bool found;
    Str status;
    f64 created_ts;
    Str model;
};

static SavedRow saved_row(sqlite3* db, Arena* a, const char* day) {
    SavedRow row = {};
    sqlite3_stmt* st = prepare(db, "SELECT status, created_ts, model FROM summaries WHERE day = ?1");
    if (!st) return row;
    bind_str(st, 1, str(day));
    if (sqlite3_step(st) == SQLITE_ROW) {
        row.found = true;
        row.status = col_str(st, 0, a);
        row.created_ts = sqlite3_column_double(st, 1);
        row.model = col_str(st, 2, a);
    }
    sqlite3_finalize(st);
    return row;
}

// ---------------------------------------------------------------------------------------------
// Items: each summary's lines and reminders as rows (summary_items), so the journal's buttons
// act on an id and a moment rather than on the markdown's text.

static const char* ITEM_KINDS[] = {"heading", "point", "text", "reminder"};

static ItemKind item_kind(Str s) {
    for (i32 i = 0; i < COUNT(ITEM_KINDS); i++)
        if (str_eq(s, str(ITEM_KINDS[i]))) return (ItemKind)i;
    return ITEM_TEXT;
}

// The "HH:MM" a point ends with: "Call Sam back (14:05)". Empty if none.
static Str point_time(Str text) {
    text = str_trim(text);
    if (text.len < 7 || text.ptr[text.len - 1] != ')' || text.ptr[text.len - 7] != '(') return {};
    Str t = str_sub(text, text.len - 6, 5);
    i32 h, m;
    return parse_hhmm(t, &h, &m) ? t : Str{};
}

static bool tx(sqlite3* db, const char* what) {
    char* err = 0;
    if (sqlite3_exec(db, what, 0, 0, &err) == SQLITE_OK) return true;
    ERR("summary %s: %s", what, err ? err : "failed");
    sqlite3_free(err);
    return false;
}

static bool item_insert(sqlite3* db, const char* day, i32 pos, ItemKind kind, Str text, f64 ts, Str said, Str said_time, f64 due) {
    sqlite3_stmt* st = prepare(db, "INSERT INTO summary_items (day, pos, kind, text, ts, said, said_time, due_ts) "
                               "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    if (!st) return false;
    bind_str(st, 1, str(day));
    sqlite3_bind_int(st, 2, pos);
    bind_str(st, 3, str(ITEM_KINDS[kind]));
    bind_str(st, 4, text);
    if (ts) sqlite3_bind_double(st, 5, ts);
    if (said.len) bind_str(st, 6, said);
    if (said_time.len) bind_str(st, 7, said_time);
    if (due) sqlite3_bind_double(st, 8, due);
    return sql_done(db, st) >= 0;
}

// `day`'s summary as items: a row per line of `markdown`, then one per reminder. Replaces the
// day's earlier rows and points its alerts at the new ones. In the caller's transaction.
static bool items_replace(sqlite3* db, const char* day, Str markdown, const Json* reminders) {
    sqlite3_stmt* st = prepare(db, "DELETE FROM summary_items WHERE day = ?1");
    if (st) bind_str(st, 1, str(day));
    if (sql_done(db, st) < 0) return false;
    i32 pos = 0;
    Str rest = markdown, line = {};
    while (next_line(&rest, &line)) {
        Str t = str_trim(line), b = {};
        if (!t.len) continue;
        bool ok;
        if (str_starts(t, str("## ")) || str_starts(t, str("# "))) {
            ok = item_insert(db, day, pos++, ITEM_HEADING, str_trim(str_sub(t, str_starts(t, str("## ")) ? 3 : 2)), 0, {}, {}, 0);
        } else if (bullet_text(t, &b)) {
            Str hm = point_time(b);
            ok = item_insert(db, day, pos++, ITEM_POINT, b, hm.len ? said_ts(day, hm) : 0, {}, {}, 0);
        } else {
            ok = item_insert(db, day, pos++, ITEM_TEXT, t, 0, {}, {}, 0);
        }
        if (!ok) return false;
    }
    for (i32 i = 0; i < json_len(reminders); i++) {
        const Json* r = json_at(reminders, i);
        Str text = str_trim(json_text(json_get(r, "text")));
        if (!text.len) continue;
        Str time = str_trim(json_text(json_get(r, "time")));
        i32 h, mi;
        f64 ts = parse_hhmm(time, &h, &mi) ? said_ts(day, time) : 0;
        if (!item_insert(db, day, pos++, ITEM_REMINDER, text, ts, str_trim(json_text(json_get(r, "said"))), time, due_of(day, r))) return false;
    }
    st = prepare(db, "UPDATE alerts SET item = (SELECT i.id FROM summary_items i WHERE i.day = alerts.day AND "
                 "i.kind = 'reminder' AND i.text = alerts.text) WHERE day = ?1");
    if (st) bind_str(st, 1, str(day));
    return sql_done(db, st) >= 0;
}

static void items_migrate_run(sqlite3* db, Job* job) {
    if (!db) return;
    Arena* a = &job->arena;
    struct Old {
        char day[11];
        Str summary;
        Json reminders;
    };
    List<Old> old = {};
    sqlite3_stmt* st = prepare(db, "SELECT day, summary, reminders FROM summaries s WHERE status = 'done' AND "
                               "NOT EXISTS (SELECT 1 FROM summary_items i WHERE i.day = s.day)");
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        Old o = {};
        SET(o.day, str((const char*)sqlite3_column_text(st, 0)));
        o.summary = col_str(st, 1, a);
        if (!json_parse(a, col_str(st, 2, a), &o.reminders, 0, 0) || o.reminders.type != JSON_ARRAY) o.reminders = {};
        list_push(&old, o);
    }
    sqlite3_finalize(st);
    i32 made = 0;
    for (i32 i = 0; i < old.count; i++) {
        Old* o = &old.items[i];
        if (tx(db, "BEGIN IMMEDIATE") && items_replace(db, o->day, o->summary, &o->reminders) && tx(db, "COMMIT")) {
            made++;
        } else {
            ERR("summary items for %s could not be made", o->day);
            tx(db, "ROLLBACK");
        }
    }
    if (made) LOG("summary items made for %d saved summaries", made);
    list_free(&old);
}

struct ItemRow {
    i64 id;
    ItemKind kind;
    Str text;
    f64 ts;
    Str said, said_time;  // a reminder's
    f64 due;
};

// A line that is only bold text: a label ("**To remember**") over the points after it.
static bool is_label(const ItemRow* r) { return r->kind == ITEM_TEXT && label_text(r->text); }

// Whether a heading or label at `i` has a line under it still shown.
static bool has_lines_under(Slice<ItemRow> rows, i32 i) {
    bool heading = rows.items[i].kind == ITEM_HEADING;
    for (i32 j = i + 1; j < rows.count; j++) {
        const ItemRow* r = &rows.items[j];
        if (r->kind == ITEM_HEADING || r->kind == ITEM_REMINDER || (!heading && is_label(r))) return false;
        if (!is_label(r)) return true;
    }
    return false;
}

// `day`'s items not dismissed, in order; headings and labels left with nothing under them are
// left out.
static Slice<ItemRow> items_read(sqlite3* db, Arena* a, const char* day) {
    List<ItemRow> all = {};
    sqlite3_stmt* st = prepare(db, "SELECT id, kind, text, COALESCE(ts, 0), said, said_time, COALESCE(due_ts, 0) FROM summary_items "
                               "WHERE day = ?1 AND dismissed = 0 ORDER BY pos");
    if (st) bind_str(st, 1, str(day));
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        ItemRow r = {};
        r.id = sqlite3_column_int64(st, 0);
        const char* kind = (const char*)sqlite3_column_text(st, 1);
        r.kind = item_kind(str(kind ? kind : ""));
        r.text = col_str(st, 2, a);
        r.ts = sqlite3_column_double(st, 3);
        r.said = col_str(st, 4, a);
        r.said_time = col_str(st, 5, a);
        r.due = sqlite3_column_double(st, 6);
        list_push(&all, r);
    }
    sqlite3_finalize(st);
    Slice<ItemRow> rows = {all.items, all.count};
    Slice<ItemRow> out = {PUSH_ARRAY(a, ItemRow, all.count), 0};
    for (i32 i = 0; i < rows.count; i++) {
        bool over = rows.items[i].kind == ITEM_HEADING || is_label(&rows.items[i]);
        if (!over || has_lines_under(rows, i)) out.items[out.count++] = rows.items[i];
    }
    list_free(&all);
    return out;
}

// The summary's markdown from its items (summaries.summary; the .md file).
static Str items_summary(Arena* a, Slice<ItemRow> rows) {
    Buf b = {};
    bool first = true;
    for (i32 i = 0; i < rows.count; i++) {
        const ItemRow* r = &rows.items[i];
        if (r->kind == ITEM_REMINDER) continue;
        if (!first) buf_cstr(&b, (r->kind == ITEM_HEADING || is_label(r)) ? "\n\n" : "\n");
        if (r->kind == ITEM_HEADING) buf_cstr(&b, "## ");
        if (r->kind == ITEM_POINT) buf_cstr(&b, "- ");
        buf_add(&b, r->text);
        first = false;
    }
    return buf_take(&b, a);
}

StoredSummary summary_get(sqlite3* db, Arena* a, const char* day) {
    StoredSummary out = {};
    if (!db) return out;
    SavedRow row = saved_row(db, a, day);
    if (!row.found) return out;
    out.found = true;
    out.status = row.status;
    out.created_ts = row.created_ts;
    out.model = row.model;
    Slice<ItemRow> rows = items_read(db, a, day);
    out.items = PUSH_ARRAY(a, SummaryItem, rows.count);
    out.reminders = PUSH_ARRAY(a, Reminder, rows.count);
    for (i32 i = 0; i < rows.count; i++) {
        const ItemRow* it = &rows.items[i];
        if (it->kind != ITEM_REMINDER) {
            out.items[out.item_count++] = {it->id, it->kind, it->text, it->ts};
            continue;
        }
        Reminder* r = &out.reminders[out.reminder_count++];
        r->id = it->id;
        r->ts = it->ts;
        r->time = it->said_time;
        r->text = it->text;
        r->said = it->said;
        // Its notification, if any; else when it would be due, if it said.
        f64 due = 0;
        sqlite3_stmt* st = prepare(db, "SELECT due_ts, state FROM alerts WHERE item = ?1");
        if (st) sqlite3_bind_int64(st, 1, it->id);
        if (st && sqlite3_step(st) == SQLITE_ROW) {
            due = sqlite3_column_double(st, 0);
            const char* state = (const char*)sqlite3_column_text(st, 1);
            r->alert_set = state && strcmp(state, "set") == 0;
        } else {
            due = it->due;
        }
        sqlite3_finalize(st);
        if (due) {
            r->alert_due = due;
            r->alert_when = when_text(a, due);
        }
    }
    return out;
}

// Also kept as summaries\YYYY-MM-DD.md next to the database, readable without the app.
static bool write_markdown(App* app, const char* day, Slice<ItemRow> rows) {
    Mark m = mark(scratch());
    Buf text = {};
    buf_fmt(&text, "# %s\n\n", day_long(scratch(), day, true).ptr);
    buf_add(&text, items_summary(scratch(), rows));
    buf_cstr(&text, "\n");
    bool any = false;
    for (i32 i = 0; i < rows.count; i++) {
        const ItemRow* r = &rows.items[i];
        if (r->kind != ITEM_REMINDER) continue;
        if (!any) buf_cstr(&text, "\n## Reminders\n\n");
        any = true;
        buf_fmt(&text, "- [ ] %.*s (%.*s)\n", (int)r->text.len, r->text.ptr, (int)r->said_time.len, r->said_time.ptr);
    }
    Str data = buf_take(&text, scratch());
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\summaries", wide(scratch(), str(data_dir())));
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
    Str name;     // the speaker as shown (the app's own name), from name_lines
    Str title;
    Str text;
};

static bool is_mic(const SLine* l) { return str_eq(l->source, str("mic")); }

// Each line's speaker as the app list shows it, as app_label does.
static void name_lines(AppLabels* labels, Arena* a, Slice<SLine> lines) {
    for (i32 i = 0; i < lines.count; i++) lines.items[i].name = str_copy(a, label_of(labels, lines.items[i].speaker));
}

static Slice<SLine> day_lines(sqlite3* db, Arena* a, f64 from, f64 to) {
    Slice<SLine> out = {};
    sqlite3_stmt* st = prepare(db, "SELECT start_ts, end_ts, source, speaker, title, text FROM segments "
                               "WHERE end_ts >= ?1 AND start_ts <= ?2 ORDER BY start_ts, id LIMIT 20000");
    if (!st) return out;
    sqlite3_bind_double(st, 1, from);
    sqlite3_bind_double(st, 2, to);
    List<SLine> list = {};
    while (sqlite3_step(st) == SQLITE_ROW) {
        SLine l = {};
        l.start_ts = sqlite3_column_double(st, 0);
        l.end_ts = sqlite3_column_double(st, 1);
        l.source = col_str(st, 2, a);
        l.speaker = col_str(st, 3, a);
        l.title = col_str(st, 4, a);
        l.text = col_str(st, 5, a);
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
    sqlite3_stmt* st = prepare(db, "SELECT COALESCE(SUM(CASE WHEN source = 'mic' THEN end_ts - start_ts END), 0), "
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

// The parts of the day a summary is grouped under. A day runs from 04:00 (day_bounds), so Night is
// the hours after midnight.
static const char* PARTS[] = {"Morning", "Afternoon", "Evening", "Night"};
static const i32 PART_COUNT = COUNT(PARTS);

static i32 part_of(f64 ts) {
    i32 h = local_time(ts).hour;
    return h < 4 ? 3 : h < 12 ? 0 : h < 18 ? 1 : 2;
}

struct Chunk {
    f64 first;
    f64 last;
    i32 part;  // PARTS: a chunk never spans two
    Str text;
    Str who;  // who can be named in its notes, as JSON enum items: "You" and the people in a call
};

struct RunCount {
    Str who;
    i64 chars;
};

// Ends the chunk in `text`: its people as JSON strings, after "You".
static void chunk_end(Arena* a, List<Chunk>* chunks, Buf* text, f64 first, f64 last, List<Str>* people) {
    Buf who = {};
    buf_cstr(&who, "\"You\"");
    for (i32 i = 0; i < people->count; i++) {
        buf_cstr(&who, ",");
        json_quote(&who, people->items[i]);
    }
    list_push(chunks, {first, last, part_of(first), buf_take(text, a), buf_take(&who, a)});
    people->count = 0;
}

// The day's lines as compact text, cut into pieces that fit one request and never span two parts
// of the day. Long stretches of one app (a film, an album) are shortened; what I said is always
// kept in full.
static Slice<Chunk> transcript_chunks(Arena* a, Slice<SLine> all) {
    Slice<SLine> lines = context_only(a, all);
    struct OutLine {
        f64 start, end;
        Str text;
        Str person;  // the speaker of a call line
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
        hm_of(line->start_ts, at);
        if (is_mic(line)) {
            since_me.count = 0;
            list_push(&out_lines, {line->start_ts, line->end_ts, str_fmt(a, "[%s] You: %.*s", at, (int)text.len, text.ptr), {}});
            continue;
        }
        // "call" lines come one person per stream (lifelog-vencord) and are named after the speaker.
        bool call = str_eq(line->source, str("call")) || is_call_app(line->speaker);
        // Media is marked as such, so it is never read as someone talking.
        Str name = line->name;
        Str who = call ? str_copy(a, name) : str_fmt(a, "media in %.*s", (int)name.len, name.ptr);
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
                               str_fmt(a, "[%s] %.*s: %.*s", at, (int)label.len, label.ptr, (int)text.len, text.ptr), call ? who : Str{}});
    }
    list_free(&since_me);

    List<Chunk> chunks = {};
    List<Str> people = {};
    Buf text = {};
    f64 first = 0, last = 0;
    for (i32 i = 0; i < out_lines.count; i++) {
        OutLine* l = &out_lines.items[i];
        if (buf_len(&text) && (buf_len(&text) + l->text.len > CHUNK_CHARS || part_of(l->start) != part_of(first)))
            chunk_end(a, &chunks, &text, first, last, &people);
        if (!buf_len(&text)) first = l->start;
        last = l->end;
        buf_add(&text, l->text);
        buf_cstr(&text, "\n");
        bool known = !l->person.len;
        for (i32 k = 0; k < people.count && !known; k++) known = str_eq(people.items[k], l->person);
        if (!known) list_push(&people, l->person);
    }
    if (buf_len(&text)) chunk_end(a, &chunks, &text, first, last, &people);
    list_free(&people);
    list_free(&out_lines);
    Slice<Chunk> out = {PUSH_ARRAY(a, Chunk, chunks.count), chunks.count};
    if (chunks.count) memcpy(out.items, chunks.items, sizeof(Chunk) * chunks.count);
    list_free(&chunks);
    return out;
}

// Words I said (the 'You' lines) in a transcript chunk.
static i64 my_words(Str chunk) {
    i64 n = 0;
    Str rest = chunk, line = {};
    while (next_line(&rest, &line)) {
        i64 at = str_find(line, str("] You: "));
        if (at >= 0) n += count_words(str_sub(line, at + 7));
    }
    return n;
}

// What a quiet day's summary says, without asking the model.
static const char* quiet_summary(i64 words) { return words == 0 ? "You were quiet all day." : "You were quiet most of the day."; }

// ---------------------------------------------------------------------------------------------
// The prompts. SYSTEM is only the default: summary-prompt.txt replaces it (summary_prompt_path).
// The rules and schemas below stay in code, as the parsing depends on them.

static const char* SYSTEM =
    "You help one person keep a diary from an automatic transcript of their day. Write to them as 'you'. "
    "Lines marked 'You' are the only words they said, into their microphone: to people in the room, on calls, or to "
    "themselves. Lines named after a person (like 'Sam' or 'Alex') are that person talking to them on a call: their "
    "words, their stories and their news, never the diary-keeper's. Lines marked 'media in <app>' are audio playing on "
    "their PC (videos, films, music, games, a voice assistant): never people they talked to, never their words and never "
    "something they did. The window title is in brackets. "
    "Who said it matters most. Before writing anything, check whose line it came from. When someone else tells a story, "
    "shares news or explains something, write it as theirs: 'Sam told you about her job interview', not 'You talked "
    "about a job interview'. Your short replies ('yeah', 'oh no', 'that's funny', questions) do not make it your story. "
    "Write 'you said', 'you told', 'you mentioned' only for things in 'You' lines. Feelings, opinions, worries and plans "
    "are theirs only when their own 'You' words say so; a video or song saying something is never them feeling it. "
    "The speech recognition makes mistakes: guess the likely word only when it is obvious, and never build a story on a "
    "garbled line. "
    "Be specific, never vague: name the people, games, projects, places, things and numbers, and say what was actually "
    "said, decided or found out, not just that something was 'discussed'. Be factual and brief and never invent details.";

static const char* REMEMBER_RULE =
    "remember = one item per flagged moment, in order: time (HH:MM of the flag) and "
    "point (what you wanted to remember, one short sentence from your own words around the flag; keep names, numbers "
    "and dates exact).";

static const char* REMINDERS_RULE =
    "reminders = only clear plans or promises you made out loud for later: "
    "\"I'll…\", \"I need to…\", \"I have to…\", \"I'm going to…\", \"remind me to…\", \"don't let me forget…\", \"we should…\", "
    "\"let's… tomorrow\". For each: time (HH:MM), said (your exact words from a 'You' line), text (a short to-do, "
    "like \"Call the dentist\"), when (\"YYYY-MM-DD HH:MM\" if you said when to do it: \"tomorrow\", \"tonight\", "
    "\"at 5\", \"on Monday\", \"in an hour\"; counted from the time you said it, using the date given; tomorrow with no "
    "time is 09:00, tonight is 20:00; \"\" if you did not say when). Comments, questions, feelings, game chatter and anything from videos or songs are NOT "
    "reminders. Most days have none or one or two; an empty list is right when nothing qualifies.";

#define REMINDERS_SCHEMA                                                                                          \
    "{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"time\":{\"type\":\"string\"},\"said\":" \
    "{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"when\":{\"type\":\"string\"}},\"required\":"          \
    "[\"time\",\"said\",\"text\",\"when\"]}}"
// A part of the transcript: notes, each named after whose lines it came from (%s: Chunk.who).
static const char* NOTES_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"notes\":{\"type\":\"array\",\"maxItems\":6,\"items\":{\"type\":\"object\","
    "\"properties\":{\"who\":{\"type\":\"string\",\"enum\":[%s]},\"note\":{\"type\":\"string\"}},\"required\":[\"who\",\"note\"]}},"
    "\"reminders\":" REMINDERS_SCHEMA "},\"required\":[\"notes\",\"reminders\"]}";
// A part of the day in the summary: up to %d points, each on its own topic.
static const char* POINTS_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"points\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":%d,\"items\":{\"type\":"
    "\"object\",\"properties\":{\"topic\":{\"type\":\"string\",\"maxLength\":40},\"text\":{\"type\":\"string\",\"maxLength\":300}},"
    "\"required\":[\"topic\",\"text\"]}}},\"required\":[\"points\"]}";
static const char* REMEMBER_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"remember\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":"
    "{\"time\":{\"type\":\"string\"},\"point\":{\"type\":\"string\"}},\"required\":[\"time\",\"point\"]}}},\"required\":[\"remember\"]}";

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

// Step 1, for each chunk: notes on it, each saying whose lines it came from, so another person's
// story is never written as mine.
static Str notes_prompt(Arena* a, const char* day, const Chunk* c, Str never) {
    char t0[6], t1[6];
    hm_of(c->first, t0);
    hm_of(c->last, t1);
    Buf b = {};
    buf_fmt(&b, "Transcript from %s to %s on %s (times after midnight are the next morning):\n\n", t0, t1, day_long(a, day, true).ptr);
    buf_add(&b, c->text);
    buf_cstr(&b,
             "\n\nReturn JSON. notes = up to 6 notes on this part of the day, one per topic (never two notes on the same "
             "thing); a short or quiet part needs only one or two. For each: who = whose lines the topic came from: 'You' "
             "only when the 'You' lines tell it (your story, your plan, your work), or the name of the person whose lines "
             "tell it, even when you replied ('yeah', questions, jokes); note = one sentence with its concrete details "
             "(names, things, numbers, what was decided), written as 'You …' or 'Sam told you …' to match who. Keep what "
             "matters for a diary: what you did, made, planned, promised or decided, what you told people, and the main "
             "news or stories people told you. Leave out small talk, bare reactions and media you only watched or "
             "listened to; the plot of a film or video is never a note (at most: \"Watched Avatar with friends on "
             "Discord\").");
    buf_add(&b, never);
    buf_cstr(&b, " ");
    buf_cstr(&b, REMINDERS_RULE);
    return buf_take(&b, a);
}

// Step 2, for each part of the day: its notes (`notes`, under each chunk's times) cut down to at
// most `points` bullets.
static Str points_prompt(Arena* a, const char* day, i32 part, f64 first, f64 last, Str notes, i32 points, Str never) {
    char t0[6], t1[6];
    hm_of(first, t0);
    hm_of(last, t1);
    Buf b = {};
    Str name = str_lower(a, str(PARTS[part]));
    buf_fmt(&b, "Notes on the %s of %s (%s–%s), in order:\n", name.ptr, day_long(a, day, true).ptr, t0, t1);
    buf_add(&b, notes);
    buf_fmt(&b,
            "\nReturn JSON. points = the diary for this part of the day: 1 to %d bullets, each with topic (2 to 4 words "
            "naming the story or subject, like 'dog at the vet' or 'new laptop') and text (one plain sentence of at "
            "most 30 words with its concrete details: names, things, numbers). Every bullet has a different topic: notes "
            "about the same story, even from different times, become one bullet. Cover the whole span, from the first "
            "time range to the last, not just its start. Most important first: plans, promises and decisions, then what "
            "you did, made or told people, then the main things people told you. Drop small talk and bare reactions; "
            "fewer bullets rather than padding. Each note starts with [who] it came from; keep that exactly: 'you …' only "
            "for your own words, 'Sam told you …' for what someone else said. No list of songs or videos.",
            points);
    buf_add(&b, never);
    return buf_take(&b, a);
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
            Str who = is_mic(l) ? str("You") : l->name;
            Str text = str_trim(l->text);
            char at[6];
            hm_of(l->start_ts, at);
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
        hm_of(flags.items[i].ts, at);
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
        hm_of(flags.items[i].ts, at);
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
        Mark m = mark(scratch());
        Str said = str_trim(json_text(json_get(r, "said")));
        // Already asked, set or turned down (an earlier run of this day's summary): it is left
        // as it is (UNIQUE (day, text)).
        sqlite3_stmt* st = prepare(db, "INSERT OR IGNORE INTO alerts (day, text, said, said_time, due_ts, state, item) VALUES (?1, ?2, ?3, ?4, ?5, 'asked', "
                                   "(SELECT id FROM summary_items WHERE day = ?1 AND kind = 'reminder' AND text = ?2))");
        if (!st) return;
        bind_str(st, 1, str(day));
        bind_str(st, 2, text);
        bind_str(st, 3, said);
        bind_str(st, 4, json_text(json_get(r, "time")));
        sqlite3_bind_double(st, 5, due);
        i32 added = sql_done(db, st);
        if (added < 0) return;
        if (added == 0) {
            rewind(m);
            continue;
        }
        i64 id = sqlite3_last_insert_rowid(db);

        ToastJob t = {};
        snprintf(t.tag, sizeof(t.tag), "remind-%lld", id);
        set(t.title, sizeof(t.title), str_fmt(scratch(), "Remind you %s?", when_text(scratch(), due).ptr));
        Str shown = strip_md(scratch(), text);
        set(t.body, sizeof(t.body), str_fmt(scratch(), "%.*s. You said: “%.*s”", (int)shown.len, shown.ptr, (int)said.len, said.ptr));
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

// Shows the reminders that have come due. On the summary thread, when Summary.next_alert_ms
// has come.
static void alerts_show_due(App* app, sqlite3* db) {
    sqlite3_stmt* st = prepare(db, "SELECT id, day, text, said, said_time FROM alerts WHERE state = 'set' AND due_ts <= ?1");
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
        d.text = col_str(st, 2, scratch());
        d.said = col_str(st, 3, scratch());
        d.time = col_str(st, 4, scratch());
        list_push(&due, d);
    }
    sqlite3_finalize(st);
    for (i32 i = 0; i < due.count; i++) {
        Due* d = &due.items[i];
        sqlite3_stmt* up = prepare(db, "UPDATE alerts SET state = 'shown' WHERE id = ?1");
        if (!up) break;
        sqlite3_bind_int64(up, 1, d->id);
        if (sql_done(db, up) < 0) break;
        i32 y, mo, dd;
        Str when = parse_day(d->day, &y, &mo, &dd) ? str(WEEKDAYS[day_plus(y, mo, dd, 0).weekday]) : str(d->day);
        ToastJob t = {};
        snprintf(t.tag, sizeof(t.tag), "remind-%lld", d->id);
        set(t.title, sizeof(t.title), strip_md(scratch(), d->text));
        if (!d->said.len) set(t.body, sizeof(t.body), str_fmt(scratch(), "From %s", when.ptr));
        else
            set(t.body, sizeof(t.body),
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
    if (any) event_send(app, version_apply);
}

// The clock, when a reminder is due: opens the database, shows what has come due and reads when
// the next one is. A reminder set meanwhile lowers next_alert_ms itself (alert_due_at), so it is
// cleared before the read, not after.
static void alerts_check(App* app) {
    InterlockedExchange64(&app->summary.next_alert_ms, INT64_MAX);
    sqlite3* db = db_open(app);
    if (!db) {
        alert_due_at(app, now_ts() + 60);  // look again in a minute
        return;
    }
    alerts_show_due(app, db);
    sqlite3_stmt* st = prepare(db, "SELECT MIN(due_ts) FROM alerts WHERE state = 'set'");
    if (!st) alert_due_at(app, now_ts() + 60);  // could not say: look again in a minute
    if (st && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        f64 next = sqlite3_column_double(st, 0);
        // One still due could not be marked shown: try again in a minute, not straight away.
        if (next <= now_ts()) next = now_ts() + 60;
        atomic_min64(&app->summary.next_alert_ms, (LONG64)(next * 1000.0));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

// ---------------------------------------------------------------------------------------------
// The window's changes: the database thread makes them (db_post), the main thread shows what
// came of them.

enum EditOutcome : u8 {
    EDIT_DONE,
    EDIT_NO_DB,         // the database could not be opened
    EDIT_FAILED,        // a statement failed (it is in the log)
    EDIT_NO_REMINDER,
    EDIT_NO_ITEM,       // that line is not in the summary (any more)
    EDIT_BAD_ACTION,
};

enum EditKind : u8 {
    EDIT_ANSWER,        // a reminder notification's button
    EDIT_REMIND,        // the journal's bell
    EDIT_DISMISS,
    EDIT_NEVER_ADD,
    EDIT_NEVER_REMOVE,
    EDIT_SKIP,
};

struct SummaryEdit {
    App* app;
    EditKind kind;
    char day[11];
    i64 id;           // EDIT_ANSWER: the alert; EDIT_NEVER_REMOVE: the topic; else the item (summary_items), 0 for none
    bool on;          // EDIT_REMIND: turn it on
    char action[16];  // EDIT_ANSWER
    Str text;         // EDIT_NEVER_ADD: the topic
    // What came of it, for the main thread (edit_apply).
    EditOutcome outcome;
    bool changed;     // the stored summaries changed: Summary.version
    bool has_row;     // `row` is the day as now saved (a dismiss, a skip)
    SummaryDay row;
    NeverTopic topic; // EDIT_NEVER_ADD: the topic as saved, with its id
    f64 due;          // EDIT_REMIND, turned on: when it shows
};

static void edit_answer(sqlite3* db, SummaryEdit* e) {
    const char* text = 0;
    if (strcmp(e->action, "yes") == 0) text = "UPDATE alerts SET state = 'set' WHERE id = ?1";
    else if (strcmp(e->action, "no") == 0) text = "UPDATE alerts SET state = 'no' WHERE id = ?1";
    else if (strcmp(e->action, "done") == 0) text = "UPDATE alerts SET state = 'shown' WHERE id = ?1";
    else if (strcmp(e->action, "snooze") == 0) text = "UPDATE alerts SET state = 'set', due_ts = ?2 WHERE id = ?1";
    if (!text) {
        WARN("reminder answer must be yes, no, done or snooze, not \"%s\"", e->action);
        e->outcome = EDIT_BAD_ACTION;
        return;
    }
    sqlite3_stmt* st = prepare(db, text);
    if (st) {
        sqlite3_bind_int64(st, 1, e->id);
        if (strcmp(e->action, "snooze") == 0) sqlite3_bind_double(st, 2, now_ts() + 3600.0);
    }
    i32 changed = sql_done(db, st);
    if (changed < 0) e->outcome = EDIT_FAILED;
    if (changed == 0) {
        WARN("no reminder %lld", e->id);
        e->outcome = EDIT_NO_REMINDER;
    }
    if (changed > 0) {
        LOG("reminder %lld: %s", e->id, e->action);
        e->changed = true;
        if (strcmp(e->action, "yes") == 0 || strcmp(e->action, "snooze") == 0) {
            // It shows when due: the clock wakes then.
            sqlite3_stmt* q = prepare(db, "SELECT due_ts FROM alerts WHERE id = ?1");
            if (q) sqlite3_bind_int64(q, 1, e->id);
            if (q && sqlite3_step(q) == SQLITE_ROW) alert_due_at(e->app, sqlite3_column_double(q, 0));
            sqlite3_finalize(q);
        }
    }
}

// A reminder that is set: due at `due`, from summary item `item` (0 for a voice command's, which
// has none). The caller runs it with sql_done. Null if it could not be prepared.
static sqlite3_stmt* alert_insert(sqlite3* db, Str day, Str text, Str said, Str said_time, f64 due, i64 item) {
    sqlite3_stmt* st = prepare(db, "INSERT INTO alerts (day, text, said, said_time, due_ts, state, item) VALUES (?1, ?2, ?3, ?4, ?5, 'set', ?6)");
    if (!st) return 0;
    bind_str(st, 1, day);
    bind_str(st, 2, text);
    bind_str(st, 3, said);
    bind_str(st, 4, said_time);
    sqlite3_bind_double(st, 5, due);
    if (item) sqlite3_bind_int64(st, 6, item);
    else sqlite3_bind_null(st, 6);
    return st;
}

static void edit_remind(sqlite3* db, Arena* a, SummaryEdit* e) {
    const char* day = e->day;
    sqlite3_stmt* st = prepare(db, "SELECT text, said, said_time, COALESCE(due_ts, 0) FROM summary_items "
                               "WHERE id = ?1 AND day = ?2 AND kind = 'reminder' AND dismissed = 0");
    if (st) {
        sqlite3_bind_int64(st, 1, e->id);
        bind_str(st, 2, str(day));
    }
    bool found = st && sqlite3_step(st) == SQLITE_ROW;
    Str text = {}, said = {}, said_time = {};
    f64 due = 0;
    if (found) {
        text = col_str(st, 0, a);
        said = col_str(st, 1, a);
        said_time = col_str(st, 2, a);
        due = sqlite3_column_double(st, 3);
    }
    sqlite3_finalize(st);
    if (!found) {
        e->outcome = EDIT_NO_REMINDER;
        return;
    }
    // Its alert: by the item, or by the day and text for one made before items had ids.
    i64 alert = 0;
    st = prepare(db, "SELECT id FROM alerts WHERE item = ?1 OR (day = ?2 AND text = ?3) ORDER BY item IS NULL LIMIT 1");
    if (st) {
        sqlite3_bind_int64(st, 1, e->id);
        bind_str(st, 2, str(day));
        bind_str(st, 3, text);
        if (sqlite3_step(st) == SQLITE_ROW) alert = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    e->changed = true;
    if (!e->on) {
        if (!alert) return;
        st = prepare(db, "UPDATE alerts SET state = 'no' WHERE id = ?1");
        if (st) sqlite3_bind_int64(st, 1, alert);
        if (sql_done(db, st) < 0) e->outcome = EDIT_FAILED;
        return;
    }
    if (due <= now_ts()) due = next_morning();
    if (alert) {
        st = prepare(db, "UPDATE alerts SET state = 'set', due_ts = ?2, item = ?3 WHERE id = ?1");
        if (st) {
            sqlite3_bind_int64(st, 1, alert);
            sqlite3_bind_double(st, 2, due);
            sqlite3_bind_int64(st, 3, e->id);
        }
    } else {
        st = alert_insert(db, str(day), text, said, said_time, due, e->id);
    }
    if (sql_done(db, st) < 0) {
        e->outcome = EDIT_FAILED;
        return;
    }
    e->due = due;
    alert_due_at(e->app, due);
}

// Takes item `item` out of `day`'s summary: marks it dismissed (a reminder's notification goes
// off with it), then writes the summary's markdown and its .md file again from what is left.
static void edit_dismiss(App* app, sqlite3* db, Arena* a, SummaryEdit* e, i64 item) {
    const char* day = e->day;
    if (!tx(db, "BEGIN IMMEDIATE")) {
        e->outcome = EDIT_FAILED;
        return;
    }
    sqlite3_stmt* st = prepare(db, "UPDATE summary_items SET dismissed = 1 WHERE id = ?1 AND day = ?2 AND dismissed = 0");
    if (st) {
        sqlite3_bind_int64(st, 1, item);
        bind_str(st, 2, str(day));
    }
    i32 changed = sql_done(db, st);
    if (changed > 0) {
        st = prepare(db, "UPDATE alerts SET state = 'no' WHERE item = ?1 AND state IN ('asked', 'set')");
        if (st) sqlite3_bind_int64(st, 1, item);
        if (sql_done(db, st) < 0) changed = -1;
    }
    Slice<ItemRow> rows = {};
    if (changed > 0) {
        rows = items_read(db, a, day);
        st = prepare(db, "UPDATE summaries SET summary = ?1 WHERE day = ?2");
        if (st) {
            bind_str(st, 1, items_summary(a, rows));
            bind_str(st, 2, str(day));
        }
        if (sql_done(db, st) < 0) changed = -1;
    }
    if (changed <= 0 || !tx(db, "COMMIT")) {
        tx(db, "ROLLBACK");
        e->outcome = changed == 0 ? EDIT_NO_ITEM : EDIT_FAILED;
        return;
    }
    write_markdown(app, day, rows);
    SavedRow row = saved_row(db, a, day);
    i32 reminders = 0;
    for (i32 i = 0; i < rows.count; i++) reminders += rows.items[i].kind == ITEM_REMINDER;
    e->has_row = true;
    e->row = day_row(day, row.status.ptr, reminders);
    e->changed = true;
}

static void edit_run(sqlite3* db, Job* job) {
    SummaryEdit* e = (SummaryEdit*)job->data;
    if (!db) {
        e->outcome = EDIT_NO_DB;
        return;
    }
    Arena* a = &job->arena;
    switch (e->kind) {
    case EDIT_ANSWER: edit_answer(db, e); break;
    case EDIT_REMIND: edit_remind(db, a, e); break;
    case EDIT_DISMISS: edit_dismiss(e->app, db, a, e, e->id); break;
    case EDIT_NEVER_ADD: {
        sqlite3_stmt* st = prepare(db, "INSERT INTO summary_never (topic, created_ts) VALUES (?1, ?2)");
        if (st) {
            bind_str(st, 1, e->text);
            sqlite3_bind_double(st, 2, now_ts());
        }
        if (sql_done(db, st) < 0) {
            e->outcome = EDIT_FAILED;
            break;
        }
        e->topic.id = sqlite3_last_insert_rowid(db);
        set(e->topic.topic, sizeof(e->topic.topic), e->text);
        LOG("the summary will never mention: %.*s", (int)e->text.len, e->text.ptr);
        if (e->day[0] && e->id) edit_dismiss(e->app, db, a, e, e->id);
        break;
    }
    case EDIT_NEVER_REMOVE: {
        sqlite3_stmt* st = prepare(db, "DELETE FROM summary_never WHERE id = ?1");
        if (st) sqlite3_bind_int64(st, 1, e->id);
        if (sql_done(db, st) < 0) e->outcome = EDIT_FAILED;
        break;
    }
    case EDIT_SKIP: {
        f64 ts = now_ts();
        if (!save(db, e->day, "skipped", ts, 0, {}, str("[]"))) {
            e->outcome = EDIT_FAILED;
            break;
        }
        e->has_row = true;
        e->row = day_row(e->day, "skipped", 0);
        e->changed = true;
        break;
    }
    }
}

// The main thread: updates Summary's lists from what the edit reports, and says how it went.
static void edit_apply(App* app, Job* job) {
    SummaryEdit* e = (SummaryEdit*)job->data;
    Summary* s = &app->summary;
    bool done = e->outcome == EDIT_DONE;
    // Saved even when taking its line out then failed: the topic is in the database either way.
    bool never_added = e->kind == EDIT_NEVER_ADD && e->topic.id;
    if (never_added) list_push(&s->never, e->topic);
    if (done && e->kind == EDIT_NEVER_REMOVE) {
        for (i32 i = 0; i < s->never.count; i++)
            if (s->never.items[i].id == e->id) list_remove(&s->never, i--);
    }
    if (e->has_row) day_put(app, &e->row);
    if (e->changed || never_added || (done && e->kind == EDIT_NEVER_REMOVE)) s->version++;
    const char* say = 0;
    switch (e->outcome) {
    case EDIT_DONE:
        if (e->kind == EDIT_REMIND) {
            if (e->on) say = str_fmt(scratch(), "I'll remind you %s", when_text(scratch(), e->due).ptr).ptr;
            else say = "Reminder turned off";
        }
        break;
    case EDIT_NO_DB: say = "The database could not be opened"; break;
    case EDIT_FAILED: say = e->kind == EDIT_SKIP ? "Could not save the skip" : "Could not save that (see the log)"; break;
    case EDIT_NO_REMINDER: say = "No such reminder"; break;
    case EDIT_NO_ITEM: say = "That point is already gone"; break;
    case EDIT_BAD_ACTION: break;  // a link from an old notification: in the log
    }
    if (say) ui_say(app, "%s", say);
    wake_window(app);
}

static SummaryEdit* edit_new(App* app, Job** job, EditKind kind, const char* day) {
    *job = job_new(edit_apply);
    SummaryEdit* e = PUSH(&(*job)->arena, SummaryEdit);
    e->app = app;
    e->kind = kind;
    if (day) SET(e->day, str(day));
    (*job)->data = e;
    return e;
}

void reminder_answer(App* app, i64 id, const char* action) {
    char tag[32];
    snprintf(tag, sizeof(tag), "remind-%lld", id);
    notify_remove(app, tag);
    Job* job;
    SummaryEdit* e = edit_new(app, &job, EDIT_ANSWER, 0);
    e->id = id;
    SET(e->action, str(action));
    db_post(job, edit_run);
}

void reminder_set(App* app, const char* day, i64 item, bool on) {
    Job* job;
    SummaryEdit* e = edit_new(app, &job, EDIT_REMIND, day);
    e->id = item;
    e->on = on;
    db_post(job, edit_run);
}

void summary_dismiss(App* app, const char* day, i64 item) {
    Job* job;
    SummaryEdit* e = edit_new(app, &job, EDIT_DISMISS, day);
    e->id = item;
    db_post(job, edit_run);
}

void summary_add_never(App* app, Str topic, const char* day, i64 item) {
    topic = str_trim(topic);
    if (!topic.len) {
        ui_say(app, "Say what to leave out");
        return;
    }
    Job* job;
    SummaryEdit* e = edit_new(app, &job, EDIT_NEVER_ADD, day);
    e->text = str_copy(&job->arena, topic);
    e->id = day ? item : 0;
    db_post(job, edit_run);
}

void summary_remove_never(App* app, i64 id) {
    Job* job;
    SummaryEdit* e = edit_new(app, &job, EDIT_NEVER_REMOVE, 0);
    e->id = id;
    db_post(job, edit_run);
}

// ---------------------------------------------------------------------------------------------
// Settings

void summary_options_publish(App* app) {
    const Config* c = &app->config;
    Summary* s = &app->summary;
    AcquireSRWLockExclusive(&s->options_lock);
    SummaryOptions* o = &s->options;
    o->max_words = c->summary.max_words;
    o->ask_reminders = c->summary.ask_reminders;
    SET(o->model, str(c->summary.model));
    SET(o->endpoint, str(c->summary.endpoint));
    SET(o->api_key, str(c->summary.api_key));
    SET(o->ai_dir, str(c->ai.dir));
    ReleaseSRWLockExclusive(&s->options_lock);
    if (s->wake) SetEvent(s->wake);
}

SummaryOptions summary_options(App* app) {
    Summary* s = &app->summary;
    AcquireSRWLockShared(&s->options_lock);
    SummaryOptions o = s->options;
    ReleaseSRWLockShared(&s->options_lock);
    return o;
}

void summary_set_enabled(App* app, bool on) {
    app->config.summary.enabled = on;
    config_save_bool("summary", "enabled", on);
    summary_options_publish(app);
}

void summary_set_ask_at(App* app, Str hhmm) {
    i32 h, m;
    if (!parse_hhmm(hhmm, &h, &m)) {
        ui_say(app, "Time must look like 21:00 (got '%.*s')", (int)hhmm.len, hhmm.ptr);
        return;
    }
    snprintf(app->config.summary.ask_at, sizeof(app->config.summary.ask_at), "%02d:%02d", h, m);
    config_save_string("summary", "ask_at", app->config.summary.ask_at);
    summary_options_publish(app);
}

const wchar_t* summary_prompt_path() {
    static wchar_t path[MAX_PATH];
    if (!path[0]) {
        wchar_t dir[MAX_PATH];
        swprintf(dir, MAX_PATH, L"%s", config_path());
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
        swprintf(path, MAX_PATH, L"%s\\summary-prompt.txt", dir);
    }
    return path;
}

const wchar_t* summary_prompt_file(App* app) {
    const wchar_t* path = summary_prompt_path();
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && !write_file(path, str(SYSTEM))) {
        ui_say(app, "Could not write summary-prompt.txt");
        return 0;
    }
    return path;
}

// The system prompt: summary-prompt.txt if it has any text, else the built-in one. Read for each
// summary, so an edit counts from the next one.
static Str system_prompt(Arena* a) {
    Str text = read_file(a, summary_prompt_path());
    if (text.ptr) {
        if (str_starts(text, str("\xEF\xBB\xBF"))) text = {text.ptr + 3, text.len - 3};  // Notepad's UTF-8 mark
        text = str_trim(text);
        if (text.len) return text;
    }
    return str(SYSTEM);
}

void summary_set_max_words(App* app, u32 words) {
    words = words < 50 ? 50 : words > 2000 ? 2000 : words;
    app->config.summary.max_words = words;
    config_save_u32("summary", "max_words", words);
    summary_options_publish(app);
}

void summary_set_ask_reminders(App* app, bool on) {
    app->config.summary.ask_reminders = on;
    config_save_bool("summary", "ask_reminders", on);
    summary_options_publish(app);
}

// ---------------------------------------------------------------------------------------------
// Writing

// Asks the transcriber for the GPU and waits until it says its model is off it (Worker.gpu_free).
// False if lifelog is closing. Not on the main thread.
static bool gpu_take(App* app) {
    Worker* w = &app->worker;
    atomic_set(&w->summary_needs_gpu, 1);
    worker_wake(app);
    if (!w->gpu_free) return !stopping(app);  // the CLI: no transcriber here
    HANDLE waits[2] = {app->stop, w->gpu_free};
    DWORD got = WaitForMultipleObjects(2, waits, FALSE, 60000);
    if (got == WAIT_TIMEOUT) WARN("the transcriber did not free the GPU in a minute: going ahead");
    return got != WAIT_OBJECT_0;
}

// Hands the GPU back once the summary model's server has exited (llm_close waits for it).
static void gpu_give_back(App* app) {
    atomic_set(&app->worker.summary_needs_gpu, 0);
    worker_wake(app);
}

// On the writer thread. `labels` are the apps' names, from its job.
// `saved`: the day's row, once it is saved.
static bool write_day(App* app, const char* day, AppLabels* labels, SummaryDay* saved, char* error, i32 cap) {
    SummaryOptions options = summary_options(app);  // as they are now: a change counts from the next summary
    const SummaryOptions* cfg = &options;
    LOG("writing the summary for %s with %s", day, cfg->model);

    // 1. Let transcription catch up, so the end of the day is in it. The supervisor says when its
    // queue or state changes (Worker.live_changed); at most 20 minutes, and only a minute while
    // it cannot run (paused, or waiting for a game): then sum up what there is.
    phase_post(app, "waiting for transcription to catch up");
    f64 started = now_ts(), idle_since = now_ts();
    for (;;) {
        f64 now = now_ts();
        if (atomic_get(&app->worker.live_queued) == 0) break;
        if (atomic_get(&app->worker.live_running)) idle_since = now;
        f64 until = started + 20 * 60;
        if (!atomic_get(&app->worker.live_running) && idle_since + 60 < until) until = idle_since + 60;
        if (now >= until || !app->worker.live_changed) break;
        HANDLE waits[2] = {app->stop, app->worker.live_changed};
        if (WaitForMultipleObjects(2, waits, FALSE, (DWORD)((until - now) * 1000) + 1) == WAIT_OBJECT_0) break;
    }

    // 2. Take the GPU from the worker.
    if (!gpu_take(app)) {
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
    name_lines(labels, a, lines);
    Slice<Chunk> chunks = transcript_chunks(a, lines);
    Slice<Flag> flags = flagged_moments(a, lines);
    Slice<Slice<Str>> mine = {PUSH_ARRAY(a, Slice<Str>, lines.count), 0};
    for (i32 i = 0; i < lines.count; i++)
        if (is_mic(&lines.items[i])) mine.items[mine.count++] = word_set(a, lines.items[i].text);
    i64 words = 0;
    for (i32 i = 0; i < chunks.count; i++) words += my_words(chunks.items[i].text);
    List<Str> never = {};
    {
        sqlite3_stmt* st = prepare(db, "SELECT topic FROM summary_never ORDER BY id");
        while (st && sqlite3_step(st) == SQLITE_ROW) list_push(&never, col_str(st, 0, a));
        sqlite3_finalize(st);
    }
    List<Str> never_low = {};
    for (i32 i = 0; i < never.count; i++) list_push(&never_low, str_lower(a, never.items[i]));
    Str rule = never_rule(a, never.items, never.count);
    Str written = {};          // the summary, in Markdown
    Json remember = {};        // the model's points for the flagged moments
    List<Json> candidates = {};  // reminders from the notes
    Str system = system_prompt(a);

    if (!chunks.count) {
        snprintf(error, cap, "nothing was transcribed that day");
        goto done;
    }
    phase_post(app, "starting the AI");
    if (words < QUIET_DAY_WORDS) {
        // Barely a word said: that is the summary; the apps' audio is not the day.
        written = str(quiet_summary(words));
    } else {
        if (!llm_open(app, cfg, 12288, &llm, error, cap)) goto done;
        // 1. Notes on each chunk, filed under its part of the day with its times and whose they are.
        Buf notes[PART_COUNT] = {};
        i64 said[PART_COUNT] = {};
        f64 first[PART_COUNT] = {}, last[PART_COUNT] = {};
        bool ok_parts = true;
        for (i32 i = 0; i < chunks.count && ok_parts; i++) {
            Chunk* c = &chunks.items[i];
            i64 n = my_words(c->text);
            if (n < QUIET_PART_WORDS) continue;
            phase_post(app, "reading the day (part %d of %d)", i + 1, chunks.count);
            Json part = {};
            Str schema = str_fmt(a, NOTES_SCHEMA, c->who.ptr);
            if (!llm_json(&llm, a, system, notes_prompt(a, day, c, rule), schema, &part, error, cap)) {
                ok_parts = false;
                break;
            }
            const Json* list = json_get(&part, "notes");
            if (!json_len(list)) continue;
            Buf* b = &notes[c->part];
            if (!buf_len(b)) first[c->part] = c->first;
            last[c->part] = c->last;
            said[c->part] += n;
            char t0[6], t1[6];
            hm_of(c->first, t0);
            hm_of(c->last, t1);
            buf_fmt(b, "\n%s–%s\n", t0, t1);
            for (i32 k = 0; k < json_len(list); k++) {
                Str who = json_text(json_get(json_at(list, k), "who"));
                Str note = str_trim(json_text(json_get(json_at(list, k), "note")));
                if (note.len) buf_fmt(b, "- [%.*s] %.*s\n", (int)who.len, who.ptr, (int)note.len, note.ptr);
            }
            Json kept = keep_real_reminders(a, json_get(&part, "reminders"), mine);
            for (i32 k = 0; k < kept.count; k++) list_push(&candidates, kept.items[k]);
        }

        // 2. Each part's notes cut down to its share of the bullets (about one per 20 words of
        // max_words), each on its own topic. The labels come from here, never from the model.
        i64 total = 0;
        for (i32 p = 0; p < PART_COUNT; p++) total += said[p];
        Buf out = {};
        for (i32 p = 0; p < PART_COUNT && ok_parts; p++) {
            if (!buf_len(&notes[p])) continue;
            i32 points = (i32)((cfg->max_words / 20.0) * (f64)said[p] / (f64)(total ? total : 1) + 0.5);
            points = points < 2 ? 2 : points > 8 ? 8 : points;
            phase_post(app, "writing the %s", str_lower(a, str(PARTS[p])).ptr);
            Json got = {};
            Str prompt = points_prompt(a, day, p, first[p], last[p], buf_take(&notes[p], a), points, rule);
            if (!llm_json(&llm, a, system, prompt, str_fmt(a, POINTS_SCHEMA, points), &got, error, cap)) {
                ok_parts = false;
                break;
            }
            const Json* list = json_get(&got, "points");
            i32 shown = 0;
            for (i32 k = 0; k < json_len(list); k++) {
                Str text = str_trim(json_text(json_get(json_at(list, k), "text")));
                Str topic = str_trim(json_text(json_get(json_at(list, k), "topic")));
                bool again = false;  // a topic already written: the model repeating itself
                for (i32 j = 0; j < k && !again; j++)
                    again = str_eq_nocase(topic, str_trim(json_text(json_get(json_at(list, j), "topic"))));
                if (!text.len || again) continue;
                if (!shown++) buf_fmt(&out, "%s**%s**\n", buf_len(&out) ? "\n" : "", PARTS[p]);
                buf_fmt(&out, "- %.*s\n", (int)text.len, text.ptr);
            }
        }
        for (i32 p = 0; p < PART_COUNT; p++) buf_free(&notes[p]);
        written = buf_take(&out, a);

        // 3. The flagged moments, in my own words around each.
        if (ok_parts && flags.count) {
            phase_post(app, "writing what to remember");
            Str prompt = str_fmt(a, "%s\n\nReturn JSON. %s", flags_material(a, flags).ptr, REMEMBER_RULE);
            Json got = {};
            ok_parts = llm_json(&llm, a, system, prompt, str(REMEMBER_SCHEMA), &got, error, cap);
            remember = *json_get(&got, "remember");
        }
        if (!ok_parts) goto done;
    }
    // The server has exited here, so the graphics card is free again.
    llm_close(&llm);
    gpu_give_back(app);

    {
        Str summary = strip_never(a, str_trim(written), never_low.items, never_low.count);
        if (!summary.len && !flags.count) {
            snprintf(error, cap, "the model returned an empty summary");
            goto done;
        }
        Str section = remember_section(a, flags, &remember);
        while (section.len && isspace((u8)section.ptr[section.len - 1])) section.len--;
        summary = str_trim(str_fmt(a, "%.*s%.*s", (int)summary.len, summary.ptr, (int)section.len, section.ptr));
        Json reminders = {};
        reminders.type = JSON_ARRAY;
        reminders.items = candidates.items;
        reminders.count = candidates.count;
        f64 saved_ts = now_ts();
        bool stored = tx(db, "BEGIN IMMEDIATE");
        stored = stored && save(db, day, "done", saved_ts, cfg->model, summary, json_string_of(a, &reminders)) &&
                 items_replace(db, day, summary, &reminders) && tx(db, "COMMIT");
        if (!stored) {
            snprintf(error, cap, "could not save the summary: %s", sqlite3_errmsg(db));
            tx(db, "ROLLBACK");
            goto done;
        }
        Slice<ItemRow> rows = items_read(db, a, day);
        i32 shown_reminders = 0;
        for (i32 i = 0; i < rows.count; i++) shown_reminders += rows.items[i].kind == ITEM_REMINDER;
        *saved = day_row(day, "done", shown_reminders);
        if (cfg->ask_reminders) alerts_offer(app, db, day, &reminders);
        if (!write_markdown(app, day, rows)) WARN("could not write the summary file for %s", day);
        i32 count = reminders.count;
        LOG("summary for %s done (%lld words, %d reminders)", day, count_words(summary), count);
        phase_post(app, "done");

        Buf plain = {};
        Str rest = summary, line = {};
        i32 taken = 0;
        while (taken < 3 && next_line(&rest, &line)) {
            if (!str_trim(line).len) continue;
            if (taken++) buf_cstr(&plain, " ");
            buf_add(&plain, strip_md(a, line));
        }
        Str text = buf_take(&plain, a);
        ToastJob t = {};
        SET(t.tag, str(TOAST_TAG));
        SET(t.title, str("Your day, summed up"));
        if (count == 0) set(t.body, sizeof(t.body), text);
        else if (count == 1) set(t.body, sizeof(t.body), str_fmt(a, "1 reminder. %s", text.ptr));
        else set(t.body, sizeof(t.body), str_fmt(a, "%d reminders. %s", count, text.ptr));
        char open[160];
        make_link(open, sizeof(open), "summary/open?day=%s", day);
        toast_button(&t, "Read it", open);
        SET(t.launch, str(open));
        notify_show(app, &t);
        ok = true;
    }

done:
    llm_close(&llm);
    list_free(&candidates);
    list_free(&never);
    list_free(&never_low);
    arena_release(&work);
    sqlite3_close(db);
    return ok;
}

// The writer's job: made by the main thread (summary_act "now"), filled in by the writer, and
// posted back when it is done.
struct WriterJob {
    App* app;
    char day[11];
    AppLabels labels;
    bool ok;
    SummaryDay saved;  // the day's row, if it was saved
    char error[512];
};

// The main thread: the writer has finished.
static void writer_done(App* app, Job* job) {
    WriterJob* w = (WriterJob*)job->data;
    app->summary.writing = false;
    app->summary.writing_day[0] = 0;
    if (w->saved.day[0]) {
        day_put(app, &w->saved);
        app->summary.version++;
    }
    if (!w->ok) {
        ERR("summary for %s: %s", w->day, w->error);
        phase_set(app, "failed: %s", w->error);
    }
    wake_window(app);
    if (!w->ok && !stopping(app)) {
        ToastJob t = {};
        SET(t.tag, str(TOAST_TAG));
        SET(t.title, str("The summary could not be written"));
        set(t.body, sizeof(t.body), str(w->error));
        char again[160];
        make_link(again, sizeof(again), "summary/now?day=%s", w->day);
        toast_button(&t, "Try again", again);
        notify_show(app, &t);
    }
}

static DWORD WINAPI writer_main(void* param) {
    Job* job = (Job*)param;
    WriterJob* w = (WriterJob*)job->data;
    App* app = w->app;
    w->ok = write_day(app, w->day, &w->labels, &w->saved, w->error, sizeof(w->error));
    gpu_give_back(app);  // write_day closed the server on every path
    job_post_wait(app, job);
    return 0;
}

// The main thread: starts the writer on `day`, with the apps' names it needs.
static void writer_start(App* app, const char* day) {
    Job* job = job_new(writer_done);
    WriterJob* w = PUSH(&job->arena, WriterJob);
    w->app = app;
    SET(w->day, str(day));
    w->labels = app_labels(app, &job->arena);
    job->data = w;
    HANDLE t = CreateThread(0, 0, writer_main, job, 0, 0);
    CHECK(t, "the summary writer to start (error %lu)", GetLastError());
    SetThreadDescription(t, L"summary writer");
    CloseHandle(t);
}

// ---------------------------------------------------------------------------------------------
// Asking

// The main thread. `mine` and `apps`: today's seconds of talk, read by the
// summary thread.
static void ask(App* app, const char* day, f64 mine, f64 apps) {
    Mark m = mark(scratch());
    ToastJob t = {};
    SET(t.tag, str(TOAST_TAG));
    SET(t.title, str("Your summary starts in 5 minutes"));
    set(t.body, sizeof(t.body),
             str_fmt(scratch(), "It uses the graphics card for a few minutes. Today: %s of you talking, %s from apps.",
                     duration_text(scratch(), mine).ptr, duration_text(scratch(), apps).ptr));
    static const char* ACTIONS[][2] = {{"Start now", "now"}, {"In an hour", "later"}, {"Delay until…", "pick"}, {"Skip today", "skip"}};
    for (i32 i = 0; i < COUNT(ACTIONS); i++) {
        char link[160];
        make_link(link, sizeof(link), "summary/%s?day=%s", ACTIONS[i][1], day);
        toast_button(&t, ACTIONS[i][0], link);
    }
    t.sticky = true;
    rewind(m);
    char at[6];
    hm_of(now_ts() + COUNTDOWN_SECS, at);
    app->summary.countdown_ts = now_ts() + COUNTDOWN_SECS;
    SET(app->summary.countdown_day, str(day));
    phase_set(app, "starts at %s", at);
    notify_show(app, &t);
    LOG("the summary for %s starts at %s unless delayed", day, at);
}

const char* summary_acted_text(SummaryActResult r, char* out, i32 cap) {
    char at[6];
    switch (r.what) {
    case ACTED_WRITING: return "writing";
    case ACTED_ALREADY_WRITING: return "already writing";
    case ACTED_LATER: return "asking again in an hour";
    case ACTED_UNTIL:
        hm_of(r.at, at);
        snprintf(out, cap, "asking again at %s", at);
        return out;
    case ACTED_SKIPPED: return "skipped";
    case ACTED_PICKING: return "pick a time";
    case ACTED_BAD_DAY: return "bad day: it must look like 2026-03-01";
    case ACTED_BAD_TIME: return "time must look like 23:00";
    case ACTED_BAD_ACTION: return "do must be now, later, until, skip or pick";
    }
    return "";
}

SummaryActResult summary_act(App* app, const char* action, const char* day_in, const char* at) {
    SummaryActResult r = {};
    if (!action) action = "";
    char day[11];
    i32 y, m, d;
    if (day_in && day_in[0]) {
        if (!parse_day(day_in, &y, &m, &d)) {
            r.what = ACTED_BAD_DAY;
            return r;
        }
        SET(day, str(day_in));
    } else {
        day_of(now_ts(), day);
    }
    // "Delay until…" only opens the delay box; the countdown goes on until a time is picked.
    if (strcmp(action, "pick") == 0) {
        ui_popup(app, POPUP_DELAY, day);
        r.what = ACTED_PICKING;
        return r;
    }
    i32 h = 0, mi = 0;
    bool now = strcmp(action, "now") == 0, later = strcmp(action, "later") == 0, until = strcmp(action, "until") == 0,
         skip = strcmp(action, "skip") == 0;
    if (!now && !later && !until && !skip) {
        r.what = ACTED_BAD_ACTION;
        return r;
    }
    if (until && !parse_hhmm(str(at), &h, &mi)) {
        r.what = ACTED_BAD_TIME;
        return r;
    }
    notify_remove(app, TOAST_TAG);
    app->summary.countdown_ts = 0;
    if (now) {
        if (app->summary.writing) {
            r.what = ACTED_ALREADY_WRITING;
            return r;
        }
        app->summary.writing = true;
        SET(app->summary.writing_day, str(day));
        writer_start(app, day);
        wake_window(app);
        r.what = ACTED_WRITING;
    } else if (later) {
        app->summary.snooze_ts = r.at = now_ts() + 3600;
        LOG("summary for %s: asking again in an hour", day);
        phase_set(app, "asking again in an hour");
        r.what = ACTED_LATER;
    } else if (until) {
        f64 when = next_at(h, mi);
        char text[6];
        hm_of(when, text);
        app->summary.snooze_ts = r.at = when;
        LOG("summary for %s: asking again at %s", day, text);
        phase_set(app, "asking again at %s", text);
        r.what = ACTED_UNTIL;
    } else {
        // Saved on the database thread; if that fails, the window says so (edit_apply).
        Job* job;
        edit_new(app, &job, EDIT_SKIP, day);
        db_post(job, edit_run);
        LOG("summary for %s: skipped", day);
        phase_set(app, "skipped today");
        r.what = ACTED_SKIPPED;
    }
    return r;
}

// How much was said today, read on the database thread once it is time to ask.
struct TalkJob {
    char day[11];
    bool ok;
    f64 mine, apps;
};

static void talk_run(sqlite3* db, Job* job) {
    TalkJob* t = (TalkJob*)job->data;
    if (db) t->ok = talk_secs(db, t->day, &t->mine, &t->apps);
}

// The main thread: asks, unless there is too little to sum up (or it started meanwhile).
static void talk_apply(App* app, Job* job) {
    TalkJob* t = (TalkJob*)job->data;
    Summary* s = &app->summary;
    if (s->writing || s->countdown_ts) return;
    if (t->ok && t->mine + t->apps >= 120.0) ask(app, t->day, t->mine, t->apps);
    else phase_set(app, "too little recorded today to sum up");
}

// The clock's reading, for the main thread (summary_tick).
struct SummaryTick {
    char day[11];
    bool gaming;   // a game runs or the GPU is busy
};

// The main thread: asks once it is time and starts the summary when the countdown ends.
static void summary_tick(App* app, Event* e) {
    SummaryTick t = latest_take<SummaryTick>(e);
    const char* day = t.day;
    bool gaming = t.gaming;
    Summary* s = &app->summary;
    // Once a day (and at start): old audio goes.
    static char pruned[11];
    if (strcmp(pruned, day) != 0) {
        SET(pruned, str(day));
        audio_prune(app);
    }
    if (gaming) s->last_game_ts = now_ts();
    // Counting down to a summary: a game starting puts it off; otherwise it starts on time.
    bool counting = s->countdown_ts != 0;
    if (counting) {
        if (gaming) {
            s->countdown_ts = 0;
            notify_remove(app, TOAST_TAG);
            LOG("a game started: the summary waits until it is over");
            phase_set(app, "waiting until you stop playing");
        } else if (now_ts() >= s->countdown_ts) {
            char countdown_day[11];
            SET(countdown_day, str(s->countdown_day));
            summary_act(app, "now", countdown_day, 0);
        }
    }
    bool look = !counting && app->config.summary.enabled && !s->writing && now_ts() >= ask_moment(day, app->config.summary.ask_at) &&
                now_ts() >= s->snooze_ts;
    // Not done or skipped yet: Summary.days has every saved day.
    if (!look || !s->lists_ready || summary_day_find(app, day)) return;
    if (s->last_game_ts && now_ts() - s->last_game_ts < AFTER_GAME_SECS) {
        // Never during a game: wait until it has been closed for a while.
        phase_set(app, "waiting until you stop playing");
        return;
    }
    // Asks if enough was said (talk_apply); either way not again for an hour.
    s->snooze_ts = now_ts() + 3600;
    Job* job = job_new(talk_apply);
    TalkJob* tj = PUSH(&job->arena, TalkJob);
    SET(tj->day, str(day));
    job->data = tj;
    db_post(job, talk_run);
}

// The summary's clock. Every 15 s (and when woken) it posts what the main thread's clock needs
// (summary_tick): two flags, no database. It opens the database only when a reminder is due
// (Summary.next_alert_ms).
static DWORD WINAPI summary_main(void* param) {
    App* app = (App*)param;
    Summary* s = &app->summary;
    f64 next_tick = 0;
    for (;;) {
        f64 now = now_ts();
        if (now * 1000.0 >= (f64)atomic_get64(&s->next_alert_ms)) alerts_check(app);
        if (now >= next_tick) {
            SummaryTick t = {};
            day_of(now, t.day);
            t.gaming = atomic_get(&app->apps.game_on) || atomic_get(&app->worker.gpu_busy);
            latest_send(app, &s->tick, summary_tick, t);
            next_tick = now + 15;
        }
        f64 until = next_tick, alert = (f64)atomic_get64(&s->next_alert_ms) / 1000.0;
        if (alert < until) until = alert;
        now = now_ts();
        DWORD ms = until <= now ? 0 : (DWORD)((until - now) * 1000.0) + 1;
        HANDLE waits[2] = {app->stop, s->wake};
        DWORD got = WaitForMultipleObjects(2, waits, FALSE, ms);
        if (got == WAIT_OBJECT_0) break;
        if (got == WAIT_OBJECT_0 + 1) next_tick = 0;  // the settings changed or a reminder was set: look again now
    }
    return 0;
}

void summary_start(App* app) {
    app->summary.notify.lock = SRWLOCK_INIT;
    app->summary.options_lock = SRWLOCK_INIT;
    app->summary.wake = CreateEventW(0, FALSE, FALSE, 0);
    CHECK(app->summary.wake, "the summary's wake event (error %lu)", GetLastError());
    summary_options_publish(app);
    db_post(job_new(0), items_migrate_run);
    Job* job = job_new(lists_apply);
    job->data = PUSH(&job->arena, ListsRead);
    db_post(job, lists_run);
    if (app->config.summary.enabled) phase_set(app, "asks at %s", app->config.summary.ask_at);
    start_thread(app, "notify", notify_main);
    start_thread(app, "summary", summary_main);
}

// ---------------------------------------------------------------------------------------------
// check-ai

bool summary_check_ai(App* app, char* said, i32 cap) {
    SummaryOptions options = summary_options(app);
    const SummaryOptions* cfg = &options;
    f64 started = now_ts();
    Llm llm = {};
    char error[512] = {};
    bool ok = gpu_take(app);
    if (!ok) snprintf(error, sizeof(error), "lifelog is closing");
    ok = ok && llm_open(app, cfg, 4096, &llm, error, sizeof(error));
    f64 loaded = now_ts() - started;
    Arena a = {};
    Json reply = {};
    if (ok) {
        ok = llm_json(&llm, &a, str("Answer briefly."), str("Say hello in five words. Return JSON."),
                      str("{\"type\":\"object\",\"properties\":{\"reply\":{\"type\":\"string\"}},\"required\":[\"reply\"]}"), &reply,
                      error, sizeof(error));
    }
    llm_close(&llm);
    gpu_give_back(app);
    if (ok) {
        Str text = json_string_of(&a, &reply);
        snprintf(said, cap, "loaded %s in %.0fs\n%s\nstopped after %.0fs", cfg->model, loaded, text.ptr, now_ts() - started);
    } else {
        snprintf(said, cap, "%s", error);
    }
    arena_release(&a);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// For voice commands

bool reminder_add(App* app, Str text, Str said, f64 due, char* error, i32 cap) {
    sqlite3* db = db_open(app);
    if (!db) {
        snprintf(error, cap, "could not open the database");
        return false;
    }
    f64 now = now_ts();
    char day[11];
    day_of(now, day);
    LocalTime t = local_time(now);
    char said_time[8];
    snprintf(said_time, sizeof(said_time), "%02d:%02d", t.hour, t.minute);
    sqlite3_stmt* st = alert_insert(db, str(day), text, said, str(said_time), due, 0);
    bool ok = sql_done(db, st) > 0;
    sqlite3_close(db);
    if (!ok) {
        snprintf(error, cap, "could not save the reminder");
        return false;
    }
    LOG("reminder set for %.0f s from now: %.*s", due - now, (int)text.len, text.ptr);
    alert_due_at(app, due);
    return true;
}

// The newest this many characters are summarised (about 6,000 words for the AI's 12,288 tokens).
static const i64 SUMMARISE_MAX_CHARS = 24000;

bool summary_quick(App* app, Arena* a, Str transcript, Str* out, char* error, i32 cap) {
    transcript = str_tail_lines(transcript, SUMMARISE_MAX_CHARS);
    SummaryOptions options = summary_options(app);
    Llm llm = {};
    bool ok = gpu_take(app);
    if (!ok) snprintf(error, cap, "lifelog is closing");
    ok = ok && llm_open(app, &options, 12288, &llm, error, cap);
    Json reply = {};
    if (ok) {
        Str system = str("You summarise a stretch of a person's day from its transcript. Lines are \"[time] speaker: text\": "
                         "the microphone is the person, the rest are apps (calls, videos, games). Write a few short plain sentences "
                         "about what was talked about and anything decided or to do. No preamble. Return JSON.");
        ok = llm_json(&llm, a, system, transcript,
                      str("{\"type\":\"object\",\"properties\":{\"summary\":{\"type\":\"string\"}},\"required\":[\"summary\"]}"),
                      &reply, error, cap);
    }
    llm_close(&llm);
    gpu_give_back(app);
    if (ok) {
        *out = str_trim(json_text(json_get(&reply, "summary")));
        if (!out->len) {
            snprintf(error, cap, "the AI gave an empty summary");
            ok = false;
        }
    }
    return ok;
}
