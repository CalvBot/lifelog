// ui_journal.cpp: the Journal tab (port of ui/src/days).

struct JournalState {
    char day[11];  // the day shown, "" for the newest
};

static void journal_show_day(JournalState* s, const char* day) {
    SET(s->day, str(day ? day : ""));
}

static void journal_draw(App* app, JournalState* s, f32* next_frame_ms) {
    (void)s; (void)next_frame_ms;
    ImGui::TextWrapped("%s", app->summary.phase[0] ? app->summary.phase : "No summary written yet.");
}

static void journal_free(JournalState* s) { (void)s; }
