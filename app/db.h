// db.h: the database (lifelog.db under data_dir, shared with the Python worker; schema.sql).
// Each thread opens its own connection with db_open. The query functions put their results in
// the arena they are given, so a whole day's lines are freed at once.

#pragma once

struct Storage {
    sqlite3* db;        // the window thread's connection
    bool ready;         // the data folder exists and the schema is in place
    u64 files_version;  // bumped when a file is pasted
};

// Opens a connection (WAL, busy timeout). Null if the database cannot be opened.
sqlite3* db_open(App* app);
// Creates the tables if needed and adds columns that came later. Any thread, unlocked.
bool db_init(Config* cfg);

struct Line {
    i64 id;
    f64 start_ts;
    f64 end_ts;
    Source source;
    Str speaker;   // "me", an exe name, or a person in a call
    Str title;
    Str text;
    bool audio;    // its clip was kept (it may since have been deleted after a week)
};

struct FileItem {
    i64 id;
    f64 ts;
    Str name;
    Str kind;    // "image" | "video" | "audio" | "text" | "file" | "folder" | "url"
    i64 size;
    Str path;    // "YYYY-MM-DD/<file>" under data_dir/files, or the full path or URL for a link
    bool link;   // points at the original, no copy kept
};

template <typename T>
struct Slice {
    T* items;
    i32 count;
};

// Lines in [from, to), oldest first, at most `limit`.
Slice<Line> db_lines_between(sqlite3* db, Arena* a, f64 from, f64 to, i32 limit);
// The first `limit` lines starting at or after `ts`.
Slice<Line> db_lines_after(sqlite3* db, Arena* a, f64 ts, i32 limit);
// Full-text search, newest first.
Slice<Line> db_search(sqlite3* db, Arena* a, Str query, i32 limit);
Slice<FileItem> db_files_between(sqlite3* db, Arena* a, f64 from, f64 to);
FileItem db_file(sqlite3* db, Arena* a, i64 id);   // id 0 if not found

struct DayCount {
    char day[11];  // "YYYY-MM-DD", the 04:00-to-04:00 day
    i32 lines;
};
// Lines heard per day, oldest first.
Slice<DayCount> db_days(sqlite3* db, Arena* a);

struct SpeakerCount {
    Str speaker;
    i64 lines;
    f64 last_ts;
};
// Every speaker ever heard, most lines first.
Slice<SpeakerCount> db_speakers(sqlite3* db, Arena* a);
