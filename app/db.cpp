// db.cpp: see db.h.

static Str schema_text(Arena* a) {
    // Next to the exe once installed, or in the repo while developing (build\lifelog.exe).
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\schema.sql", exe_dir());
    Str s = read_file(a, path);
    if (!s.ptr) {
        swprintf(path, MAX_PATH, L"%ls\\..\\schema.sql", exe_dir());
        s = read_file(a, path);
    }
    return s;
}

// Opens the database for reading and writing (made if missing), or `read_only` for copying and
// playback, which never write.
static sqlite3* db_open_path(bool read_only) {
    wchar_t path[MAX_PATH];
    db_path(path);
    Mark m = mark(scratch());
    Str utf8 = narrow(scratch(), path);
    sqlite3* db = 0;
    int flags = read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2(utf8.ptr, &db, flags | SQLITE_OPEN_NOMUTEX, 0);
    rewind(m);
    if (rc != SQLITE_OK) {
        ERR("opening the database%s: %s", read_only ? " to read" : "", db ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 5000);
    if (!read_only) sqlite3_exec(db, "PRAGMA journal_mode = WAL", 0, 0, 0);
    return db;
}

static bool has_column(sqlite3* db, const char* table, const char* column) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM pragma_table_info('%s') WHERE name = '%s'", table, column);
    sqlite3_stmt* st = 0;
    bool found = false;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) == SQLITE_OK) found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

static bool migrate_recordings(sqlite3* db);

bool db_init() {
    wchar_t dir[MAX_PATH];
    Mark m = mark(scratch());
    wchar_t* data = wide(scratch(), str(data_dir()));
    SHCreateDirectoryExW(0, data, 0);
    spool_dir(dir);
    SHCreateDirectoryExW(0, dir, 0);
    rewind(m);

    sqlite3* db = db_open_path(false);
    if (!db) return false;
    Str schema = schema_text(scratch());
    bool ok = schema.ptr != 0;
    if (!ok) ERR("schema.sql is missing next to the exe");
    char* err = 0;
    if (ok && sqlite3_exec(db, str_copy(scratch(), schema).ptr, 0, 0, &err) != SQLITE_OK) {
        ERR("creating the tables: %s", err);
        sqlite3_free(err);
        ok = false;
    }
    // Columns added after the first release.
    if (ok && !has_column(db, "segments", "recording")) {
        if (!has_column(db, "segments", "audio")) sqlite3_exec(db, "ALTER TABLE segments ADD COLUMN audio TEXT", 0, 0, 0);
        if (!has_column(db, "segments", "audio_offset")) sqlite3_exec(db, "ALTER TABLE segments ADD COLUMN audio_offset REAL", 0, 0, 0);
        ok = migrate_recordings(db);
    }
    if (ok && !has_column(db, "segments", "words")) sqlite3_exec(db, "ALTER TABLE segments ADD COLUMN words BLOB", 0, 0, 0);
    if (ok && !has_column(db, "files", "link")) sqlite3_exec(db, "ALTER TABLE files ADD COLUMN link INTEGER NOT NULL DEFAULT 0", 0, 0, 0);
    if (ok && !has_column(db, "alerts", "item")) sqlite3_exec(db, "ALTER TABLE alerts ADD COLUMN item INTEGER", 0, 0, 0);
    sqlite3_close(db);
    rewind(m);
    return ok;
}

sqlite3* db_open(App*) {
    return db_open_path(false);
}

// A column as a Str copied into `a` ("" for NULL).
static Str col_str(sqlite3_stmt* st, int i, Arena* a) {
    const char* t = (const char*)sqlite3_column_text(st, i);
    return str_copy(a, {t ? t : "", t ? sqlite3_column_bytes(st, i) : 0});
}

// Binds `s` as text, "" when it is empty: for NOT NULL columns.
static void bind_str(sqlite3_stmt* st, int i, Str s) { sqlite3_bind_text(st, i, s.ptr ? s.ptr : "", (int)s.len, SQLITE_TRANSIENT); }

