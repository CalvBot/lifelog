// summary.h: the daily summary and the reminders in it.
//
// At ask_at a notification asks whether to write today's summary (now, in an hour, later, skip).
// It is written by lifelog's own llama.cpp server (started only while it writes) or an
// OpenAI-compatible endpoint. Reminders ("I'll call Sam tomorrow") are offered as notifications
// and shown when due. Settings live in App.config.summary; the functions below change them there
// and in config.toml.

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
    Str when;    // "YYYY-MM-DD HH:MM" if a time was said, else empty
    // Its notification: none, asked, set (with when it will show), shown or declined.
    Str alert_state;
    Str alert_when;  // "tomorrow at 09:00"
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

struct Summary {
    char phase[160];      // what it is doing: "", "asks at 21:00", "writing: notes for 3 of 5 parts", ...
    bool writing;
    char writing_day[11];
    List<SummaryDay> days;      // every day with a summary, newest first
    List<NeverTopic> never;     // topics never to mention
    u64 version;                // bumped when a summary, a reminder or the never list changes
    // The summary thread's own:
    f64 ask_ts;                 // when it will next ask
    char ask_day[11];
};

void summary_start(App* app);

// The notification's and the window's buttons. `action`: "now", "later" (in an hour), "until"
// (with `at`, "HH:MM"), "skip", "pick" (opens the delay box). `day` defaults to today.
// Returns what happened, for a message: "writing", "at 23:30", "skipped".
Str summary_act(App* app, Arena* a, const char* action, const char* day, const char* at);

void summary_set_enabled(App* app, bool on);
void summary_set_ask_at(App* app, Str hhmm);
void summary_set_max_words(App* app, u32 words);
void summary_set_ask_reminders(App* app, bool on);

// One day's summary, with each reminder's notification state. Unlocked is fine.
StoredSummary summary_get(sqlite3* db, Arena* a, const char* day);

// Takes a point out of a day's summary (it comes back if the day is rewritten). `bullet` is the
// line's text; `reminder` is an index into its reminders, or -1.
void summary_dismiss(App* app, const char* day, Str bullet, i32 reminder);
// Never mention `topic` again; with a day and bullet, also takes that point out.
void summary_add_never(App* app, Str topic, const char* day, Str bullet);
void summary_remove_never(App* app, i64 id);

// The journal's bell: notify about reminder `n` of `day` when it is due (or tomorrow at 09:00).
// Returns when it will show ("tomorrow at 09:00"), or empty when turned off.
Str reminder_set(App* app, Arena* a, const char* day, i32 n, bool on);
// A reminder notification's buttons: "yes", "no", "done", "snooze".
void reminder_answer(App* app, i64 id, const char* action);

// For the CLI's check-ai: starts the AI, asks it one thing, stops it. Unlocked.
bool summary_check_ai(App* app, char* said, i32 cap);
