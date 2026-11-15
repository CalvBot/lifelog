// ui.cpp: see ui.h. Each frame draws from App directly; the only copies kept are the lines of the
// day on screen, reloaded when the worker writes new ones (App.worker.lines_version).

struct UiState {
    Arena day_arena;          // the shown day's lines and files
    char day[11];             // the day on screen
    u64 lines_version;        // App.worker.lines_version when they were loaded
    u64 files_version;
    bool loaded;
    Slice<Line> lines;
    Slice<FileItem> files;
    char search[256];
};

void ui_window_opened(App* app) {
    UiState* s = (UiState*)calloc(1, sizeof(UiState));
    CHECK(s, "memory for the window's state");
    char today[11];
    day_of(now_ts(), today);
    SET(s->day, str(app->ui.want_day[0] ? app->ui.want_day : today));
    app->ui.state = s;
}

void ui_window_closed(App* app) {
    UiState* s = app->ui.state;
    if (!s) return;
    arena_release(&s->day_arena);
    free(s);
    app->ui.state = 0;
}

static void load_day(App* app, UiState* s) {
    arena_reset(&s->day_arena);
    s->lines = {};
    s->files = {};
    if (app->storage.db) {
        f64 from, to;
        day_bounds(s->day, &from, &to);
        s->lines = db_lines_between(app->storage.db, &s->day_arena, from, to, 20000);
        s->files = db_files_between(app->storage.db, &s->day_arena, from, to);
    }
    s->lines_version = app->worker.lines_version;
    s->files_version = app->storage.files_version;
    s->loaded = true;
}

static void shift_day(UiState* s, i32 days) {
    f64 from, to;
    day_bounds(s->day, &from, &to);
    day_of(from + 12 * 3600 + days * 86400.0, s->day);
    s->loaded = false;
}

static void draw_timeline(App* app, UiState* s) {
    if (!s->loaded || s->lines_version != app->worker.lines_version || s->files_version != app->storage.files_version)
        load_day(app, s);

    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) shift_day(s, -1);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", s->day);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) shift_day(s, 1);
    ImGui::SameLine();
    char today[11];
    day_of(now_ts(), today);
    if (strcmp(today, s->day) != 0 && ImGui::Button("Today")) {
        SET(s->day, str(today));
        s->loaded = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d lines", s->lines.count);
    ImGui::Separator();

    if (!app->storage.db) {
        ImGui::TextWrapped("The database could not be opened. The log says why.");
        return;
    }
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None);
    {
        for (i32 i = 0; i < s->lines.count; i++) {
            Line* l = &s->lines.items[i];
            if (names_has(&app->config.ui.hidden, l->speaker)) continue;
            LocalTime t = local_time(l->start_ts);
            ImGui::TextDisabled("%02d:%02d", t.hour, t.minute);
            ImGui::SameLine();
            ImVec4 who = l->source == SOURCE_MIC ? ImVec4(0, 0.47f, 0.83f, 1) : ImVec4(0.35f, 0.35f, 0.35f, 1);
            ImGui::TextColored(who, "%.*s", (int)l->speaker.len, l->speaker.ptr);
            ImGui::SameLine();
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(l->text.ptr, l->text.ptr + l->text.len);
            ImGui::PopTextWrapPos();
        }
    }
    // Stay at the bottom while new lines arrive, unless scrolled up.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

static void draw_journal(App* app, UiState* s) {
    (void)s;
    ImGui::TextWrapped("%s", app->summary.phase[0] ? app->summary.phase : "No summary written yet.");
}