// Binds `s` as text, or NULL when it is empty: for columns where NULL means "none" (a line's
// audio, exe or title).
static void bind_opt(sqlite3_stmt* st, int i, Str s) {
    if (s.len) sqlite3_bind_text(st, i, s.ptr, (int)s.len, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, i);
}

// Lines used to name their clip's file and their start in it in seconds (the "audio" and
// "audio_offset" columns), and each mix worked out where a clip starts from its lines. Now a
// recording holds the clip and each line its sample range in it. Runs once, in one transaction.
static bool migrate_recordings(sqlite3* db) {
    const i32 rate = 16000;
    Arena* a = scratch();
    Mark m = mark(a);
    wchar_t audio[MAX_PATH];
    audio_dir(audio);
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE;"
                               "CREATE INDEX migrate_audio ON segments (audio);"
                               "ALTER TABLE segments ADD COLUMN recording INTEGER;"
                               "ALTER TABLE segments ADD COLUMN start_sample INTEGER;"
                               "ALTER TABLE segments ADD COLUMN end_sample INTEGER;", 0, 0, 0) == SQLITE_OK;
    sqlite3_stmt* clips = 0;
    sqlite3_stmt* add = 0;
    sqlite3_stmt* point = 0;
    ok = ok && sqlite3_prepare_v2(db, "SELECT audio, MIN(start_ts - audio_offset), MAX(end_ts), MIN(source), MIN(speaker) FROM segments "
                                      "WHERE audio IS NOT NULL AND audio_offset IS NOT NULL GROUP BY audio", -1, &clips, 0) == SQLITE_OK;
    ok = ok && sqlite3_prepare_v2(db, "INSERT INTO recordings (file, sample_rate, start, length, source, speaker) VALUES (?, ?, ?, ?, ?, ?)",
                                  -1, &add, 0) == SQLITE_OK;
    ok = ok && sqlite3_prepare_v2(db, "UPDATE segments SET recording = ?1, start_sample = CAST(ROUND(start_ts * ?3) AS INTEGER) - ?2, "
                                      "end_sample = CAST(ROUND(end_ts * ?3) AS INTEGER) - ?2 WHERE audio = ?4", -1, &point, 0) == SQLITE_OK;
    i32 count = 0;
    while (ok && sqlite3_step(clips) == SQLITE_ROW) {
        Str file = col_str(clips, 0, a);
        f64 start_secs = sqlite3_column_double(clips, 1);
        i64 start = (i64)floor(start_secs * rate + 0.5);
        i64 length = (i64)ceil((sqlite3_column_double(clips, 2) - start_secs) * rate);
        // The file's own length, while it is still kept.
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\%ls", audio, wide(a, file));
        for (wchar_t* c = path; *c; c++)
            if (*c == L'/') *c = L'\\';
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &info) && info.nFileSizeLow > WAV_HEADER && !info.nFileSizeHigh)
            length = (info.nFileSizeLow - WAV_HEADER) / 2;
        bind_str(add, 1, file);
        sqlite3_bind_int(add, 2, rate);
        sqlite3_bind_int64(add, 3, start);
        sqlite3_bind_int64(add, 4, length);
        bind_str(add, 5, col_str(clips, 3, a));
        bind_str(add, 6, col_str(clips, 4, a));
        ok = sqlite3_step(add) == SQLITE_DONE;
        sqlite3_reset(add);
        sqlite3_bind_int64(point, 1, sqlite3_last_insert_rowid(db));
        sqlite3_bind_int64(point, 2, start);
        sqlite3_bind_int(point, 3, rate);
        bind_str(point, 4, file);
        ok = ok && sqlite3_step(point) == SQLITE_DONE;
        sqlite3_reset(point);
        count++;
    }
    sqlite3_finalize(clips);
    sqlite3_finalize(add);
    sqlite3_finalize(point);
    ok = ok && sqlite3_exec(db, "DROP INDEX migrate_audio;"
                                "ALTER TABLE segments DROP COLUMN audio;"
                                "ALTER TABLE segments DROP COLUMN audio_offset;"
                                "COMMIT;", 0, 0, 0) == SQLITE_OK;
    if (ok) LOG("database: %d kept clips moved to the recordings table", count);
    else {
        ERR("database: moving the kept clips to the recordings table failed: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
    }
    rewind(m);
    return ok;
}

