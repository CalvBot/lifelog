-- Loaded by lifelog at start. Must stay idempotent.
PRAGMA journal_mode = WAL;

-- One row per recorded clip that had words in it. Positions are sample counts at sample_rate
-- since 1970 (UTC), taken from the capture device's clock.
CREATE TABLE IF NOT EXISTS recordings (
    id          INTEGER PRIMARY KEY,
    file        TEXT NOT NULL,     -- 'YYYY-MM-DD/<file>.wav' under <data_dir>/audio (deleted after capture.keep_audio_days)
    sample_rate INTEGER NOT NULL,  -- 16000
    start       INTEGER NOT NULL,  -- the position of its first sample
    length      INTEGER NOT NULL,  -- in samples
    source      TEXT NOT NULL,     -- as its lines'
    speaker     TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS recordings_start ON recordings (start);

CREATE TABLE IF NOT EXISTS segments (
    id       INTEGER PRIMARY KEY,
    start_ts REAL NOT NULL,   -- unix seconds (UTC)
    end_ts   REAL NOT NULL,
    source   TEXT NOT NULL,   -- 'mic' | 'app'
    speaker  TEXT NOT NULL,   -- 'me' for the mic; exe name for apps until phase 2 adds real speakers
    exe      TEXT,            -- exe and pid: kept as a record; lifelog does not read them back
    pid      INTEGER,
    title    TEXT,            -- window title when the audio started
    device   TEXT,            -- capture device name (mic only); a record, not read back
    text     TEXT NOT NULL,
    recording    INTEGER,     -- the recording it was heard in (NULL for lines from before audio was kept)
    start_sample INTEGER,     -- where it is in that recording, in samples from its start; start_ts and
    end_sample   INTEGER,     -- end_ts are these as unix seconds, kept for searching by time
    words        BLOB         -- when each word was said: per word, four little-endian int32s, its text as
                              -- bytes [from, to) of text, its sound as samples [start, end) from start_sample
);
CREATE INDEX IF NOT EXISTS segments_start ON segments (start_ts);

CREATE VIRTUAL TABLE IF NOT EXISTS segments_fts USING fts5 (text, content = 'segments', content_rowid = 'id');

CREATE TRIGGER IF NOT EXISTS segments_ai AFTER INSERT ON segments BEGIN
    INSERT INTO segments_fts (rowid, text) VALUES (new.id, new.text);
END;
CREATE TRIGGER IF NOT EXISTS segments_ad AFTER DELETE ON segments BEGIN
    INSERT INTO segments_fts (segments_fts, rowid, text) VALUES ('delete', old.id, old.text);
END;
-- One row per day (the day runs 04:00 to 04:00 local time, so late nights count as the day before).
CREATE TABLE IF NOT EXISTS summaries (
    day        TEXT PRIMARY KEY,  -- 'YYYY-MM-DD'
    status     TEXT NOT NULL,     -- 'done' | 'skipped'
    created_ts REAL NOT NULL,
    model      TEXT,
    summary    TEXT,              -- markdown, as shown (made from summary_items)
    reminders  TEXT               -- JSON array of {"time": "HH:MM", "said": "...", "text": "...", "when": "YYYY-MM-DD HH:MM"},
                                  -- as the model wrote them (summary_items is what is shown)
);

-- Each summary's lines and reminders, in order: what the journal shows, and what its buttons
-- (dismiss, never mention, remind me, show in timeline) act on, by id. Written with the summary;
-- a rewrite replaces the day's rows, so dismissed ones come back.
CREATE TABLE IF NOT EXISTS summary_items (
    id        INTEGER PRIMARY KEY,
    day       TEXT NOT NULL,     -- the summary's day, 'YYYY-MM-DD'
    pos       INTEGER NOT NULL,  -- its place in the day
    kind      TEXT NOT NULL,     -- 'heading' | 'point' | 'text' | 'reminder'
    text      TEXT NOT NULL,     -- markdown (bold) for a heading, point or text; the to-do of a reminder
    ts        REAL,              -- unix seconds of the moment it is about (a point's "(14:05)", when a reminder was said)
    said      TEXT,              -- a reminder: my words
    said_time TEXT,              -- a reminder: 'HH:MM'
    due_ts    REAL,              -- a reminder: when it said it is due, if it did
    dismissed INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS summary_items_day ON summary_items (day, pos);

-- Files pasted into the timeline from the tray ("Paste here"); copies kept under <data_dir>/files.
CREATE TABLE IF NOT EXISTS files (
    id    INTEGER PRIMARY KEY,
    ts    REAL NOT NULL,      -- unix seconds: the moment it was pasted
    name  TEXT NOT NULL,      -- original file name
    path  TEXT NOT NULL,      -- 'YYYY-MM-DD/<file>' under <data_dir>/files; for a link, the full path or URL
    kind  TEXT NOT NULL,      -- 'image' | 'video' | 'audio' | 'text' | 'file' | 'folder' | 'url'
    size  INTEGER NOT NULL,
    link  INTEGER NOT NULL DEFAULT 0  -- 1: points at the original (a pasted path or URL), no copy kept
);
CREATE INDEX IF NOT EXISTS files_ts ON files (ts);

-- Reminders from summaries that pop up as a notification when due.
CREATE TABLE IF NOT EXISTS alerts (
    id        INTEGER PRIMARY KEY,
    day       TEXT NOT NULL,     -- the summary's day, 'YYYY-MM-DD'
    text      TEXT NOT NULL,     -- the to-do, as in the summary
    said      TEXT,              -- my words
    said_time TEXT,              -- 'HH:MM'
    due_ts    REAL NOT NULL,     -- unix seconds
    state     TEXT NOT NULL,     -- 'asked' | 'set' | 'shown' | 'no'
    item      INTEGER,           -- the reminder's summary_items id (added later: see db_init)
    UNIQUE (day, text)
);

-- Topics the daily summary must never mention again (set from the summaries window).
CREATE TABLE IF NOT EXISTS summary_never (
    id         INTEGER PRIMARY KEY,
    topic      TEXT NOT NULL,
    created_ts REAL NOT NULL   -- a record; not read back
);

CREATE TRIGGER IF NOT EXISTS segments_au AFTER UPDATE OF text ON segments BEGIN
    INSERT INTO segments_fts (segments_fts, rowid, text) VALUES ('delete', old.id, old.text);
    INSERT INTO segments_fts (rowid, text) VALUES (new.id, new.text);
END;
