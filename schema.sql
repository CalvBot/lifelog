-- Shared by the old service (built in) and the Python worker. Must stay idempotent.
PRAGMA journal_mode = WAL;

CREATE TABLE IF NOT EXISTS segments (
    id       INTEGER PRIMARY KEY,
    start_ts REAL NOT NULL,   -- unix seconds (UTC)
    end_ts   REAL NOT NULL,
    source   TEXT NOT NULL,   -- 'mic' | 'app'
    speaker  TEXT NOT NULL,   -- 'me' for the mic; exe name for apps until phase 2 adds real speakers
    exe      TEXT,
    pid      INTEGER,
    title    TEXT,            -- window title when the audio started
    device   TEXT,            -- capture device name (mic only)
    text     TEXT NOT NULL,
    audio    TEXT,            -- the clip it came from, 'YYYY-MM-DD/<file>.wav' under <data_dir>/audio (kept a week)
    audio_offset REAL         -- seconds into that clip where the line starts
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
    summary    TEXT,              -- markdown
    reminders  TEXT               -- JSON array of {"time": "HH:MM", "text": "..."}
);

-- Topics the daily summary must never mention again (set from the summaries window).
CREATE TABLE IF NOT EXISTS summary_never (
    id         INTEGER PRIMARY KEY,
    topic      TEXT NOT NULL,
    created_ts REAL NOT NULL
);

CREATE TRIGGER IF NOT EXISTS segments_au AFTER UPDATE OF text ON segments BEGIN
    INSERT INTO segments_fts (segments_fts, rowid, text) VALUES ('delete', old.id, old.text);
    INSERT INTO segments_fts (rowid, text) VALUES (new.id, new.text);
END;