#define LINE_COLUMNS                                                                                     "s.id, s.start_ts, s.end_ts, s.source, s.speaker, s.title, s.text, s.recording IS NOT NULL, s.words,"     " (SELECT sample_rate FROM recordings WHERE id = s.recording)"

// Runs `st` (already bound) and reads Lines from it; finalizes it.
static Slice<Line> read_lines(sqlite3_stmt* st, Arena* a, i32 limit) {
    // Gathered in a List first: the strings go into `a` as they are read, the array after.
    Slice<Line> out = {};
    List<Line> lines = {};
    while (sqlite3_step(st) == SQLITE_ROW && lines.count < limit) {
        Line l = {};
        l.id = sqlite3_column_int64(st, 0);
        l.start_ts = sqlite3_column_double(st, 1);
        l.end_ts = sqlite3_column_double(st, 2);
        const char* src = (const char*)sqlite3_column_text(st, 3);
        l.source = src && strcmp(src, "mic") == 0 ? SOURCE_MIC : SOURCE_APP;
        l.speaker = col_str(st, 4, a);
        l.title = col_str(st, 5, a);
        l.text = col_str(st, 6, a);
        l.audio = sqlite3_column_int(st, 7) != 0;
        // Its words (see schema.sql), with their sound as unix seconds.
        const i32* q = (const i32*)sqlite3_column_blob(st, 8);
        i32 n = sqlite3_column_bytes(st, 8) / 16;
        i32 rate = sqlite3_column_int(st, 9);
        if (q && n && rate > 0) {
            Word* w = PUSH_ARRAY(a, Word, n);
            for (i32 i = 0; i < n; i++) {
                i32 v[4];
                memcpy(v, q + i * 4, 16);
                w[i] = {v[0], v[1], l.start_ts + (f64)v[2] / rate, l.start_ts + (f64)v[3] / rate};
            }
            l.words = {w, n};
        }
        list_push(&lines, l);
    }
    sqlite3_finalize(st);
    out.items = PUSH_ARRAY(a, Line, lines.count);
    if (lines.count) memcpy(out.items, lines.items, sizeof(Line) * lines.count);
    out.count = lines.count;
    list_free(&lines);
    return out;
}

static sqlite3_stmt* prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) {
        ERR("query failed: %s (%s)", sqlite3_errmsg(db), sql);
        return 0;
    }
    return st;
}

Slice<Line> db_lines_between(sqlite3* db, Arena* a, f64 from, f64 to, i32 limit) {
    sqlite3_stmt* st = prepare(db, "SELECT " LINE_COLUMNS " FROM segments s WHERE s.start_ts >= ?1 AND s.start_ts < ?2 ORDER BY s.start_ts");
    if (!st) return {};
    sqlite3_bind_double(st, 1, from);
    sqlite3_bind_double(st, 2, to);
    return read_lines(st, a, limit);
}

Slice<Line> db_lines_after(sqlite3* db, Arena* a, f64 ts, i32 limit) {
    sqlite3_stmt* st = prepare(db, "SELECT " LINE_COLUMNS " FROM segments s WHERE s.start_ts >= ?1 ORDER BY s.start_ts");
    if (!st) return {};
    sqlite3_bind_double(st, 1, ts);
    return read_lines(st, a, limit);
}

