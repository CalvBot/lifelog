// summary.h: the daily summary and the reminders in it.
//
// At ask_at a notification says today's summary starts in 5 minutes (start now, in an hour,
// delay until, skip today).
// It is written by lifelog's own llama.cpp server (started only while it writes) or an
// OpenAI-compatible endpoint. Reminders ("I'll call Sam tomorrow") are offered as notifications
// and shown when due. Settings live in App.config.summary; the functions below change them there
// and in config.toml.
//
// Notifications (notify.cpp) are lifelog's own toast windows. Their buttons open `lifelog:` links,
// which the running app acts on (notify_open_link). The link type is also registered to run
// lifelog.exe with the link; that second copy hands it to the running one (notify_forward) and exits.

#pragma once

struct SummaryDay {
    char day[11];        // "YYYY-MM-DD"
    char status[12];     // "done" | "skipped"
    i32 reminders;       // how many "to do" points
};

// A summary's line, as the journal shows it (summary_items).
enum ItemKind : u8 { ITEM_HEADING, ITEM_POINT, ITEM_TEXT, ITEM_REMINDER };
struct SummaryItem {
    i64 id;
    ItemKind kind;   // not ITEM_REMINDER: those are StoredSummary.reminders
    Str text;        // markdown
    f64 ts;          // the moment it is about, for "show in timeline"; 0 if none
};

struct Reminder {
    i64 id;      // its summary_items id
    f64 ts;      // when it was said; 0 if not known
    Str time;    // "HH:MM" when it was said
    Str text;    // the to-do
    Str said;    // my words
    bool alert_set;  // its notification is set to show (at alert_due)
    Str alert_when;  // "tomorrow at 09:00"
    f64 alert_due;   // when it shows (or would), unix seconds; 0 if not known
};

struct StoredSummary {
    bool found;
    Str status;
    f64 created_ts;
    Str model;
    SummaryItem* items;      // the lines not dismissed, in order
    i32 item_count;
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
enum ToastIcon { TOAST_ICON_SUMMARY, TOAST_ICON_REMINDER, TOAST_ICON_ERROR, TOAST_ICON_VOICE };
struct ToastJob {
    bool remove;          // take away the toast with `tag` instead of showing one
    char tag[32];         // the same tag replaces the earlier toast
    char title[256];
    char body[1024];
    ToastButton buttons[4];
    i32 button_count;
    char launch[160];     // the link clicking the toast itself opens; "" for none
    bool question;        // asks something: stays up longer before it goes
    ToastIcon icon;       // the tile's icon; an error's is red and stays a little longer
};

struct Notify {
    SRWLOCK lock;            // guards `queue`
    List<ToastJob> queue;
    HWND window;             // the notify thread's message window, which links are sent to
};

// What a summary can be for. Its text says what to keep and what matters most; it goes into the
// prompts in place of the general one. The rules about who said what stay the same for all.
#define FOCUS_LEAD "What to keep, most important first:"  // the words before the focus text in the prompts
struct SummaryFocus {
    const char* key;   // in config.toml
    const char* name;  // in Settings
    const char* text;
};
static const SummaryFocus SUMMARY_FOCUSES[] = {
    {"general", "General",
     "what you did, made, planned, promised or decided, what you told people, and the main news or stories people "
     "told you. Skip small talk and bare reactions."},
    {"emotion", "Emotion",
     "how the day felt: moments that made you or the people you talked with happy, stressed, proud, sad, annoyed or "
     "close, and what caused them. Name a feeling only when someone's own words show it; never guess one."},
    {"business", "Business",
     "work and study: tasks done, problems and how they were solved, decisions, meetings, what people asked of you, "
     "deadlines and next steps. Skip personal chat unless it affects the work."},
    {"memory", "Memory",
     "the details that are easy to forget: things to do, promises, dates, times, names, numbers, places, things to "
     "buy or send, and what people asked you for. Keep each detail exact."},
    {"mood", "Mood",
     "what made the day good or bad: wins, setbacks, fun, stress, rest, arguments and kind moments, and whether this "
     "part of the day leaned good, bad or mixed, judged only from what was said."},
};

// What the summary threads (and llm.cpp) need of the settings.
struct SummaryOptions {
    u32 max_words;
    bool ask_reminders;
    char model[128];
    char endpoint[512];
    char api_key[256];
    char ai_dir[MAX_PATH];
    char focus[16];
    char focus_text[1024];
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
    // Settings' "Write with the AI" (summary_write_focus): the wish it was given, then its draft or
    // why it failed. The main thread's, except `wish` while `focus_writing` (the writer reads it).
    bool focus_writing;
    char focus_wish[512];
    char focus_draft[1024];
    char focus_error[256];
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
// `day`'s entry in Summary.days, or null. The main thread.
const SummaryDay* summary_day_find(App* app, const char* day);

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

// What the summary is for: a SUMMARY_FOCUSES key, or "custom" with `text` (made one paragraph).
void summary_set_focus(App* app, Str key, Str text);
// The focus text options `o` ask for: a preset's, or the custom one (General's while it is empty).
const char* summary_focus_text(const SummaryOptions* o);
// Settings' "Write with the AI": the summary AI turns `wish` ("I want to see if my days are
// getting better") into a focus paragraph, on its own thread. It lands in Summary.focus_draft, or
// focus_error says why not. The main thread.
void summary_write_focus(App* app, Str wish);

// One day's summary, with each reminder's notification state. Any thread.
StoredSummary summary_get(sqlite3* db, Arena* a, const char* day);

// Takes a line or reminder (`item`, a SummaryItem or Reminder id) out of a day's summary; it comes
// back if the day is rewritten. A reminder's notification goes off with it.
void summary_dismiss(App* app, const char* day, i64 item);
// Never mention `topic` again; with a day and item, also takes that line out.
void summary_add_never(App* app, Str topic, const char* day, i64 item);
void summary_remove_never(App* app, i64 id);

// The journal's bell: notify about reminder `item` of `day` when it is due (or tomorrow at 09:00).
// The window then says when it will show ("tomorrow at 09:00").
//
// These change the database on its thread (db_post) and return at once. Each reports what it
// did (the row, topic or due time it changed); the main thread updates Summary's lists from that
// and the window says what happened. The main thread.
void reminder_set(App* app, const char* day, i64 item, bool on);
// A reminder notification's buttons: "yes", "no", "done", "snooze".
void reminder_answer(App* app, i64 id, const char* action);

// For the CLI's check-ai: starts the AI, asks it one thing, stops it.
bool summary_check_ai(App* app, char* said, i32 cap);
// Voice commands: a reminder that shows at `due` (unix seconds), as the summary's do. `said` is
// the sentence it came from. Any thread but the main one.
bool reminder_add(App* app, Str text, Str said, f64 due, char* error, i32 cap);
// Voice commands: a few sentences about `transcript` (clip_text's lines) from the summary AI,
// into `a`. Slow (it may start the AI). Any thread but the main one.
bool summary_quick(App* app, Arena* a, Str transcript, Str* out, char* error, i32 cap);

// Notifications. Any thread; notify_show and notify_remove only queue the work.
void notify_show(App* app, ToastJob* toast);
void notify_remove(App* app, const char* tag);
// A `lifelog:` link (a notification's button) in the running app, on the main thread: does what
// it says.
bool notify_open_link(App* app, Str link);
// In a second copy of lifelog started with a link: hands it to the running one. False if none runs.
bool notify_forward(const wchar_t* link);
// Every kind of toast drawn into a .bmp at `path`, dark and light, at `scale` (1 = 100%).
bool notify_preview(const wchar_t* path, f32 scale);
