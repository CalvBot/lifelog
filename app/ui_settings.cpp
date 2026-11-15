// ui_settings.cpp: the Settings tab (port of ui/src/settings).

struct SettingsState {
    i32 unused;
};

static void settings_draw(App* app, SettingsState* s, f32* next_frame_ms) {
    (void)s; (void)next_frame_ms;
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
    ImGui::TextDisabled("%s", app->worker.state);
    ImGui::SeparatorText("lifelog");
    if (ImGui::Button("Restart")) app_restart(app);
    ImGui::SameLine();
    if (ImGui::Button("Quit")) app_quit(app);
}

static void settings_free(SettingsState* s) { (void)s; }