Slice<Line> db_search(sqlite3* db, Arena* a, Str query, i32 limit) {
    bool any = false;
    for (i64 i = 0; i < query.len; i++) any |= isalnum((u8)query.ptr[i]) || (u8)query.ptr[i] >= 0x80;
    if (!any) return {};
    // As an FTS5 phrase, so punctuation in what was typed cannot be read as query syntax.
    Mark m = mark(scratch());
    List<char> phrase = {};
    list_push(&phrase, '"');
    for (i64 i = 0; i < query.len; i++) {
        if (query.ptr[i] == '"') list_push(&phrase, '"');
        list_push(&phrase, query.ptr[i]);
    }
    const char* tail = "\" *";
    for (const char* c = tail; *c; c++) list_push(&phrase, *c);
    sqlite3_stmt* st = prepare(db, "SELECT " LINE_COLUMNS " FROM segments_fts f JOIN segments s ON s.id = f.rowid "
                                   "WHERE segments_fts MATCH ?1 ORDER BY s.start_ts DESC");
    Slice<Line> out = {};
    if (st) {
        sqlite3_bind_text(st, 1, phrase.items, phrase.count, SQLITE_TRANSIENT);
        out = read_lines(st, a, limit);
    }
    list_free(&phrase);
    rewind(m);
    return out;
}

static FileItem read_file_item(sqlite3_stmt* st, Arena* a) {
    FileItem f = {};
    f.id = sqlite3_column_int64(st, 0);
    f.ts = sqlite3_column_double(st, 1);
    f.name = col_str(st, 2, a);
    f.path = col_str(st, 3, a);
    f.kind = col_str(st, 4, a);
    f.size = sqlite3_column_int64(st, 5);
    f.link = sqlite3_column_int(st, 6) != 0;
    return f;
}

Slice<FileItem> db_files_between(sqlite3* db, Arena* a, f64 from, f64 to) {
    sqlite3_stmt* st = prepare(db, "SELECT id, ts, name, path, kind, size, link FROM files WHERE ts >= ?1 AND ts < ?2 ORDER BY ts");
    if (!st) return {};
    sqlite3_bind_double(st, 1, from);
    sqlite3_bind_double(st, 2, to);
    List<FileItem> list = {};
    while (sqlite3_step(st) == SQLITE_ROW) list_push(&list, read_file_item(st, a));
    sqlite3_finalize(st);
    Slice<FileItem> out = {PUSH_ARRAY(a, FileItem, list.count), list.count};
    if (list.count) memcpy(out.items, list.items, sizeof(FileItem) * list.count);
    list_free(&list);
    return out;
}

FileItem db_file(sqlite3* db, Arena* a, i64 id) {
    sqlite3_stmt* st = prepare(db, "SELECT id, ts, name, path, kind, size, link FROM files WHERE id = ?1");
    if (!st) return {};
    sqlite3_bind_int64(st, 1, id);
    FileItem f = {};
    if (sqlite3_step(st) == SQLITE_ROW) f = read_file_item(st, a);
    sqlite3_finalize(st);
    return f;
}

Slice<DayCount> db_days(sqlite3* db, Arena* a) {
    sqlite3_stmt* st = prepare(db, "SELECT date(start_ts, 'unixepoch', 'localtime', '-4 hours') AS d, COUNT(*) FROM segments GROUP BY d ORDER BY d");
    if (!st) return {};
    List<DayCount> list = {};
    while (sqlite3_step(st) == SQLITE_ROW) {
        DayCount d = {};
        const char* t = (const char*)sqlite3_column_text(st, 0);
        SET(d.day, str(t));
        d.lines = sqlite3_column_int(st, 1);
        list_push(&list, d);
    }
    sqlite3_finalize(st);
    Slice<DayCount> out = {PUSH_ARRAY(a, DayCount, list.count), list.count};
    if (list.count) memcpy(out.items, list.items, sizeof(DayCount) * list.count);
    list_free(&list);
    return out;
}

