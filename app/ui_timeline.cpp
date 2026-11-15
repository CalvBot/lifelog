// ui_timeline.cpp: the Timeline tab (port of ui/src/browser).

struct TimelineState {
    Arena arena;           // the shown day's lines and files
    char day[11];          // the day on screen
    u64 lines_version;     // App.worker.lines_version when they were loaded
    u64 files_version;
    bool loaded;
    Slice<Line> lines;
    Slice<FileItem> files;
};

static void timeline_load(App* app, TimelineState* s) {
    arena_reset(&s->arena);
    s->lines = {};
    s->files = {};
    if (app->storage.db) {
        f64 from, to;
        day_bounds(s->day, &from, &to);
        s->lines = db_lines_between(app->storage.db, &s->arena, from, to, 20000);
        s->files = db_files_between(app->storage.db, &s->arena, from, to);
    }
    s->lines_version = app->worker.lines_version;
    s->files_version = app->storage.files_version;
    s->loaded = true;
}

static void timeline_shift_day(TimelineState* s, i32 days) {
    f64 from, to;
    day_bounds(s->day, &from, &to);
    day_of(from + 12 * 3600 + days * 86400.0, s->day);
    s->loaded = false;
}

// Show the moment `ts` (and the line where `said` was said, if found).
static void timeline_jump(App* app, TimelineState* s, f64 ts, Str said) {
    (void)app; (void)said;
    day_of(ts, s->day);
    s->loaded = false;
}

static void timeline_draw(App* app, TimelineState* s, f32* next_frame_ms) {
    (void)next_frame_ms;
    if (!s->day[0]) today_day(s->day);
    if (!s->loaded || s->lines_version != app->worker.lines_version || s->files_version != app->storage.files_version)
        timeline_load(app, s);

    if (icon_button(ICON_CHEVRON_LEFT, "Day before", "prev")) timeline_shift_day(s, -1);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", page_name(s->day));
    ImGui::SameLine();
    if (icon_button(ICON_CHEVRON_RIGHT, "Day after", "next")) timeline_shift_day(s, 1);
    ImGui::SameLine();
    ImGui::TextDisabled("%d lines", s->lines.count);
    ImGui::Separator();

    if (!app->storage.db) {
        ImGui::TextWrapped("The database could not be opened. The log says why.");
        return;
    }
    ImGui::BeginChild("lines");
    for (i32 i = 0; i < s->lines.count; i++) {
        Line* l = &s->lines.items[i];
        if (!speaker_visible(app, l->speaker)) continue;
        ImGui::TextDisabled("%s", hm_text(l->start_ts));
        ImGui::SameLine();
        draw_app_icon(app, l->speaker);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(l->text.ptr, l->text.ptr + l->text.len);
        ImGui::PopTextWrapPos();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

static void timeline_free(TimelineState* s) {
    arena_release(&s->arena);
}
