// config.h: the settings, from %APPDATA%\lifelog\config.toml.
//
// The app lists (included, excluded, games, not games) live in App.apps once loaded; config_save
// writes them back from there.

#pragma once

enum Mode : i32 {
    MODE_AUTO,    // wait while a game uses the GPU, catch up afterwards
    MODE_ALWAYS,  // transcribe no matter what
    MODE_PAUSED,  // only capture
};
const char* mode_name(Mode m);
bool mode_parse(Str s, Mode* out);

struct Config {
    char data_dir[MAX_PATH];
    u16 api_port;

    struct {
        bool mic;
        bool apps;
        Names included_apps;
        Names excluded_apps;
        // Apps removed from the Settings lists: not listed until they play sound again.
        Names removed_apps;
        // Every app in these folders (subfolders too) is included or excluded, unless the exe has
        // its own decision; the deepest folder wins.
        Folders included_folders;
        Folders excluded_folders;
        f32 gate_rms;
        u32 keep_audio_days;
    } capture;

    struct {
        Mode mode;
        u32 gpu_busy_percent;
        bool pause_for_games;  // in auto mode, wait while a game (processing.games) runs
        u32 resume_after_secs;
        Names games;
        Names not_games;
    } processing;

    struct {
        bool enabled;
        char ask_at[8];  // "HH:MM"
        u32 max_words;
        char model[128];
        char endpoint[512];  // an OpenAI-compatible ".../v1" to use instead of lifelog's own llama.cpp
        char api_key[256];
        bool ask_reminders;
    } summary;

    struct {
        char model[128];     // ggml-<model>.bin for our whisper.cpp; or the name your endpoint knows
        char language[16];   // "en"; empty: detected per clip
        char endpoint[512];  // an OpenAI-compatible ".../v1" to use instead of lifelog's own whisper.cpp
        char api_key[256];
    } worker;

    struct {
        Names hidden;        // speakers hidden in the timeline (and left out of copies and playback)
        bool show_dismissed;  // show the lines of excluded apps
    } ui;
};

const wchar_t* config_path();
// Loads the config, writing the default file first if there is none. False (with `error` set) if
// the file cannot be read or parsed.
bool config_load(Config* cfg, char* error, i32 error_cap);

// Sets one key in the file, leaving everything else (and the comments) as it is. `value` is
// already TOML: "\"21:00\"", "250", "true", "[\"a.exe\", \"b.exe\"]". The file writer
// (disk.h) does it, so these return at once; true means it was asked for.
bool config_save_value(const char* table, const char* key, Str value);
bool config_save_names(const char* table, const char* key, Names* names);
bool config_save_folders(const char* table, const char* key, Folders* folders);
bool config_save_bool(const char* table, const char* key, bool on);
bool config_save_u32(const char* table, const char* key, u32 n);
bool config_save_string(const char* table, const char* key, const char* s);
// The write itself, on the file writer's thread (or at once, when it is not running).
bool config_write_value(const char* table, const char* key, Str value);

// The paths under data_dir.
void spool_dir(const Config* cfg, wchar_t out[MAX_PATH]);
void audio_dir(const Config* cfg, wchar_t out[MAX_PATH]);
void files_dir(const Config* cfg, wchar_t out[MAX_PATH]);
void db_path(const Config* cfg, wchar_t out[MAX_PATH]);
