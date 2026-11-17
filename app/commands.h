// commands.h: custom voice commands (docs/custom-commands.md). Each is a folder in
// %APPDATA%\lifelog\commands with a command.toml (how to run its script, the secrets it needs, and
// what to say for each action) and the script. A sentence after the wake word is matched against
// the phrases loosely (fillers dropped, words in order anywhere, {slots} take the words between),
// and the script gets the action and slots as JSON on stdin and answers with a JSON line. A
// command runs only once allowed in Settings, and again after its files change. Secrets are kept
// encrypted for the Windows account (DPAPI) in %LOCALAPPDATA%\lifelog\secrets.

#pragma once

static const i32 CMD_MAX = 32;      // commands
static const i32 CMD_ACTIONS = 16;  // actions in one command
static const i32 CMD_PHRASES = 48;  // phrases in one command, all actions together
static const i32 CMD_SECRETS = 8;

struct CmdPhrase {
    i32 action;
    char say[128];  // "play {song}"
};

struct CmdAction {
    char name[32];          // its [table] in command.toml: "play"
    char description[160];
};

struct CustomCommand {
    char id[48];            // the folder's name: "spotify"
    char name[64];          // "Spotify"
    char description[160];
    char run[256];          // "python spotify.py", run in `dir`
    wchar_t dir[MAX_PATH];
    char secrets[CMD_SECRETS][32];
    i32 secret_count;
    CmdAction actions[CMD_ACTIONS];
    i32 action_count;
    CmdPhrase phrases[CMD_PHRASES];
    i32 phrase_count;
    u64 hash;               // of the folder's files
    bool allowed;           // allowed with this hash
    bool changed;           // allowed before, with other files
    bool enabled;           // not switched off
    char error[200];        // command.toml could not be read: it never runs
};

// What a sentence matched.
struct CmdMatch {
    char id[48];
    char action[32];
    char slots[512];  // a JSON object: {"song":"bad guy"}
    char said[256];   // the command's words after the wake word, fillers and all
    char label[160];  // for the log and Settings: "spotify play: song = bad guy"
    i32 score;        // fixed words matched
};

struct Commands {
    SRWLOCK lock;  // guards everything below: the main thread writes, the voice thread reads
    CustomCommand items[CMD_MAX];
    i32 count;
    u64 version;   // bumped by each load
};
extern Commands g_commands;

// Main thread (or the CLI): reads the commands folder again.
void commands_load(Config* cfg);
// `w` is the sentence after the wake word, as lower-case words. With `leading`, only phrases that
// start the sentence count (they go before the built-in commands); without, any (after them).
// Only allowed, switched-on commands match.
bool commands_match(const Str* w, i32 n, bool leading, CmdMatch* out);
// The same for a typed sentence, without the wake word (Settings' test box, the CLI).
bool commands_match_text(Str text, CmdMatch* out);
// Runs a matched command's script on its own thread; `done` gets the outcome on that thread.
typedef void (*CmdDone)(App* app, const CmdMatch* m, bool ok, const char* message, const char* banner);
void commands_run(App* app, const CmdMatch* m, CmdDone done);
// Runs it here and waits (the CLI).
bool commands_run_here(const CmdMatch* m, char* message, i32 message_cap, char* banner, i32 banner_cap);

// Main thread: allow a command as its files are now, switch one on or off.
void commands_allow(App* app, const char* id);
void commands_enable(App* app, const char* id, bool on);
const wchar_t* commands_dir();

// Secrets, encrypted for this Windows account. Setting "" removes one.
bool secret_set(const char* id, const char* key, Str value);
bool secret_is_set(const char* id, const char* key);
