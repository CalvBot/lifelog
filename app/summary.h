// summary.h: the daily summary and the reminders in it.
//
// At ask_at a notification asks whether to write today's summary (now, in an hour, later, skip).
// It is written by lifelog's own llama.cpp server (started only while it writes) or an
// OpenAI-compatible endpoint. Reminders ("I'll call Sam tomorrow") are offered as notifications
// and shown when due. Settings live in App.config.summary; the functions below change them there
// and in config.toml.
//
// Notifications (notify.cpp) are Windows toasts. Their buttons open `lifelog:` links, a link type
// registered to run lifelog.exe with the link; that second copy hands the link to the running one
// (notify_forward) and exits, and the running one acts on it (notify_open_link).

#pragma once

struct SummaryDay {
    char day[11];        // "YYYY-MM-DD"
    char status[12];     // "done" | "skipped"
    f64 created_ts;
    i32 reminders;       // how many "to do" points
};

struct Reminder {
    Str time;    // "HH:MM" when it was said
    Str text;    // the to-do
    Str said;    // my words
    // Its notification: none, asked, set (with when it will show), shown or no (declined).
    // Empty when it has none and said no time.
    Str alert_state;
    Str alert_when;  // "tomorrow at 09:00"
    f64 alert_due;   // when it shows (or would), unix seconds; 0 if not known
};

struct StoredSummary {
    bool found;
    char day[11];
    Str status;
    f64 created_ts;
    Str model;
    Str markdown;
    Reminder* reminders;
    i32 reminder_count;
};

struct NeverTopic {
    i64 id;
    char topic[128];
};

// A notification waiting for the notify thread to show it (or take it away).
struct ToastButton {
    char text[64];
    char link[160];
};
struct ToastJob {
    bool remove;          // take away the toast with `tag` instead of showing one
    char tag[32];         // the same tag replaces the earlier toast
    char title[256];
    char body[1024];
    ToastButton buttons[4];
    i32 button_count;
    char launch[160];     // the link clicking the toast itself opens; "" for none
    bool sticky;          // stays on screen until answered
};

struct Notify {
    SRWLOCK lock;            // guards `queue`
    List<ToastJob> queue;
    HWND window;             // the notify thread's message window, which links are sent to
};

// What the summary threads (and llm.cpp) need of the settings.
struct SummaryOptions {
    bool enabled;
    char ask_at[8];       // "HH:MM"
    u32 max_words;
    bool ask_reminders;
    char model[128];
    char endpoint[512];
    char api_key[256];
    char ai_dir[MAX_PATH];
};

struct Summary {
    char phase[160];      // what it is doing: "", "asks at 21:00", "writing: notes for 3 of 5 parts", ...
    bool writing;
    char writing_day[11];
    List<SummaryDay> days;      // every day with a summary, newest first; read once at start, then
                                // kept up to date from what each change reports
    List<NeverTopic> never;     // topics never to mention
    bool lists_ready;           // `days` and `never` have been read
    u64 version;                // bumped when a summary, a reminder or the never list changes
    // The summary thread's own:
    f64 snooze_ts;              // no asking before this (after "in an hour" or "delay until")
    f64 countdown_ts;           // the "starts in 5 minutes" toast is up: writing starts at this moment
    char countdown_day[11];
    f64 last_game_ts;           // a game was running or the GPU was busy then
    Notify notify;
    Latest phase_next;          // the summary threads' newest phase, for the main thread (events.h)
    Latest tick;                // the clock's newest reading (summary_tick)
    // The options as the main thread last handed them over (summary_options_publish).
    SRWLOCK options_lock;
    SummaryOptions options;
    HANDLE wake;                // auto-reset: the summary's clock looks again
    // The earliest moment a set reminder is due, in unix milliseconds (INT64_MAX: none). The
    // database thread lowers it when it sets one; the clock opens the database only then.
    volatile LONG64 next_alert_ms;
};

// What summary_act did.
enum SummaryActed : u8 {
    ACTED_WRITING,
    ACTED_ALREADY_WRITING,
    ACTED_LATER,      // asks again in an hour (`at`)
    ACTED_UNTIL,      // asks again at `at`
    ACTED_SKIPPED,    // saved on the database thread; the window says if that fails
    ACTED_PICKING,    // the delay box is open
    // Failures:
    ACTED_BAD_DAY,
    ACTED_BAD_TIME,
    ACTED_BAD_ACTION,
};
struct SummaryActResult {
    SummaryActed what;
    f64 at;  // ACTED_LATER, ACTED_UNTIL: when it asks again
};
inline bool acted_failed(SummaryActResult r) { return r.what >= ACTED_BAD_DAY; }
// For a message: "writing", "asking again at 23:30", "skipped", or what was wrong.
const char* summary_acted_text(SummaryActResult r, char* out, i32 cap);

void summary_start(App* app);
// Hands the summary threads the settings in App.config now, and wakes the clock. The setters
// below call it; so does anything else that changes what they use (the AI folder). The main
// thread (or the CLI, before it calls summary_check_ai).
void summary_options_publish(App* app);
// A copy of the options as last handed over. Any thread.
SummaryOptions summary_options(App* app);

// The notification's and the window's buttons. `action`: "now", "later" (in an hour), "until"
// (with `at`, "HH:MM"), "skip", "pick" (opens the delay box). `day` defaults to today.
SummaryActResult summary_act(App* app, const char* action, const char* day, const char* at);

void summary_set_enabled(App* app, bool on);
void summary_set_ask_at(App* app, Str hhmm);
void summary_set_max_words(App* app, u32 words);
void summary_set_ask_reminders(App* app, bool on);

// summary-prompt.txt next to config.toml: the AI's instructions (tone, what to focus on). No file,
// or an empty one, means the built-in text. summary_prompt_file writes the built-in text there if
// the file is missing, to edit; null (saying why) if it could not.
const wchar_t* summary_prompt_path();
const wchar_t* summary_prompt_file(App* app);

// One day's summary, with each reminder's notification state. Any thread.
StoredSummary summary_get(sqlite3* db, Arena* a, const char* day);

// Takes a point out of a day's summary (it comes back if the day is rewritten). `bullet` is the
// line's text; `reminder` is an index into its reminders, or -1.
void summary_dismiss(App* app, const char* day, Str bullet, i32 reminder);
// Never mention `topic` again; with a day and bullet, also takes that point out.
void summary_add_never(App* app, Str topic, const char* day, Str bullet);
void summary_remove_never(App* app, i64 id);

// The journal's bell: notify about reminder `n` of `day` when it is due (or tomorrow at 09:00).
// The window then says when it will show ("tomorrow at 09:00").
//
// These change the database on its thread (db_post) and return at once. Each reports what it
// did (the row, topic or due time it changed); the main thread updates Summary's lists from that
// and the window says what happened. The main thread.
void reminder_set(App* app, const char* day, i32 n, bool on, bool quiet = false);
// A reminder notification's buttons: "yes", "no", "done", "snooze".
void reminder_answer(App* app, i64 id, const char* action);

// For the CLI's check-ai: starts the AI, asks it one thing, stops it.
bool summary_check_ai(App* app, char* said, i32 cap);

// Notifications. Any thread; notify_show and notify_remove only queue the work.
void notify_show(App* app, ToastJob* toast);
void notify_remove(App* app, const char* tag);
// A `lifelog:` link (a notification's button) in the running app, on the main thread: does what
// it says.
bool notify_open_link(App* app, Str link);
// In a second copy of lifelog started with a link: hands it to the running one. False if none runs.
bool notify_forward(const wchar_t* link);
