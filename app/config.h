// config.h: the settings, from %APPDATA%\lifelog\config.toml.
//
// Each setting is saved on its own when it changes (config_save_value and the helpers below),
// leaving the rest of the file and its comments as they were.

#pragma once

enum Mode : i32 {
    MODE_AUTO,    // wait while a game uses the GPU, catch up afterwards
    MODE_ALWAYS,  // transcribe no matter what
    MODE_PAUSED,  // only capture
};
const char* mode_name(Mode m);
bool mode_parse(Str s, Mode* out);

// How the chimes sound (chime.h): three sets, or none. Saved by name, so the order may change.
enum ChimeStyle : i32 {
    CHIME_GLASS,  // a warm bell, like an electric piano
    CHIME_SOFT,   // a low marimba
    CHIME_SYNTH,  // a mellow synth: detuned saws, a filter, a sine below
    CHIME_OFF,
};
const char* chime_style_name(ChimeStyle s);
bool chime_style_parse(Str s, ChimeStyle* out);

struct Config {
    char data_dir[MAX_PATH];
    u16 api_port;

    struct {
        bool mic;
        // The mic to record: its Windows device id, "" for the Windows default (followed when it
        // changes). The name is only shown, for when the device is not plugged in.
        char mic_device[256];
        char mic_name[128];
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
        Mode before_pause;     // the mode "continue transcription" goes back to (mode_before_pause)
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
        char dir[MAX_PATH];     // where whisper\ and llm\ are (setup.h); %LOCALAPPDATA%\lifelog by default
        char cleanup[2048];     // folders to delete at the next start, separated by '|' (setup_cleanup)
    } ai;

    struct {
        ChimeStyle style;
        u32 volume;  // 0-100
    } chimes;

    struct {
        bool listen;    // voice commands: listen on the mic for the wake word (voice.h)
        char wake[64];  // "lifelog"
        bool backup;    // while the transcriber's model is not loaded, listen with a small one
        char model[32]; // that one, on the processor: tiny.en, base.en (default) or small.en
    } voice;

    struct {
        Names allowed;   // custom commands allowed to run: "folder:hash" (commands.h)
        Names disabled;  // folders switched off
    } commands;

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

// `dir` (UTF-8) and `name` joined with a backslash.
void path_under(const char* dir, const wchar_t* name, wchar_t out[MAX_PATH]);
// data_dir and api_port as config_load read them. They do not change while the program runs, so
// any thread may use them without a copy.
const char* data_dir();
u16 api_port();
// The paths under data_dir.
void spool_dir(wchar_t out[MAX_PATH]);
void audio_dir(wchar_t out[MAX_PATH]);
void files_dir(wchar_t out[MAX_PATH]);
void db_path(wchar_t out[MAX_PATH]);
