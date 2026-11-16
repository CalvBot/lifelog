// ui.cpp: the window's frame: the tab bar, the listening pill, the toast. Each tab is its own
// file (ui_timeline, ui_journal, ui_settings) with its own state, kept while the window is open.
// Each frame draws from App directly.

struct UiState {
    TimelineState timeline;
    JournalState journal;
    SettingsState settings;
};

void ui_window_opened(App* app) {
    UiState* s = (UiState*)calloc(1, sizeof(UiState));
    CHECK(s, "memory for the window's state");
    app->ui.state = s;
}

void ui_window_closed(App* app) {
    UiState* s = app->ui.state;
    if (!s) return;
    timeline_free(&s->timeline);
    journal_free(&s->journal);
    settings_free(&s->settings);
    free(s);
    app->ui.state = 0;
}

void ui_popup_opened(App* app) {
    PopupState* s = (PopupState*)calloc(1, sizeof(PopupState));
    CHECK(s, "memory for the popup's state");
    app->ui.popup_state = s;
}

void ui_popup_closed(App* app) {
    if (!app->ui.popup_state) return;
    popup_free(app->ui.popup_state);
    free(app->ui.popup_state);
    app->ui.popup_state = 0;
}

void ui_jump(App* app, f64 ts, Str said) {
    app->ui.want_jump = true;
    app->ui.jump_ts = ts;
    SET(app->ui.jump_said, said);
    ui_show(app, TAB_TIMELINE, 0);
}

// The top-right pill: click to pause or resume recording.
static void listen_button(App* app) {
    bool paused = app->capture.paused;
    const char* text = paused ? label(ICON_CIRCLE_PAUSE, "Paused") : label(ICON_MIC, "Listening");
    ImVec4 fill = paused ? ImGui::GetStyleColorVec4(ImGuiCol_Button) : ImVec4(ACCENT.x, ACCENT.y, ACCENT.z, 0.18f);
    ImVec4 color = paused ? weak_text() : ACCENT;
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fill.x, fill.y, fill.z, fill.w + 0.1f));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, px(14));
    if (ImGui::Button(text, ImVec2(px(116), 0))) {
        capture_set_paused(app, !paused);
        tray_update(app);
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

static void tab_button(App* app, Tab tab, const char* icon, const char* text) {
    bool on = app->ui.tab == tab;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(label(icon, text))) app->ui.tab = tab;
    ImGui::PopStyleColor();
}

bool ui_draw_window(App* app, f32* next_frame_ms) {
    UiState* s = app->ui.state;
    CHECK(s, "the window's state to be made when it opened");
    Mark m = mark(scratch());

    // What other threads asked for.
    if (app->ui.want_tab != (Tab)0xff) {
        app->ui.tab = app->ui.want_tab;
        if (app->ui.tab == TAB_JOURNAL) journal_show_day(&s->journal, app->ui.want_day[0] ? app->ui.want_day : 0);
        app->ui.want_tab = (Tab)0xff;
    }
    if (app->ui.want_jump) {
        app->ui.want_jump = false;
        app->ui.tab = TAB_TIMELINE;
        timeline_jump(app, &s->timeline, app->ui.jump_ts, str(app->ui.jump_said));
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("main", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    // The tab bar.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(8)));
    ImGui::BeginChild("tabs", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    tab_button(app, TAB_TIMELINE, ICON_SCROLL_TEXT, "Timeline");
    ImGui::SameLine();
    tab_button(app, TAB_JOURNAL, ICON_CALENDAR, "Journal");
    ImGui::SameLine();
    tab_button(app, TAB_SETTINGS, ICON_SETTINGS, "Settings");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - px(116));
    listen_button(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(8)));
    // Settings scrolls as a whole; its scrollbar stays put so switching tabs doesn't shift the layout.
    ImGuiWindowFlags page_flags = app->ui.tab == TAB_SETTINGS ? ImGuiWindowFlags_AlwaysVerticalScrollbar : 0;
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, page_flags);
    switch (app->ui.tab) {
    case TAB_TIMELINE: timeline_draw(app, &s->timeline, next_frame_ms); break;
    case TAB_JOURNAL: journal_draw(app, &s->journal, next_frame_ms); break;
    case TAB_SETTINGS: settings_draw(app, &s->settings, next_frame_ms); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // While something plays, the timeline follows it.
    if (app->clips.playing_line && (*next_frame_ms < 0 || *next_frame_ms > 250)) *next_frame_ms = 250;

    // The toast, centred above the bottom edge for 4 s.
    if (app->ui.toast[0]) {
        f64 age = now_ts() - app->ui.toast_ts;
        if (age < 4) {
            ImVec2 size = ImGui::CalcTextSize(app->ui.toast);
            ImVec2 pad = ImVec2(14, 8);
            ImVec2 at = ImVec2(vp->WorkPos.x + (vp->WorkSize.x - size.x) / 2 - pad.x, vp->WorkPos.y + vp->WorkSize.y - 56 - size.y - pad.y * 2);
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(at, ImVec2(at.x + size.x + pad.x * 2, at.y + size.y + pad.y * 2), ImGui::GetColorU32(ImGuiCol_PopupBg), 8);
            dl->AddRect(at, ImVec2(at.x + size.x + pad.x * 2, at.y + size.y + pad.y * 2), ImGui::GetColorU32(ImGuiCol_Border), 8);
            dl->AddText(ImVec2(at.x + pad.x, at.y + pad.y), ImGui::GetColorU32(ImGuiCol_Text), app->ui.toast);
            f32 left = (f32)((4 - age) * 1000) + 10;
            if (*next_frame_ms < 0 || *next_frame_ms > left) *next_frame_ms = left;
        } else {
            app->ui.toast[0] = 0;
        }
    }
    ImGui::End();
    rewind(m);
    return true;
}

bool ui_draw_apps(App* app, f32*) {
    UiState* s = app->ui.state;
    if (!s) return false;  // it belongs to the main window, which has closed
    Mark m = mark(scratch());
    bool keep = timeline_apps_draw(app, &s->timeline);
    rewind(m);
    return keep;
}

void ui_apps_closed(App* app) {
    app->ui.want_apps = false;
    if (app->ui.state) app->ui.state->timeline.apps_open = false;
}

bool ui_draw_popup(App* app, f32* next_frame_ms) {
    CHECK(app->ui.popup_state, "the popup's state to be made when it opened");
    Mark m = mark(scratch());
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("popup", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    bool keep = popup_draw(app, app->ui.popup_state, next_frame_ms);
    // As tall as what was drawn (down to the last item), so there is no empty space below it.
    f32 bottom = ImGui::GetItemRectMax().y - ImGui::GetWindowPos().y;
    app->ui.popup_height = (i32)(bottom + ImGui::GetStyle().WindowPadding.y + 0.5f);
    ImGui::End();
    rewind(m);
    return keep;
}
