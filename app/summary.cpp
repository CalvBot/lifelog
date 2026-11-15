// summary.cpp: see summary.h. To come.

void summary_start(App* app) { (void)app; }
Str summary_act(App* app, Arena* a, const char* action, const char* day, const char* at) {
    (void)app; (void)day; (void)at;
    return str_fmt(a, "%s: not built yet", action);
}
void summary_set_enabled(App* app, bool on) { app->config.summary.enabled = on; config_save_bool("summary", "enabled", on); }
void summary_set_ask_at(App* app, Str hhmm) { SET(app->config.summary.ask_at, hhmm); config_save_string("summary", "ask_at", app->config.summary.ask_at); }
void summary_set_max_words(App* app, u32 words) { app->config.summary.max_words = words; config_save_u32("summary", "max_words", words); }
void summary_set_ask_reminders(App* app, bool on) { app->config.summary.ask_reminders = on; config_save_bool("summary", "ask_reminders", on); }
StoredSummary summary_get(sqlite3* db, Arena* a, const char* day) { (void)db; (void)a; (void)day; return {}; }
void summary_dismiss(App* app, const char* day, Str bullet, i32 reminder) { (void)app; (void)day; (void)bullet; (void)reminder; }
void summary_add_never(App* app, Str topic, const char* day, Str bullet) { (void)app; (void)topic; (void)day; (void)bullet; }
void summary_remove_never(App* app, i64 id) { (void)app; (void)id; }
Str reminder_set(App* app, Arena* a, const char* day, i32 n, bool on) { (void)app; (void)a; (void)day; (void)n; (void)on; return {}; }
void reminder_answer(App* app, i64 id, const char* action) { (void)app; (void)id; (void)action; }
bool summary_check_ai(App* app, char* said, i32 cap) { (void)app; snprintf(said, cap, "not built yet"); return false; }
