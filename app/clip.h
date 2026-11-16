// clip.h: copying a stretch of what was heard (as text, a .txt file, or audio), pasting files and
// links into the timeline, opening them, and playing kept audio back.

#pragma once

enum ClipKind : u8 {
    CLIP_FILE,   // a .txt put on the clipboard as a file, so it pastes straight into Discord
    CLIP_TEXT,   // the lines as text
    CLIP_AUDIO,  // the stretch mixed into one .wav, on the clipboard as a file
};

struct ClipResult {
    bool ok;
    i32 lines;
    f64 seconds;          // for audio
    char path[MAX_PATH];  // the .txt or .wav written to the temp folder (UTF-8)
    char error[256];
};

// Copies [from, to) to the clipboard, leaving out `exclude` (speakers; may be null), with each
// speaker named from `labels` (may be null). Slow: it reads the database and the audio files, and
// mixing audio can take a while. Any thread but the main one.
// `label` names the file "lifelog-<label>.txt"; by default it is the times ("2026-11-15-1400-to-1415").
ClipResult clip_copy(const Config* cfg, AppLabels* labels, f64 from, f64 to, Names* exclude, ClipKind kind,
                     const char* label = 0);
// The main thread: clip_copy on the pool, then `done(app, result, kind, ctx)` back on the main
// thread. Takes `exclude` (freed after).
typedef void ClipDone(App* app, ClipResult* r, ClipKind kind, i64 ctx);
void clip_copy_later(App* app, f64 from, f64 to, Names exclude, ClipKind kind, const char* label, ClipDone* done, i64 ctx);
// The CLI's and the API's "15m", "1h", "90s", "2d", "today". False if `range` is not one of those.
// `label` gets the file label ("last-15m", "today") when given.
bool clip_range(Str range, f64 now, f64* from, char* label = 0, i32 label_cap = 0);
// Puts text on the clipboard. Any thread. False if the clipboard stayed busy.
bool clipboard_set_text(Str text);

// "Paste here": reads the files, images, text or links on the clipboard now, and saves them into
// the timeline on the pool (then Storage.files_version goes up and it says "Saved N items").
// The main thread. Returns how many things were on the clipboard, or -1 with `error` set.
i32 files_paste(App* app, char* error, i32 cap);
// Opens a pasted file in its app (a link in the browser), or shows it in its folder. The main
// thread; the work is done on the pool and failures show as a toast.
void file_open(App* app, i64 id, bool in_folder);
// Takes a pasted item off the timeline, and deletes lifelog's copy of it (a link's original
// stays). The main thread; done on the database thread.
void file_delete(App* app, i64 id);
// Where a pasted file is on disk (its copy under data_dir\files, or what a link points at). False
// for a web address or a bad path. Does not check that the file is still there.
bool file_full_path(const Config* cfg, FileItem* f, wchar_t out[MAX_PATH]);

// Playback of kept audio (16 kHz mono).
enum PlayMode : u8 {
    PLAY_ALL_ON,       // everything heard, mixed, playing on (a plain click)
    PLAY_ALL_AT_LINE,  // everything heard while that line was said (Shift)
    PLAY_STREAM_ON,    // that line's app, playing on (Ctrl)
    PLAY_LINE,         // just that line (Ctrl+Shift)
};

struct Clips {
    // Playback, for the window: what is playing and where.
    i64 playing_line;     // 0 when nothing plays
    PlayMode play_mode;
    f64 play_ts;          // the moment being heard (unix seconds); 0 until the audio starts
    void* player;         // the playback thread's own
};

// Starts playing from `line_id` (stops what was playing). `hidden` are speakers left out (copied).
// The main thread; the audio is mixed and played on a thread of its own, and failures show as a
// toast.
void play(App* app, i64 line_id, PlayMode mode, Names* hidden);
void play_stop(App* app);
