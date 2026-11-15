// apps.h: every app lifelog knows of. One record per exe, and the one place its decisions live:
// recorded or not, game or not. The config file's lists are written from here.

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
    bool playing;        // making sound now
    i64 lines;           // lines heard, from the database
    f64 last_ts;         // when it was last heard
    u8* icon;            // 32×32 RGBA (malloc), or null if none was found
    u32 icon_version;    // bumped when `icon` changes, so the window makes a new texture
};

struct Apps {
    List<AppInfo> list;
    char game_running[64];  // a game exe running now, "" if none
    bool scanned;           // the scan of installed apps and games has finished
};

// The record for `exe`, or null.
AppInfo* app_find(App* app, Str exe);
// The record for `exe`, added if it is not there yet.
AppInfo* app_get(App* app, Str exe);

// Sets the decisions from the config's lists (once, at start).
void apps_from_config(App* app);
// Change a decision and save the config's lists.
void app_set_decision(App* app, Str exe, Decision d);
void app_set_game(App* app, Str exe, GameMark g);

// Starts the background scan of installed apps and games (Start menu, Steam, Epic, ...), the
// process watcher that keeps `running` and `game_running` current, and the icon cache.
void apps_start(App* app);