static void draw_settings(App* app) {
    Config* c = &app->config;
    ImGui::SeparatorText("Recording");
    bool paused = app->capture.paused;
    if (ImGui::Checkbox("Private mode (record nothing)", &paused)) {
        capture_set_paused(app, paused);
        tray_update(app);
    }

    ImGui::SeparatorText("Transcription");
    const char* modes[] = {"Wait while a game runs", "Always", "Paused"};
    i32 mode = c->processing.mode;
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Mode", &mode, modes, COUNT(modes))) worker_set_mode(app, (Mode)mode);
    i32 limit = (i32)c->processing.gpu_busy_percent;
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderInt("GPU busy above", &limit, 5, 100, "%d%%")) worker_set_gpu_limit(app, (u32)limit);
    ImGui::TextDisabled("%s", app->worker.state);

    ImGui::SeparatorText("Daily summary");
    bool on = c->summary.enabled;
    if (ImGui::Checkbox("Ask to write a summary each evening", &on)) summary_set_enabled(app, on);
    i32 words = (i32)c->summary.max_words;
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputInt("Word limit", &words, 50, 100) && words >= 50) summary_set_max_words(app, (u32)words);

    ImGui::SeparatorText("lifelog");
    if (ImGui::Button("Restart")) app_restart(app);
    ImGui::SameLine();
    if (ImGui::Button("Quit")) app_quit(app);
}

bool ui_draw_window(App* app, f32* next_frame_ms) {
    UiState* s = app->ui.state;
    CHECK(s, "the window's state to be made when it opened");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("main", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (ImGui::BeginTabBar("tabs")) {
        Tab want = app->ui.want_tab;
        ImGuiTabItemFlags f0 = want == TAB_TIMELINE ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags f1 = want == TAB_JOURNAL ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags f2 = want == TAB_SETTINGS ? ImGuiTabItemFlags_SetSelected : 0;
        app->ui.want_tab = (Tab)0xff;
        if (ImGui::BeginTabItem("Timeline", 0, f0)) { draw_timeline(app, s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Journal", 0, f1)) { draw_journal(app, s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Settings", 0, f2)) { draw_settings(app); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    if (app->ui.toast[0]) {
        f64 age = now_ts() - app->ui.toast_ts;
        if (age < 4) {
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 8);
            ImGui::TextDisabled("%s", app->ui.toast);
            *next_frame_ms = (f32)((4 - age) * 1000) + 10;
        } else {
            app->ui.toast[0] = 0;
        }
    }
    ImGui::End();
    return true;
}

bool ui_draw_popup(App* app, f32* next_frame_ms) {
    (void)next_frame_ms;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("popup", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    bool keep = true;
    f32 w = ImGui::GetContentRegionAvail().x;
    if (app->ui.want_popup == POPUP_DELAY) {
        ImGui::TextWrapped("Ask about the summary later");
        if (ImGui::Button("In an hour", ImVec2(w, 0))) { summary_act(app, scratch(), "later", app->ui.popup_day, 0); keep = false; }
        if (ImGui::Button("Skip today", ImVec2(w, 0))) { summary_act(app, scratch(), "skip", app->ui.popup_day, 0); keep = false; }
    } else {
        ImGui::Text("lifelog");
        ImGui::TextDisabled("%s", app->capture.paused ? "Private: recording nothing" : "Recording");
        ImGui::Separator();
        if (ImGui::Button("Open timeline", ImVec2(w, 0))) { ui_show(app, TAB_TIMELINE, 0); keep = false; }
        if (ImGui::Button("Journal", ImVec2(w, 0))) { ui_show(app, TAB_JOURNAL, 0); keep = false; }
        if (ImGui::Button(app->capture.paused ? "Start recording" : "Private mode", ImVec2(w, 0))) {
            capture_set_paused(app, !app->capture.paused);
            tray_update(app);
            keep = false;
        }
        if (ImGui::Button("Paste here", ImVec2(w, 0))) {
            char error[256];
            i32 n = files_paste(app, error, sizeof(error));
            if (n < 0) ui_say(app, "Paste failed: %s", error);
            keep = false;
        }
        if (ImGui::Button("Settings", ImVec2(w, 0))) { ui_show(app, TAB_SETTINGS, 0); keep = false; }
        ImGui::Separator();
        if (ImGui::Button("Quit lifelog", ImVec2(w, 0))) { app_quit(app); keep = false; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) keep = false;
    ImGui::End();
    return keep;
}