Slice<SpeakerCount> db_speakers(sqlite3* db, Arena* a) {
    sqlite3_stmt* st = prepare(db, "SELECT speaker, COUNT(*), MAX(start_ts) FROM segments GROUP BY speaker ORDER BY 2 DESC");
    if (!st) return {};
    List<SpeakerCount> list = {};
    while (sqlite3_step(st) == SQLITE_ROW) {
        SpeakerCount s = {};
        s.speaker = col_str(st, 0, a);
        s.lines = sqlite3_column_int64(st, 1);
        s.last_ts = sqlite3_column_double(st, 2);
        list_push(&list, s);
    }
    sqlite3_finalize(st);
    Slice<SpeakerCount> out = {PUSH_ARRAY(a, SpeakerCount, list.count), list.count};
    if (list.count) memcpy(out.items, list.items, sizeof(SpeakerCount) * list.count);
    list_free(&list);
    return out;
}

// ---------------------------------------------------------------------------------------------
// The database thread

struct DbTask {
    DbTask* next;
    Job* job;
    DbRun* run;
    volatile LONG64* wanted;  // db_post_latest: dropped unless this is still `asked`
    u64 asked;
};

static struct {
    SRWLOCK lock;  // guards head and tail
    DbTask* head;
    DbTask* tail;
    HANDLE wake;
    HANDLE thread;
    App* app;
} g_dbt;

static DWORD WINAPI db_thread_main(void*) {
    App* app = g_dbt.app;
    sqlite3* db = 0;
    bool tried = false;
    HANDLE waits[2] = {app->stop, g_dbt.wake};
    for (;;) {
        AcquireSRWLockExclusive(&g_dbt.lock);
        DbTask* t = g_dbt.head;
        g_dbt.head = g_dbt.tail = 0;
        ReleaseSRWLockExclusive(&g_dbt.lock);
        while (t) {
            DbTask* next = t->next;
            Job* job = t->job;  // t is in the job's arena
            bool stale = t->wanted && (u64)InterlockedCompareExchange64(t->wanted, 0, 0) != t->asked;
            if (stale || stopping(app)) {
                job_drop(job);
            } else {
                if (!db && !tried && app->storage.ready) {
                    db = db_open(app);
                    tried = !db;  // once: the log says why
                }
                t->run(db, job);
                job_post_wait(app, job);
            }
            t = next;
        }
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
    }
    // What was asked for after the end: dropped.
    AcquireSRWLockExclusive(&g_dbt.lock);
    for (DbTask* t = g_dbt.head; t;) {
        DbTask* next = t->next;
        job_drop(t->job);
        t = next;
    }
    g_dbt.head = g_dbt.tail = 0;
    ReleaseSRWLockExclusive(&g_dbt.lock);
    if (db) sqlite3_close(db);
    return 0;
}

void db_thread_start(App* app) {
    g_dbt.app = app;
    g_dbt.wake = CreateEventW(0, FALSE, FALSE, 0);
    CHECK(g_dbt.wake, "the database thread's event (error %lu)", GetLastError());
    g_dbt.thread = CreateThread(0, 0, db_thread_main, 0, 0, 0);
    CHECK(g_dbt.thread, "the database thread to start (error %lu)", GetLastError());
    SetThreadDescription(g_dbt.thread, L"database");
}

// After App.stop is set.
void db_thread_stop() {
    if (!g_dbt.thread) return;
    WaitForSingleObject(g_dbt.thread, 10000);
    CloseHandle(g_dbt.thread);
    g_dbt.thread = 0;
}

void db_post_latest(Job* job, DbRun* run, volatile LONG64* wanted, u64 asked) {
    CHECK(g_dbt.thread, "db_post after db_thread_start");
    DbTask* t = PUSH(&job->arena, DbTask);
    t->job = job;
    t->run = run;
    t->wanted = wanted;
    t->asked = asked;
    AcquireSRWLockExclusive(&g_dbt.lock);
    if (g_dbt.tail) g_dbt.tail->next = t;
    else g_dbt.head = t;
    g_dbt.tail = t;
    ReleaseSRWLockExclusive(&g_dbt.lock);
    SetEvent(g_dbt.wake);
}

void db_post(Job* job, DbRun* run) { db_post_latest(job, run, 0, 0); }
