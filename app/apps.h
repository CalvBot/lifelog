// apps.h: every app lifelog knows of. One record per exe, and the one place its decisions live:
// recorded or not, game or not. The config file's lists are written from here.
//
// The list holds apps in the sense of the Apps settings: those in the config's lists, those found
// installed (Start menu, game launchers), those that played sound, and those heard in the
// database. Windows' own background processes are not in it; their paths are in `paths` only.

#pragma once

enum Decision : u8 {
    DECISION_NEW,       // not decided yet: recorded meanwhile, listed under New
    DECISION_INCLUDED,  // keep recording
    DECISION_EXCLUDED,  // record nothing
};

enum GameMark : u8 {
    GAME_NO,
    GAME_YES,        // in auto mode, nothing is transcribed while it runs
    GAME_DISMISSED,  // "not a game": no longer offered as one, even if a launcher lists it
};

struct AppInfo {
    char exe[64];        // lower case: "discord.exe"
    char name[128];      // from the Start menu or the exe's version info; "" if not known
    char path[MAX_PATH]; // the exe's full path, if known
    Decision decision;
    GameMark game;
    bool running;        // has a process now
    bool playing;        // being recorded now (it played sound in the last 30 s)
    bool installed;      // found by the scan (Start menu or a game launcher)
    i64 lines;           // lines heard, from the database
    f64 last_ts;         // when it was last heard
    u8* icon;            // 32×32 RGBA (malloc), or null if none was found
    u32 icon_version;    // bumped when `icon` changes, so the window makes a new texture
    bool icon_tried;     // the icon was looked for (found or not), so it is not tried again
    bool icon_recheck;   // look again even if it has one: it started playing, and its exe may have
                         // been rebuilt with a new icon or name since
    bool name_from_exe;  // `name` is the exe's own description, so a changed exe may change it
};

// Where an exe lives, for every exe seen (not only apps). Kept in paths.txt between runs.
struct ExePath {
    char exe[64];
    char path[MAX_PATH];
};

struct Apps {
    List<AppInfo> list;
    char game_running[64];  // a game exe running now, "" if none
    volatile LONG game_on;  // game_running is set: an atomic other threads read (events.h)
    List<ExePath> paths;
};

// The record for `exe`, or null.
AppInfo* app_find(App* app, Str exe);
// The record for `exe`, added if it is not there yet.
AppInfo* app_get(App* app, Str exe);
// The full path of `exe` if it was ever seen, or null.
const char* app_path(App* app, Str exe);
// lifelog's own programs: never recorded (its window plays recordings back) and not listed.
bool is_own_exe(Str exe);
// How a speaker is shown: an app by its own name (from the Start menu or its exe's description,
// "Brave Browser"), else its exe without ".exe"; anything else ("me") as it is.
Str app_label(App* app, Str speaker);

// The apps' names, copied for a thread that names speakers (the summary writer, copying).
struct AppLabel {
    char exe[64];
    char name[128];
};
struct AppLabels {
    AppLabel* items;
    i32 count;
};
// The main thread: every app's name, into `a`.
AppLabels app_labels(App* app, Arena* a);
// A speaker as the app list shows it, as app_label does. Any thread.
Str label_of(AppLabels* labels, Str speaker);
// The full path of the exe process `pid` runs, into `out`. False if it can't be read.
bool proc_path(u32 pid, char* out, i32 cap);
// Notes that `exe` runs from `path` (the main thread), updating the app and paths.txt if it moved.
void app_learn_path(App* app, Str exe, Str path);

// Sets the decisions from the config's lists (once, at start).
void apps_from_config(App* app);
// Change a decision and save the config's lists.
void app_set_decision(App* app, Str exe, Decision d);
void app_set_game(App* app, Str exe, GameMark g);
// Removing an app from the lists forgets its choices (recorded, game) and hides it until it plays
// sound again, when app_unremove brings it back under New. Both save the config.
void app_remove(App* app, Str exe);
void app_unremove(App* app, Str exe);
bool app_removed(App* app, Str exe);

// Folder rules (capture.included_folders and excluded_folders): every app in the folder,
// subfolders too, unless the exe has a decision of its own; the deepest folder wins.
// Whether `path` is `folder` or inside it, ignoring case and / against \.
bool path_under(Str path, Str folder);
// The decision an app follows: its own, else its folder's, else new.
Decision app_decision(App* app, AppInfo* a);
// The rule set for exactly `folder`: DECISION_NEW if none.
Decision folder_decision(Config* c, Str folder);
// Sets the rule for `folder` (DECISION_NEW removes it) and saves the config's lists.
void app_set_folder(App* app, Str folder, Decision d);

// Starts the background scan of installed apps and games (Start menu, Steam, Epic, ...), the
// process watcher that keeps `running` and `game_running` current, and the icon cache.
void apps_start(App* app);

// ---------------------------------------------------------------------------------------------
// Processes and windows now, for the watchers. These take no lock and touch no App.

struct Proc {
    u32 pid;
    u32 parent;
    char exe[64];  // lower case
};
struct Procs {
    Proc* items;
    i32 count;
};
Procs processes(Arena* a);
Proc* proc_find(Procs* p, u32 pid);
// Browsers and launchers play sound from a child process, and process loopback records a whole
// tree, so this climbs to the topmost ancestor that is still the same program.
u32 proc_root(Procs* p, u32 pid);

struct Win {
    u32 pid;
    char title[256];
};
struct Wins {
    Win* items;
    i32 count;
};
// Visible top-level windows with a title, the foreground one first.
Wins windows_now(Arena* a);
// The title of the first window of a process named `exe`, or "".
Str title_for(Str exe, Procs* p, Wins* w);

// An exe's full path, read by the capture watcher the first time it sees the exe this run.
struct SeenPath {
    char exe[64];
    char path[MAX_PATH];
};
// The main thread: what the capture watcher's process scan saw (the one scan of
// the processes). Learns the new paths and sets which apps run and whether a game does.
void apps_seen(App* app, Procs* procs, SeenPath* fresh, i32 fresh_count);

// ---------------------------------------------------------------------------------------------
// The lifelog logo (icon.cpp), drawn in code so it is sharp at every size: a speech bubble with
// three sound-level bars. Blue while listening, grey with a pause badge in private mode.

// `size`×`size` BGRA pixels (straight alpha), top row first, into `out` (size*size*4 bytes).
void logo_render(u8* out, i32 size, bool paused);
// The logo as an icon, for the tray and the windows. The caller destroys it with DestroyIcon.
HICON logo_icon(i32 size, bool paused);
// The logo as a PNG file's bytes in `a` (Windows notifications want an image file).
Str logo_png(Arena* a, i32 size);
