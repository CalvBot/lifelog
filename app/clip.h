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
    f64 seconds;       // for audio
    char error[256];
};

// Copies [from, to) to the clipboard, leaving out `exclude` (speakers). Unlocked: it reads the
// database and the audio files. Must run on a thread with a window (the clipboard needs one).
ClipResult clip_copy(App* app, f64 from, f64 to, Names* exclude, ClipKind kind);
// The CLI's and the API's "15m", "1h", "2d", "today". False if `range` is not one of those.
bool clip_range(Str range, f64 now, f64* from);

// "Paste here": saves the files, images, text or links on the clipboard into the timeline now.
// Returns how many were saved, or -1 with `error` set.
i32 files_paste(App* app, char* error, i32 cap);
void file_open(App* app, i64 id, bool in_folder);

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
    f64 play_ts;          // the moment being heard (unix seconds)
    void* player;         // the playback thread's own
};

// Starts playing from `line_id` (stops what was playing). `hidden` are speakers left out.
void play(App* app, i64 line_id, PlayMode mode, Names* hidden);
void play_stop(App* app);
