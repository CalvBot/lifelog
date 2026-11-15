// ui_popups.cpp: the small windows by the tray: lifelog's menu and the summary delay box (port of
// ui/src/popups).

struct PopupState {
    i32 unused;
};

// One frame of the popup. False when it should close.
static bool popup_draw(App* app, PopupState* s, f32* next_frame_ms) {
    (void)s; (void)next_frame_ms;
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
            files_paste(app, error, sizeof(error));
            keep = false;
        }
        if (ImGui::Button("Settings", ImVec2(w, 0))) { ui_show(app, TAB_SETTINGS, 0); keep = false; }
        ImGui::Separator();
        if (ImGui::Button("Quit lifelog", ImVec2(w, 0))) { app_quit(app); keep = false; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) keep = false;
    return keep;
}

static void popup_free(PopupState* s) { (void)s; }
