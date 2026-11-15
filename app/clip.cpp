// clip.cpp: see clip.h. To come.

ClipResult clip_copy(App* app, f64 from, f64 to, Names* exclude, ClipKind kind) {
    (void)app; (void)from; (void)to; (void)exclude; (void)kind;
    ClipResult r = {};
    snprintf(r.error, sizeof(r.error), "not built yet");
    return r;
}
bool clip_range(Str range, f64 now, f64* from) { (void)range; (void)now; (void)from; return false; }
i32 files_paste(App* app, char* error, i32 cap) { (void)app; snprintf(error, cap, "not built yet"); return -1; }
void file_open(App* app, i64 id, bool in_folder) { (void)app; (void)id; (void)in_folder; }
void play(App* app, i64 line_id, PlayMode mode, Names* hidden) { (void)app; (void)line_id; (void)mode; (void)hidden; }
void play_stop(App* app) { app->clips.playing_line = 0; }
