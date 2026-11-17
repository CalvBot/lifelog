// ui_settings.cpp: the Settings tab: which apps are recorded and which are games, transcription,
// voice commands, the daily summary, where files live and the AI models. Ctrl+Z takes back the last
// change made here.

enum AppsView : u8 {
    VIEW_NEW,
    VIEW_INCLUDED,  // recorded, games too
    VIEW_EXCLUDED,
    VIEW_FOLDERS,   // the folder rules
};

static const i32 APPS_PER_PAGE = 10;

enum UndoKind : u8 {
    UNDO_APP,        // put an app's decision and game mark back
    UNDO_FOLDER,     // put a folder's rule back
    UNDO_MODE,
    UNDO_GPU,
    UNDO_ENABLED,
    UNDO_ASK_AT,
    UNDO_WORDS,
    UNDO_REMINDERS,
    UNDO_PAUSE_GAMES,
    UNDO_NEVER,      // a never-mention topic that was allowed again: add it back
    UNDO_CHIME_STYLE,
    UNDO_CHIME_VOLUME,
    UNDO_VOICE,
    UNDO_WAKE,
};

// One step back for Ctrl+Z: the value it puts back.
struct SettingsUndo {
    UndoKind kind;
    char label[128];  // "spotify included", for the message after undoing
    char exe[64];
    char folder[MAX_PATH];
    Decision decision;
    GameMark game;
    u32 n;
    bool on;
    char text[128];
};

struct SettingsState {
    // The apps page on screen, made again when something changes (and every 10 s, so names found
    // by the scan and "x min ago" catch up), not every frame.
    AppsView view;
    i32 page;
    i32 pages;
    i32 counts[4];
    Name rows[APPS_PER_PAGE];
    i32 row_count;
    bool stale;
    u64 loaded_version;  // App.apps.version when it was made
    i32 drawn_frame;     // the last frame Settings was drawn in
    char search[128];
    bool show_by_folder;  // list apps that only follow a folder rule too
    i32 by_folder;        // how many of those the list on screen has (shown or not)

    // The fields being typed in or dragged; set from the config while not being edited.
    char ask_at[16];
    i32 max_words;
    i32 gpu;
    i32 chime_volume;
    bool ask_at_active;
    char wendpoint[512];  // transcription's own server
    bool wendpoint_active;
    char wmodel[128];
    bool wmodel_active;
    char endpoint[512];
    bool endpoint_active;
    char model[128];
    bool model_active;
    char wake[64];        // the voice commands' wake word
    bool wake_active;
    bool voice_list;      // the list of voice commands is open
    bool own[SETUP_PARTS];  // "Your own server" was picked, before its address is typed

    // The AI file to delete once the confirmation says yes.
    SetupPart remove_part;
    char remove_model[128];
    bool remove_all;

    List<SettingsUndo> undo;
};

// ---------------------------------------------------------------------------------------------
// Look

static f32 small_size() { return ImGui::GetStyle().FontSizeBase * 0.8f; }

// Titles stand out a little from the body text.
static ImVec4 strong_text() {
    ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    return bg.x < 0.5f ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);
}

// A card's title, in bold on its own line.
static void title(const char* text) {
    ImFont* bold = bold_font();
    if (bold) ImGui::PushFont(bold, 0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(strong_text(), "%s", text);
    if (bold) ImGui::PopFont();
}

static void weak(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(weak_text(), "%s", text);
}

// A rounded card `width` wide around a section (call settings_card_end after).
static void settings_card(const char* id, f32 width) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), px(14)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild(id, ImVec2(width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // Text boxes, drag values and switches sit darker than the card, as the window's background.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
}
static void settings_card_end() {
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// A form row: its name in the left column, then (on the same line) what it sets.
static const f32 ROW_LABEL_W = 132;
static void row_label(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(weak_text(), "%s", text);
    ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + px(ROW_LABEL_W));
}

// `text` in weak text at the right of the current line (a card title's status).
static void status_right(const char* text) {
    f32 w = ImGui::CalcTextSize(text).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
    weak(text);
}

// A button that stays lit while `on` (a selectable button). `size` is the least it takes.
static bool pick(const char* text, bool on, ImVec2 size = ImVec2(0, 0)) {
    f32 need = ImGui::CalcTextSize(text, 0, true).x + ImGui::GetStyle().FramePadding.x * 2;
    if (size.x < need) size.x = need;
    ImVec4 lit = ImVec4(ACCENT.x, ACCENT.y, ACCENT.z, 0.55f);
    ImGui::PushStyleColor(ImGuiCol_Button, on ? lit : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(lit.x, lit.y, lit.z, 0.7f) : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    if (on) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool clicked = button(text, size);
    ImGui::PopStyleColor(on ? 3 : 2);
    return clicked;
}


// ---------------------------------------------------------------------------------------------
// Opening files: on the pool, since the shell can take a while. Windows lets the new window take
// the focus because this process just had the click.

struct OpenPathJob {
    wchar_t path[MAX_PATH];
    bool select;  // show it selected in its folder instead of opening it
    bool ok;
};

static void open_path_run(Job* job) {
    OpenPathJob* o = (OpenPathJob*)job->data;
    o->ok = shell_show(o->path, o->select);
}

static void open_path_apply(App* app, Job* job) {
    OpenPathJob* o = (OpenPathJob*)job->data;
    if (o->ok) return;
    Str path = narrow(scratch(), o->path);
    WARN("could not open %s", path.ptr);
    ui_say(app, "Could not open %s", path.ptr);
}

static void settings_open_path(App*, const wchar_t* path, bool select) {
    Job* job = job_new(open_path_apply);
    OpenPathJob* o = PUSH(&job->arena, OpenPathJob);
    wcsncpy_s(o->path, path, _TRUNCATE);
    o->select = select;
    job->data = o;
    pool_post(job, open_path_run);
}

// ---------------------------------------------------------------------------------------------
// Undo

static SettingsUndo* remember(SettingsState* s, UndoKind kind, const char* fmt, ...) {
    if (s->undo.count >= 50) list_remove(&s->undo, 0);
    SettingsUndo* u = list_push(&s->undo);
    u->kind = kind;
    va_list args;
    va_start(args, fmt);
    vsnprintf(u->label, sizeof(u->label), fmt, args);
    va_end(args);
    return u;
}

// Remembers how `exe` is now, before it is changed.
static void remember_app(App* app, SettingsState* s, Str exe, const char* what) {
    Str name = app_label(app, exe);
    SettingsUndo* u = remember(s, UNDO_APP, "%.*s %s", (int)name.len, name.ptr, what);
    SET(u->exe, exe);
    AppInfo* a = app_find(app, exe);
    u->decision = a ? a->decision : DECISION_NEW;
    u->game = a ? a->game : GAME_NO;
    u->on = app_removed(app, exe);
}

static void undo_last(App* app, SettingsState* s) {
    if (!s->undo.count) return;
    SettingsUndo u = s->undo[s->undo.count - 1];
    s->undo.count--;
    switch (u.kind) {
    case UNDO_APP: {
        AppInfo* a = app_find(app, str(u.exe));
        if (!a || a->decision != u.decision) app_set_decision(app, str(u.exe), u.decision);
        if (!a || a->game != u.game) app_set_game(app, str(u.exe), u.game);
        if (!u.on) app_unremove(app, str(u.exe));
    } break;
    case UNDO_FOLDER: app_set_folder(app, str(u.folder), u.decision); break;
    case UNDO_MODE: worker_set_mode(app, (Mode)u.n); break;
    case UNDO_GPU: worker_set_gpu_limit(app, u.n); break;
    case UNDO_ENABLED: summary_set_enabled(app, u.on); break;
    case UNDO_ASK_AT: summary_set_ask_at(app, str(u.text)); break;
    case UNDO_WORDS: summary_set_max_words(app, u.n); break;
    case UNDO_REMINDERS: summary_set_ask_reminders(app, u.on); break;
    case UNDO_PAUSE_GAMES: worker_set_pause_for_games(app, u.on); break;
    case UNDO_NEVER: summary_add_never(app, str(u.text), 0, 0); break;
    case UNDO_CHIME_STYLE: chime_set_style(app, (ChimeStyle)u.n); break;
    case UNDO_CHIME_VOLUME: chime_set_volume(app, u.n); break;
    case UNDO_VOICE: voice_set_listen(app, u.on); break;
    case UNDO_WAKE: voice_set_wake(app, str(u.text)); break;
    }
    s->stale = true;
    ui_say(app, "Undone: %s", u.label);
}

// ---------------------------------------------------------------------------------------------
// Apps

static AppsView view_of(App* app, AppInfo* a) {
    Decision d = app_decision(app, a);
    if (a->game == GAME_YES || d == DECISION_INCLUDED) return VIEW_INCLUDED;
    if (d == DECISION_EXCLUDED) return VIEW_EXCLUDED;
    return VIEW_NEW;
}

struct AppRowSort {
    f64 key;  // last heard; "now" for one playing
    const char* exe;
};

static int by_recent(const void* pa, const void* pb) {
    const AppRowSort* a = (const AppRowSort*)pa;
    const AppRowSort* b = (const AppRowSort*)pb;
    if (a->key != b->key) return a->key > b->key ? -1 : 1;
    return strcmp(a->exe, b->exe);
}

// The counts per list and the rows of the page on screen; the search matches the exe or the name.
static void apps_load(App* app, SettingsState* s) {
    Mark m = mark(scratch());
    Str q = str_lower(scratch(), str_trim(str(s->search)));
    List<AppRowSort> rows = {};
    s->counts[0] = s->counts[1] = s->counts[2] = 0;
    s->by_folder = 0;
    s->counts[VIEW_FOLDERS] = app->config.capture.included_folders.count + app->config.capture.excluded_folders.count;
    f64 now = now_ts();
    for (i32 i = 0; i < app->apps.list.count; i++) {
        AppInfo* a = &app->apps.list[i];
        if (is_own_exe(str(a->exe))) continue;
        if (!a->playing && app_removed(app, str(a->exe))) continue;
        if (q.len && str_find(str(a->exe), q) < 0 && str_find(str_lower(scratch(), str(a->name)), q) < 0) continue;
        AppsView v = view_of(app, a);
        // New lists what was heard, plays now, or is installed; not every exe the config names.
        if (v == VIEW_NEW && !a->playing && !a->installed && a->lines == 0 && a->last_ts == 0) continue;
        // Included or excluded only by a folder rule: counted aside, listed when asked for.
        if (v != VIEW_NEW && a->decision == DECISION_NEW && a->game != GAME_YES) {
            if (v == s->view) s->by_folder++;
            if (!s->show_by_folder) continue;
        }
        s->counts[v]++;
        if (v == s->view) list_push(&rows, {v == VIEW_NEW && a->playing ? now : a->last_ts, a->exe});
    }
    if (rows.count) qsort(rows.items, rows.count, sizeof(AppRowSort), by_recent);
    s->pages = rows.count ? (rows.count + APPS_PER_PAGE - 1) / APPS_PER_PAGE : 1;
    if (s->page >= s->pages) s->page = s->pages - 1;
    s->row_count = 0;
    for (i32 i = s->page * APPS_PER_PAGE; i < rows.count && s->row_count < APPS_PER_PAGE; i++)
        SET(s->rows[s->row_count++].s, str(rows[i].exe));
    list_free(&rows);
    s->stale = false;
    s->loaded_version = app->apps.version;
    rewind(m);
}

// What a click on a row asks for; done after the rows are drawn.
enum AppAct : u8 { ACT_NONE, ACT_INCLUDE, ACT_EXCLUDE, ACT_GAME, ACT_REVEAL, ACT_INCLUDE_FOLDER, ACT_EXCLUDE_FOLDER, ACT_REMOVE };

// ---------------------------------------------------------------------------------------------
// Folder rules

static void set_folder(App* app, SettingsState* s, Str folder, Decision d) {
    Str shown = folder;
    while (shown.len && (shown.ptr[shown.len - 1] == '\\' || shown.ptr[shown.len - 1] == '/')) shown.len--;
    for (i64 i = shown.len - 1; i >= 0; i--)
        if (shown.ptr[i] == '\\' || shown.ptr[i] == '/') {
            shown = str_sub(shown, i + 1);
            break;
        }
    const char* what = d == DECISION_INCLUDED ? "included" : d == DECISION_EXCLUDED ? "excluded" : "no longer a rule";
    SettingsUndo* u = remember(s, UNDO_FOLDER, "everything in %.*s %s", (int)(shown.len > 80 ? 80 : shown.len), shown.ptr, what);
    SET(u->folder, folder);
    u->decision = folder_decision(&app->config, folder);
    app_set_folder(app, folder, d);
    s->stale = true;
}

// The folder picker is modal, so it runs on the pool while the window keeps drawing. Its answer
// comes back as a job; the next frame applies it. These are the main thread's.
static bool g_picking;
static Decision g_pick_decision;
static bool g_pick_ai;  // for the AI folder, not a folder rule
static char g_picked[MAX_PATH];

struct PickJob {
    Decision decision;
    bool ai;
    char picked[MAX_PATH];
};

static void folder_pick_run(Job* job) {
    PickJob* j = (PickJob*)job->data;
    char* picked = j->picked;
    IFileOpenDialog* d = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(j->ai ? L"Where to keep lifelog's AI"
                    : j->decision == DECISION_INCLUDED ? L"Include every app in a folder" : L"Exclude every app in a folder");
        IShellItem* item = 0;
        if (SUCCEEDED(d->Show(0)) && SUCCEEDED(d->GetResult(&item))) {
            wchar_t* path = 0;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                Mark m = mark(scratch());
                set(picked, MAX_PATH, narrow(scratch(), path));
                rewind(m);
                CoTaskMemFree(path);
            }
            item->Release();
        }
        d->Release();
    }
}

static void folder_pick_apply(App* app, Job* job) {
    PickJob* j = (PickJob*)job->data;
    SET(g_picked, str(j->picked));
    g_picking = false;
    wake_window(app);
}

static void folders_view(App* app, SettingsState* s) {
    if (!g_picking && g_picked[0] && !g_pick_ai) {
        set_folder(app, s, str(g_picked), g_pick_decision);
        g_picked[0] = 0;
    }
    Config* c = &app->config;
    Str q = str_lower(scratch(), str_trim(str(s->search)));
    const f32 row_h = px(30);
    ImVec2 bsize = ImVec2(px(40), ImGui::GetFrameHeight());
    f32 spacing = ImGui::GetStyle().ItemSpacing.x;
    f32 rows_top = ImGui::GetCursorPosY();
    char act_folder[MAX_PATH] = "";
    Decision act_to = DECISION_NEW;
    bool act = false, reveal = false;
    i32 shown = 0;
    for (i32 list = 0; list < 2; list++) {
        Folders* fs = list ? &c->capture.excluded_folders : &c->capture.included_folders;
        Decision d = list ? DECISION_EXCLUDED : DECISION_INCLUDED;
        for (i32 i = 0; i < fs->count; i++) {
            const char* path = fs->items[i].s;
            if (q.len && str_find(str_lower(scratch(), str(path)), q) < 0) continue;
            shown++;
            ImGui::PushID(list * 100000 + i);
            ImVec2 at = ImGui::GetCursorScreenPos();
            f32 x = at.x + ImGui::GetContentRegionAvail().x;
            bool clicked = false;
            Decision to = DECISION_NEW;
            // Where the app rows have theirs: the other choice on the right, then Remove.
            x -= bsize.x;
            ImGui::SetCursorScreenPos(ImVec2(x, at.y + (row_h - bsize.y) / 2));
            if (button(d == DECISION_EXCLUDED ? ICON_CHECK "##inc" : ICON_X "##exc", bsize)) {
                clicked = true;
                to = d == DECISION_EXCLUDED ? DECISION_INCLUDED : DECISION_EXCLUDED;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d == DECISION_EXCLUDED ? "Include instead" : "Exclude instead");
            x -= spacing;
            const char* remove = "Remove";
            x -= ImGui::CalcTextSize(remove).x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SetCursorScreenPos(ImVec2(x, at.y + (row_h - bsize.y) / 2));
            if (ImGui::Button(remove)) {
                clicked = true;
                to = DECISION_NEW;
            }
            x -= spacing;

            i32 apps = 0;
            for (i32 k = 0; k < app->apps.list.count; k++)
                if (app->apps.list[k].path[0] && path_under(str(app->apps.list[k].path), str(path))) apps++;
            const char* what = tmp("%s · %d %s", d == DECISION_INCLUDED ? "included" : "excluded", apps, apps == 1 ? "app" : "apps");
            ImGui::PushFont(0, small_size());
            f32 what_w = ImGui::CalcTextSize(what).x;
            ImGui::PopFont();
            f32 text_y = at.y + (row_h - ImGui::GetTextLineHeight()) / 2;
            ImGui::SetCursorScreenPos(ImVec2(at.x, text_y));
            ImGui::BeginGroup();
            ImGui::TextColored(weak_text(), "%s", ICON_FOLDER);
            ImGui::SameLine();
            f32 left = x - ImGui::GetCursorScreenPos().x - what_w - spacing;
            ImGui::TextUnformatted(fit(path, left));
            ImGui::SameLine();
            ImGui::PushFont(0, small_size());
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, at.y + (row_h - ImGui::GetTextLineHeight()) / 2 + px(1)));
            ImGui::TextColored(weak_text(), "%s", what);
            ImGui::PopFont();
            ImGui::EndGroup();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path);
            if (ImGui::BeginPopupContextItem("menu")) {
                if (ImGui::MenuItem(label(ICON_FOLDER_OPEN, "Open folder"))) reveal = true;
                if (ImGui::MenuItem(label(ICON_X, "Remove rule"))) {
                    clicked = true;
                    to = DECISION_NEW;
                }
                ImGui::EndPopup();
            }
            if ((clicked || reveal) && !act_folder[0]) {
                SET(act_folder, str(path));
                act = clicked;
                act_to = to;
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + row_h));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
    }
    if (!shown) weak(q.len ? "No match." : "None. A folder's rule covers every app in it, subfolders too; an app's own choice wins.");
    // As tall as a full page of apps, so switching tabs never moves the card's edge.
    f32 full = APPS_PER_PAGE * (row_h + ImGui::GetStyle().ItemSpacing.y);
    f32 used = ImGui::GetCursorPosY() - rows_top;
    if (used < full) ImGui::SetCursorPosY(rows_top + full);
    ImGui::BeginDisabled(g_picking);
    for (i32 k = 0; k < 2; k++) {
        if (k) ImGui::SameLine();
        if (ImGui::Button(label(ICON_PLUS, k ? "Exclude a folder" : "Include a folder"))) {
            g_pick_decision = k ? DECISION_EXCLUDED : DECISION_INCLUDED;
            g_pick_ai = false;
            g_picked[0] = 0;
            g_picking = true;
            Job* job = job_new(folder_pick_apply);
            PickJob* j = PUSH(&job->arena, PickJob);
            j->decision = g_pick_decision;
            job->data = j;
            pool_post(job, folder_pick_run);
        }
    }
    ImGui::EndDisabled();
    if (act) set_folder(app, s, str(act_folder), act_to);
    else if (act_folder[0]) settings_open_path(app, wide(scratch(), str(act_folder)), false);
}

static void apps_card(App* app, SettingsState* s, f32 width) {
    settings_card("apps", width);
    title("Processes");
    const char* names[] = {"New", "Included", "Excluded", "Folders"};
    for (i32 v = 0; v < 4; v++) {
        if (v) ImGui::SameLine();
        if (pick(tmp("%s  %d##view%d", names[v], s->counts[v], v), s->view == v, ImVec2(px(96), 0)) && s->view != v) {
            s->view = (AppsView)v;
            s->page = 0;
            s->stale = true;
        }
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(weak_text(), "%s", ICON_SEARCH);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##search", "Search", s->search, sizeof(s->search))) {
        s->page = 0;
        s->stale = true;
    }
    ImGui::Dummy(ImVec2(0, 0));

    if (s->view == VIEW_FOLDERS) {
        folders_view(app, s);
        settings_card_end();
        return;
    }

    AppAct act = ACT_NONE;
    char act_exe[64] = "";
    const f32 row_h = px(30);
    ImVec2 bsize = ImVec2(px(40), ImGui::GetFrameHeight());
    f32 spacing = ImGui::GetStyle().ItemSpacing.x;
    f32 rows_top = ImGui::GetCursorPosY();
    for (i32 i = 0; i < s->row_count; i++) {
        Str exe = str(s->rows[i].s);
        AppInfo* a = app_find(app, exe);
        if (!a) continue;
        bool game = a->game == GAME_YES;
        ImGui::PushID(i);
        ImVec2 at = ImGui::GetCursorScreenPos();
        f32 right = at.x + ImGui::GetContentRegionAvail().x;

        // Buttons first, from the right, so a long path only gets the room left over. The same
        // buttons sit in the same places on every row, so a run of yes/no clicks stays put.
        f32 x = right;
        bool show_include = s->view != VIEW_INCLUDED;
        bool show_exclude = s->view != VIEW_EXCLUDED;
        if (show_include) {
            x -= bsize.x;
            ImGui::SetCursorScreenPos(ImVec2(x, at.y + (row_h - bsize.y) / 2));
            if (button(ICON_CHECK "##inc", bsize)) act = ACT_INCLUDE;
            x -= spacing;
        }
        if (show_exclude) {
            x -= bsize.x;
            ImGui::SetCursorScreenPos(ImVec2(x, at.y + (row_h - bsize.y) / 2));
            if (button(ICON_X "##exc", bsize)) act = ACT_EXCLUDE;
            x -= spacing;
        }
        x -= px(32);
        ImGui::SetCursorScreenPos(ImVec2(x, at.y + (row_h - bsize.y) / 2));
        ImVec4 pad_color = game ? ACCENT : ImVec4(weak_text().x, weak_text().y, weak_text().z, 0.6f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, pad_color);
        ImGui::PushFont(0, ImGui::GetStyle().FontSizeBase * 1.12f);
        if (button(ICON_GAMEPAD "##game", ImVec2(px(32), bsize.y))) act = ACT_GAME;
        ImGui::PopFont();
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", game ? "Game" : "Mark as game");
        x -= spacing;

        // Icon, name, "N lines · x ago" and the path, in what is left.
        f32 text_y = at.y + (row_h - ImGui::GetTextLineHeight()) / 2;
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + (row_h - px(20)) / 2));
        ImGui::BeginGroup();
        draw_app_icon(app, exe, px(20));
        ImGui::SameLine();
        f32 left = x - ImGui::GetCursorScreenPos().x;
        const char* name = str_copy(scratch(), app_label(app, exe)).ptr;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, text_y));
        const char* shown = fit(name, left);
        ImGui::TextUnformatted(shown);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a->exe);
        left -= ImGui::CalcTextSize(shown).x + spacing;
        ImGui::PushFont(0, small_size());
        f32 small_y = at.y + (row_h - ImGui::GetTextLineHeight()) / 2 + px(1);
        if (a->lines > 0 && left > px(40)) {
            const char* heard = tmp("%lld lines · %s", a->lines, ago_text(a->last_ts));
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, small_y));
            heard = fit(heard, left);
            ImGui::TextColored(weak_text(), "%s", heard);
            left -= ImGui::CalcTextSize(heard).x + spacing;
        }
        if (a->path[0] && left > px(40)) {
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, small_y));
            ImGui::TextColored(weak_text(), "%s", fit(a->path, left));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a->path);
        }
        ImGui::PopFont();
        ImGui::EndGroup();

        // Right-click on the app: the same choices, and its file.
        if (ImGui::BeginPopupContextItem("menu")) {
            if (ImGui::MenuItem(label(ICON_GAMEPAD, game ? "Not a game" : "Add as game"))) act = ACT_GAME;
            if (s->view != VIEW_INCLUDED && ImGui::MenuItem(label(ICON_CHECK, "Include"))) act = ACT_INCLUDE;
            if (s->view != VIEW_EXCLUDED && ImGui::MenuItem(label(ICON_X, "Exclude"))) act = ACT_EXCLUDE;
            if (ImGui::MenuItem(label(ICON_FOLDER_OPEN, "Open file location"), 0, false, a->path[0] != 0)) act = ACT_REVEAL;
            ImGui::Separator();
            if (ImGui::MenuItem(label(ICON_FOLDER, "Include every app in its folder"), 0, false, a->path[0] != 0)) act = ACT_INCLUDE_FOLDER;
            if (ImGui::MenuItem(label(ICON_FOLDER, "Exclude every app in its folder"), 0, false, a->path[0] != 0)) act = ACT_EXCLUDE_FOLDER;
            ImGui::Separator();
            if (ImGui::MenuItem(label(ICON_TRASH, "Remove from list"))) act = ACT_REMOVE;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forgets its choices. It comes back under New if it plays sound again.");
            ImGui::EndPopup();
        }
        if (act != ACT_NONE && !act_exe[0]) SET(act_exe, exe);

        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + row_h));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }

    if (s->row_count == 0) {
        Str query = str_lower(scratch(), str_trim(str(s->search)));
        if (!query.len) {
            weak(s->view == VIEW_NEW ? "All decided." : "None.");
        } else {
            weak("No match.");
            Str add = str_ends(query, str(".exe")) ? query : str_fmt(scratch(), "%s.exe", query.ptr);
            if (str_find(add, str(" ")) < 0 && add.len < (i64)sizeof(Name)) {
                ImGui::SameLine();
                if (ImGui::Button(label(ICON_PLUS, tmp("Add %s", add.ptr)))) {
                    remember_app(app, s, add, "included");
                    app_set_decision(app, add, DECISION_INCLUDED);
                    s->view = VIEW_INCLUDED;
                    s->page = 0;
                    SET(s->search, add);
                    s->stale = true;
                }
            }
        }
    }

    {
        // Every page of every tab takes the height of a full one and shows the arrows (greyed out
        // when there is one page), so switching tabs or pages never moves the card's edge.
        f32 full = APPS_PER_PAGE * (row_h + ImGui::GetStyle().ItemSpacing.y);
        f32 used = ImGui::GetCursorPosY() - rows_top;
        if (used < full) ImGui::SetCursorPosY(rows_top + full);
        ImGui::BeginDisabled(s->page == 0);
        if (ImGui::Button(ICON_CHEVRON_LEFT "##prev")) { s->page--; s->stale = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d / %d", s->page + 1, s->pages);
        ImGui::SameLine();
        ImGui::BeginDisabled(s->page + 1 >= s->pages);
        if (ImGui::Button(ICON_CHEVRON_RIGHT "##next")) { s->page++; s->stale = true; }
        ImGui::EndDisabled();
        // Apps that only follow a folder rule (a call app per person adds up fast) are left out
        // unless asked for.
        if (s->view != VIEW_NEW) {
            const char* text = tmp("Show folder rule processes (%d)##byfolder", s->by_folder);
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(text, 0, true).x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::Checkbox(text, &s->show_by_folder)) {
                s->page = 0;
                s->stale = true;
            }
        }
    }
    settings_card_end();

    if (act == ACT_NONE) return;
    Str exe = str(act_exe);
    AppInfo* a = app_find(app, exe);
    CHECK(a, "the clicked app %s to be in the list", act_exe);
    switch (act) {
    case ACT_INCLUDE:
        remember_app(app, s, exe, "included");
        app_set_decision(app, exe, DECISION_INCLUDED);
        break;
    case ACT_EXCLUDE:
        remember_app(app, s, exe, "excluded");
        app_set_decision(app, exe, DECISION_EXCLUDED);
        break;
    case ACT_GAME: {
        // Game or not is separate from recording; "not a game" sticks even if a launcher lists it.
        bool game = a->game == GAME_YES;
        remember_app(app, s, exe, game ? "unmarked as a game" : "marked as a game");
        app_set_game(app, exe, game ? GAME_DISMISSED : GAME_YES);
    } break;
    case ACT_INCLUDE_FOLDER:
    case ACT_EXCLUDE_FOLDER: {
        Str path = str(a->path);
        i64 cut = path.len;
        while (cut > 0 && path.ptr[cut - 1] != '\\' && path.ptr[cut - 1] != '/') cut--;
        Mark m = mark(scratch());
        set_folder(app, s, str_copy(scratch(), str_sub(path, 0, cut)), act == ACT_INCLUDE_FOLDER ? DECISION_INCLUDED : DECISION_EXCLUDED);
        rewind(m);
    } break;
    case ACT_REMOVE:
        remember_app(app, s, exe, "removed from the list");
        app_remove(app, exe);
        {
        Str name = app_label(app, exe);
        ui_say(app, "Removed %.*s. It comes back under New if it plays sound again.", (int)name.len, name.ptr);
    }
        break;
    case ACT_REVEAL: {
        Mark m = mark(scratch());
        settings_open_path(app, wide(scratch(), str(a->path)), true);
        rewind(m);
    } break;
    case ACT_NONE: break;
    }
    if (act != ACT_REVEAL) s->stale = true;
}

// ---------------------------------------------------------------------------------------------
// Setting up the AI (setup.h)

// A path or URL field: shows the config's value while not being typed in, and calls back with
// what was typed once the typing ends.
static bool setting_field(const char* id, const char* hint, char* buf, i32 cap, bool* active, const char* value, f32 width) {
    if (!*active) set(buf, cap, str(value));
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint(id, hint, buf, cap);
    *active = ImGui::IsItemActive();
    return ImGui::IsItemDeactivatedAfterEdit() && !str_eq(str_trim(str(buf)), str(value));
}

static const ImVec4 ERROR_RED = ImVec4(0.86f, 0.33f, 0.31f, 1);

// The model file `name` is in the AI folder (as last looked).
static const AiFile* ai_file(App* app, SetupPart part, const char* name) {
    for (i32 i = 0; i < app->setup.file_count[part]; i++)
        if (strcmp(app->setup.files[part][i].name, name) == 0) return &app->setup.files[part][i];
    return 0;
}


// One line of the model dropdown: its name, what it is for, and its size (a tick once it is here).
static bool model_item(const char* text, const char* note, const char* size, bool on, f32 note_x, f32 right) {
    bool clicked = ImGui::Selectable(text, on);
    f32 size_w = ImGui::CalcTextSize(size).x;
    ImGui::SameLine(note_x);
    ImGui::TextColored(weak_text(), "%s", fit(note, right - size_w - note_x - px(16)));
    ImGui::SameLine(right - size_w);
    ImGui::TextColored(weak_text(), "%s", size);
    return clicked;
}

// "Model": a dropdown of the models lifelog can download, those already in the AI folder, and
// your own server; then how it is doing.
static void model_row(App* app, SettingsState* s, SetupPart part) {
    Config* c = &app->config;
    bool whisper = part == SETUP_TRANSCRIBER;
    const char* current = whisper ? c->worker.model : c->summary.model;
    bool yours = (whisper ? c->worker.endpoint : c->summary.endpoint)[0] || s->own[part];
    SetupState* st = &app->setup.part[part];
    const AiModel* models;
    i32 n = ai_models(part, &models);
    const AiModel* cur = ai_model(part, current);
    bool have = whisper ? whisper_ready(c->ai.dir, current) : summary_ai_ready(c->ai.dir, current);

    row_label("Model");
    ImGui::SetNextItemWidth(px(230));
    ImGui::SetNextWindowSizeConstraints(ImVec2(px(540), 0), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::BeginDisabled(st->running || app->setup.move.running);
    if (ImGui::BeginCombo(tmp("##model%d", (int)part), yours ? "Your own server" : cur ? cur->label : current)) {
        f32 note_x = px(180), right = ImGui::GetContentRegionMax().x;
        const char* chosen = 0;
        for (i32 i = 0; i < n; i++) {
            const AiModel* m = &models[i];
            const AiFile* f = ai_file(app, part, m->name);
            const char* size = f ? tmp("%s  %s", size_text((i64)f->bytes), ICON_CHECK) : tmp("%.1f GB  %s", m->gb, ICON_ARROW_DOWN);
            if (model_item(tmp("%s##m%d", m->label, i), m->note, size, !yours && strcmp(current, m->name) == 0, note_x, right)) chosen = m->name;
        }
        // Model files put in the folder by hand.
        for (i32 i = 0; i < app->setup.file_count[part]; i++) {
            const AiFile* f = &app->setup.files[part][i];
            if (ai_model(part, f->name)) continue;
            if (model_item(tmp("%s##f%d", f->name, i), "A file you added", tmp("%s  %s", size_text((i64)f->bytes), ICON_CHECK),
                           !yours && strcmp(current, f->name) == 0, note_x, right))
                chosen = f->name;
        }
        ImGui::Separator();
        if (model_item("Your own server", whisper ? "Any OpenAI-compatible transcription server" : "Ollama, LM Studio, or any OpenAI-compatible one", "",
                       yours, note_x, right))
            s->own[part] = true;
        ImGui::EndCombo();
        if (chosen) {
            s->own[part] = false;
            if (ai_file(app, part, chosen)) {
                if (whisper) worker_set_endpoint(app, str(""), str(chosen));
                else summary_set_endpoint(app, str(""), str(chosen));
            } else {
                setup_start(app, part, chosen);
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    f32 room = ImGui::GetContentRegionAvail().x;
    if (st->running) {
        weak(fit(st->text, room));
    } else if (st->failed) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ERROR_RED, "%s", fit(st->text, room));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st->text);
    } else if (!yours && !have) {
        const AiModel* get = cur ? cur : &models[0];
        if (button(label(ICON_ARROW_DOWN, tmp("Download, %.1f GB", get->gb)))) setup_start(app, part, get->name);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s and %s, from where they are published", get->label, whisper ? "whisper.cpp to run it" : "llama.cpp to run it");
    } else if (!yours && cur) {
        weak(fit(cur->note, room));
    }
}

// "Server": the address and model name of your own, when that is picked.
static void own_server_row(App* app, SettingsState* s, SetupPart part) {
    Config* c = &app->config;
    row_label("Server");
    f32 model_w = px(140);
    f32 url_w = ImGui::GetContentRegionAvail().x - model_w - ImGui::GetStyle().ItemSpacing.x;
    if (part == SETUP_SUMMARY_AI) {
        bool url_done = setting_field("##endpoint", "http://localhost:11434/v1 (OpenAI-compatible)", s->endpoint, sizeof(s->endpoint),
                                      &s->endpoint_active, c->summary.endpoint, url_w);
        ImGui::SameLine();
        bool model_done = setting_field("##model", "model", s->model, sizeof(s->model), &s->model_active, c->summary.model, model_w);
        if (url_done || model_done) summary_set_endpoint(app, str(url_done ? s->endpoint : c->summary.endpoint), str(model_done ? s->model : ""));
    } else {
        bool url_done = setting_field("##wendpoint", "http://localhost:8000/v1 (OpenAI-compatible)", s->wendpoint, sizeof(s->wendpoint),
                                      &s->wendpoint_active, c->worker.endpoint, url_w);
        ImGui::SameLine();
        bool model_done = setting_field("##wmodel", "model", s->wmodel, sizeof(s->wmodel), &s->wmodel_active, c->worker.model, model_w);
        if (url_done || model_done) worker_set_endpoint(app, str(url_done ? s->wendpoint : c->worker.endpoint), str(model_done ? s->wmodel : ""));
    }
}

// One of `names` picked, as a segmented control on a darker strip. The index clicked (other than
// `on`), or -1.
static i32 segments(const char* id, const char* const* names, i32 n, i32 on, f32 seg_w) {
    ImVec2 pad = ImGui::GetStyle().FramePadding;
    f32 inset = px(3);
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + seg_w * n + inset * 2, at.y + ImGui::GetFrameHeight()),
                                              ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad.x, pad.y - inset));
    i32 clicked = -1;
    for (i32 i = 0; i < n; i++) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + inset + seg_w * i, at.y + inset));
        if (pick(tmp("%s##%s", names[i], id), on == i, ImVec2(seg_w, 0)) && on != i) clicked = i;
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy(ImVec2(seg_w * n + inset * 2, ImGui::GetFrameHeight()));
    return clicked;
}

// ---------------------------------------------------------------------------------------------
// Transcription

static void transcription_card(App* app, SettingsState* s, f32 width) {
    Config* c = &app->config;
    settings_card("transcription", width);
    title("Transcription");
    f64 queued = app->worker.queued_secs / 60.0;
    status_right(queued >= 1 ? tmp("%s · %.0f min waiting", app->worker.state, queued) : app->worker.state);

    row_label("Run");
    const char* names[] = {"Auto", "Always", "Paused"};
    i32 m = segments("mode", names, 3, (i32)c->processing.mode, px(78));
    if (m >= 0) {
        SettingsUndo* u = remember(s, UNDO_MODE, "transcription %s", names[m]);
        u->n = (u32)c->processing.mode;
        worker_set_mode(app, (Mode)m);
    }
    ImGui::SameLine();
    weak(fit(c->processing.mode == MODE_AUTO ? "waits for a free GPU" : c->processing.mode == MODE_ALWAYS ? "even during games" : "still records, transcribes later",
             ImGui::GetContentRegionAvail().x));

    if (c->processing.mode == MODE_AUTO) {
        row_label("Games");
        bool games = c->processing.pause_for_games;
        if (ImGui::Checkbox("Pause while one is running", &games)) {
            SettingsUndo* u = remember(s, UNDO_PAUSE_GAMES, games ? "pause for games on" : "pause for games off");
            u->on = !games;
            worker_set_pause_for_games(app, games);
        }
        row_label("Graphics card");
        ImGui::TextUnformatted("Wait while busier than");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(64));
        ImGui::DragInt("##gpu", &s->gpu, 0.2f, 10, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp);
        // Saved when the drag or the typing ends, not on every frame of it.
        if (ImGui::IsItemDeactivatedAfterEdit() && (u32)s->gpu != c->processing.gpu_busy_percent) {
            SettingsUndo* u = remember(s, UNDO_GPU, "graphics card limit %d %%", s->gpu);
            u->n = c->processing.gpu_busy_percent;
            worker_set_gpu_limit(app, (u32)s->gpu);
        }
        if (!ImGui::IsItemActive()) s->gpu = (i32)c->processing.gpu_busy_percent;
        ImGui::SameLine();
        weak(tmp("now %u%% 3D", app->worker.gpu_3d_percent));
        if (ImGui::IsItemHovered()) {
            // The other readings: compute does not pause transcription; memory does, when short.
            LONG free_mb = atomic_get(&app->worker.gpu_free_mb);
            const char* memory = free_mb < 0 ? "not known until the model has loaded"
                                             : tmp("%.1f GB free beside other programs, the model needs %.1f",
                                                   free_mb / 1024.0, atomic_get(&app->worker.gpu_need_mb) / 1024.0);
            ImGui::SetTooltip("Other programs on the graphics card Whisper uses:\n3D (games) %ld%%: over the limit, transcription waits\n"
                              "Compute %ld%%: shared, does not pause it\nMemory: %s",
                              atomic_get(&app->worker.gpu_3d_percent), atomic_get(&app->worker.gpu_compute_percent), memory);
        }
    }

    // lifelog's own whisper.cpp, or any OpenAI-compatible transcription server.
    model_row(app, s, SETUP_TRANSCRIBER);
    if (c->worker.endpoint[0] || s->own[SETUP_TRANSCRIBER]) own_server_row(app, s, SETUP_TRANSCRIBER);
    settings_card_end();
}

// ---------------------------------------------------------------------------------------------
// Voice commands (voice.h), and the chimes they make (chime.h).

static void voice_card(App* app, SettingsState* s, f32 width) {
    Config* c = &app->config;
    Voice* v = &app->voice;
    settings_card("voice", width);

    bool on = c->voice.listen;
    if (toggle("##voice", &on)) {
        SettingsUndo* u = remember(s, UNDO_VOICE, on ? "voice commands on" : "voice commands off");
        u->on = !on;
        voice_set_listen(app, on);
    }
    ImGui::SameLine();
    title("Voice commands");
    ImGui::SameLine(0, px(10));
    SetupState* st = &app->setup.part[SETUP_TRANSCRIBER];
    VoiceState state = (VoiceState)atomic_get(&v->state);
    f32 room = ImGui::GetContentRegionAvail().x;
    if (!on) {
        weak(fit("off", room));
    } else if (st->running) {
        weak(fit(st->text, room));
    } else if (state == VOICE_NEEDS_SETUP) {
        ImGui::BeginDisabled(app->setup.move.running);
        if (button(label(ICON_ARROW_DOWN, "Download, 0.15 GB"))) setup_start_voice(app);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Whisper base.en, a small English model that runs on the processor");
    } else if (state == VOICE_FAILED) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ERROR_RED, "%s", fit("could not load its model (see the log)", room));
    } else {
        weak(fit(state == VOICE_READY ? "listening" : "starting", room));
    }

    row_label("Wake word");
    const char* wake = c->voice.wake[0] ? c->voice.wake : "lifelog";
    if (setting_field("##wake", "lifelog", s->wake, sizeof(s->wake), &s->wake_active, wake, px(160))) {
        char old[64];
        SET(old, str(wake));
        if (voice_set_wake(app, str_trim(str(s->wake)))) {
            SettingsUndo* u = remember(s, UNDO_WAKE, "wake word \"%s\"", c->voice.wake);
            SET(u->text, str(old));
        }
        s->wake_active = false;
    }
    ImGui::SameLine(0, px(12));
    weak(fit(tmp("then a command: \"%s, clip the last hour\"", wake), ImGui::GetContentRegionAvail().x));

    row_label("Commands");
    if (button(s->voice_list ? label(ICON_CHEVRON_UP, "Hide") : label(ICON_CHEVRON_DOWN, "Show all")))
        s->voice_list = !s->voice_list;
    if (s->voice_list) {
        struct { const char* say; const char* does; } cmds[] = {
            {"clip the last hour", "copy that part of the transcript (or 15 minutes, today)"},
            {"summarise the last 30 minutes", "a few sentences from the summary AI, on the clipboard"},
            {"remind me in 10 minutes to ...", "a reminder (or \"at 5 pm\", \"tomorrow at 9\", \"set an alarm for 7\")"},
            {"open Spotify", "start an installed app or game"},
            {"pause  /  resume", "private mode on or off"},
            {"pause  /  continue transcription", "hold transcribing, then back to how it was"},
            {"check the AI", "start the summary AI and test it"},
            {"never mind", "cancel after the chime"},
        };
        ImGui::Indent(px(ROW_LABEL_W));
        if (ImGui::BeginTable("voice_cmds", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("say", ImGuiTableColumnFlags_WidthFixed, px(250));
            ImGui::TableSetupColumn("does", ImGuiTableColumnFlags_WidthStretch);
            for (i32 i = 0; i < COUNT(cmds); i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(cmds[i].say);
                ImGui::TableNextColumn();
                ImGui::TextColored(weak_text(), "%s", cmds[i].does);
            }
            ImGui::EndTable();
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(weak_text(), "Say it as one sentence, or say \"%s\" and wait for the chime. In private mode only resume works.", wake);
        ImGui::PopTextWrapPos();
        ImGui::Unindent(px(ROW_LABEL_W));
    }

    if (v->heard_ts) {
        row_label("Last");
        LocalTime t = local_time(v->heard_ts);
        weak(fit(tmp("%02d:%02d  %s: %s", t.hour, t.minute, v->heard, v->result), ImGui::GetContentRegionAvail().x));
    }

    row_label("Chimes");
    const char* names[] = {"Glass", "Soft", "Synth", "Off"};
    i32 style = segments("chimes", names, COUNT(names), (i32)c->chimes.style, px(70));
    if (style >= 0) {
        SettingsUndo* u = remember(s, UNDO_CHIME_STYLE, "chimes %s", names[style]);
        u->n = (u32)c->chimes.style;
        chime_set_style(app, (ChimeStyle)style);
        chime_play(app, CHIME_DONE);
    }
    ImGui::SameLine();
    const char* hints[] = {"a warm bell, like an electric piano", "a low marimba", "a mellow synth", "silent"};
    weak(fit(hints[c->chimes.style], ImGui::GetContentRegionAvail().x));

    bool off = c->chimes.style == CHIME_OFF;
    row_label("Chime volume");
    ImGui::BeginDisabled(off);
    ImGui::SetNextItemWidth(px(64));
    ImGui::DragInt("##chime_volume", &s->chime_volume, 0.3f, 0, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp);
    // Saved, and played so you hear it, when the drag or the typing ends.
    if (ImGui::IsItemDeactivatedAfterEdit() && (u32)s->chime_volume != c->chimes.volume) {
        SettingsUndo* u = remember(s, UNDO_CHIME_VOLUME, "chime volume %d %%", s->chime_volume);
        u->n = c->chimes.volume;
        chime_set_volume(app, (u32)s->chime_volume);
        chime_play(app, CHIME_DONE);
    }
    if (!ImGui::IsItemActive()) s->chime_volume = (i32)c->chimes.volume;
    ImGui::EndDisabled();

    // Each chime, to hear it; wrapped like words when they do not fit on one line.
    row_label("Listen");
    ImGui::BeginDisabled(off);
    f32 left = ImGui::GetCursorPosX();
    f32 right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (i32 i = 0; i < CHIME_COUNT; i++) {
        const char* text = label(ICON_PLAY, tmp("%s##chime%d", chime_label((Chime)i), i));
        wrap_button(i, text, left, right);
        if (ImGui::Button(text)) chime_play(app, (Chime)i);
    }
    ImGui::EndDisabled();
    settings_card_end();
}

// ---------------------------------------------------------------------------------------------
// Daily summary

static void summary_card(App* app, SettingsState* s, f32 width) {
    Config* c = &app->config;
    settings_card("summary", width);

    bool on = c->summary.enabled;
    if (toggle("##enabled", &on)) {
        SettingsUndo* u = remember(s, UNDO_ENABLED, on ? "daily summary on" : "daily summary off");
        u->on = !on;
        summary_set_enabled(app, on);
    }
    ImGui::SameLine();
    title("Daily summary");

    // Edit prompt at the right of the title, and how it is doing between, cut to fit.
    const char* edit = label(ICON_FILE_TEXT, "Edit prompt");
    f32 edit_w = ImGui::CalcTextSize(edit).x + ImGui::GetStyle().FramePadding.x * 2;
    f32 buttons_x = ImGui::GetContentRegionMax().x - edit_w;
    if (app->summary.phase[0]) {
        ImGui::SameLine(0, px(10));
        f32 room = buttons_x - ImGui::GetCursorPosX() - px(10);
        if (room > px(40)) weak(fit(app->summary.phase, room));
    }
    ImGui::SameLine(buttons_x);
    // Opened from the pool, as the other files are: the shell may not open it from this thread.
    if (ImGui::Button(edit)) {
        const wchar_t* path = summary_prompt_file(app);
        if (path) settings_open_path(app, path, false);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The AI's instructions for the summary, in summary-prompt.txt.\n"
                          "Changes count from the next summary. Delete the file to go back to the built-in text.");

    row_label("Ask at");
    if (!s->ask_at_active) SET(s->ask_at, str(c->summary.ask_at));
    ImGui::SetNextItemWidth(px(56));
    ImGui::InputText("##ask_at", s->ask_at, sizeof(s->ask_at));
    s->ask_at_active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Str at = str_trim(str(s->ask_at));
        if (!str_eq(at, str(c->summary.ask_at))) {
            char before[8];
            SET(before, str(c->summary.ask_at));
            summary_set_ask_at(app, at);
            // Remembered only if it took (a bad time is refused with a message).
            if (strcmp(before, c->summary.ask_at) != 0) {
                SettingsUndo* u = remember(s, UNDO_ASK_AT, "ask at %s", c->summary.ask_at);
                SET(u->text, str(before));
            }
        }
    }
    row_label("Length");
    ImGui::TextUnformatted("At most");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(100));
    ImGui::DragInt("##words", &s->max_words, 10, 50, 2000, "%d words", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit() && (u32)s->max_words != c->summary.max_words) {
        SettingsUndo* u = remember(s, UNDO_WORDS, "at most %d words", s->max_words);
        u->n = c->summary.max_words;
        summary_set_max_words(app, (u32)s->max_words);
    }
    if (!ImGui::IsItemActive()) s->max_words = (i32)c->summary.max_words;

    // lifelog's own llama.cpp, or any OpenAI-compatible server (Ollama, LM Studio, a cloud one).
    model_row(app, s, SETUP_SUMMARY_AI);
    if (c->summary.endpoint[0] || s->own[SETUP_SUMMARY_AI]) own_server_row(app, s, SETUP_SUMMARY_AI);

    row_label("Reminders");
    bool remind = c->summary.ask_reminders;
    if (toggle("##reminders", &remind)) {
        SettingsUndo* u = remember(s, UNDO_REMINDERS, remind ? "reminder notifications on" : "reminder notifications off");
        u->on = !remind;
        summary_set_ask_reminders(app, remind);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(fit("Offer a notification when I say I'll do something later", ImGui::GetContentRegionAvail().x));

    List<NeverTopic>* never = &app->summary.never;
    if (never->count) {
        row_label("Never mention");
        i64 remove = 0;
        f32 left = ImGui::GetCursorPosX();
        f32 right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (i32 i = 0; i < never->count; i++) {
            NeverTopic* n = &(*never)[i];
            const char* text = tmp("%s  %s##never%lld", n->topic, ICON_X, n->id);
            wrap_button(i, text, left, right);
            if (ImGui::Button(text)) remove = n->id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Allow again");
        }
        if (remove) {
            for (i32 i = 0; i < never->count; i++) {
                if ((*never)[i].id != remove) continue;
                SettingsUndo* u = remember(s, UNDO_NEVER, "allowed %s", (*never)[i].topic);
                SET(u->text, str((*never)[i].topic));
                break;
            }
            summary_remove_never(app, remove);
        }
    }
    settings_card_end();
}

// ---------------------------------------------------------------------------------------------
// Files

static void files_card(App* app, f32 width) {
    settings_card("files", width);
    title("Files");
    if (app->config.data_dir[0]) status_right(app->config.data_dir);
    Mark m = mark(scratch());
    const wchar_t* targets[3] = {wide(scratch(), str(app->config.data_dir)), log_path(), config_path()};
    const char* icons[3] = {ICON_FOLDER_OPEN, ICON_FILE_TEXT, ICON_SETTINGS};
    const char* texts[3] = {"Recordings", "Log", "Config"};
    for (i32 i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        bool has = targets[i] && targets[i][0];
        ImGui::BeginDisabled(!has);
        if (ImGui::Button(label(icons[i], texts[i]))) settings_open_path(app, targets[i], i > 0);
        ImGui::EndDisabled();
        if (has && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(i ? "Show %s in its folder" : "%s", narrow(scratch(), targets[i]).ptr);
    }
    rewind(m);
    settings_card_end();
}

// ---------------------------------------------------------------------------------------------
// AI models: what lifelog downloaded, where, and taking it off again (the uninstaller leaves it).

static void ai_card(App* app, SettingsState* s, f32 width) {
    Setup* su = &app->setup;
    settings_card("ai", width);
    title("AI models");
    status_right(su->total_bytes > 0 ? tmp("%s on this computer", size_text((i64)su->total_bytes)) : "none downloaded");
    bool busy = su->move.running;
    for (i32 i = 0; i < SETUP_PARTS; i++) busy = busy || su->part[i].running;

    // The folder, with Change… and Open at the right.
    if (!g_picking && g_picked[0] && g_pick_ai) {
        setup_move(app, str(g_picked));
        g_picked[0] = 0;
        g_pick_ai = false;
    }
    row_label("Folder");
    const char* change = label(ICON_FOLDER, "Change…");
    const char* open = label(ICON_FOLDER_OPEN, "Open");
    ImGuiStyle& st = ImGui::GetStyle();
    f32 change_w = ImGui::CalcTextSize(change).x + st.FramePadding.x * 2;
    f32 open_w = ImGui::CalcTextSize(open).x + st.FramePadding.x * 2;
    f32 buttons_x = ImGui::GetContentRegionMax().x - open_w - st.ItemSpacing.x - change_w;
    if (su->move.running) {
        weak(fit(su->move.text, buttons_x - ImGui::GetCursorPosX() - px(8)));
    } else if (su->move.failed) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ERROR_RED, "%s", fit(su->move.text, buttons_x - ImGui::GetCursorPosX() - px(8)));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", su->move.text);
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(fit(app->config.ai.dir, buttons_x - ImGui::GetCursorPosX() - px(8)));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", app->config.ai.dir);
    }
    ImGui::SameLine(buttons_x);
    ImGui::BeginDisabled(busy || g_picking || app->summary.writing);
    if (ImGui::Button(change)) {
        g_pick_ai = true;
        g_picked[0] = 0;
        g_picking = true;
        Job* job = job_new(folder_pick_apply);
        PickJob* j = PUSH(&job->arena, PickJob);
        j->ai = true;
        job->data = j;
        pool_post(job, folder_pick_run);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Move the AI files to another folder or drive");
    ImGui::SameLine();
    if (ImGui::Button(open)) {
        Mark m = mark(scratch());
        settings_open_path(app, wide(scratch(), str(app->config.ai.dir)), false);
        rewind(m);
    }

    // Each downloaded model, with a button to delete it.
    const char* names[SETUP_PARTS] = {"Transcription", "Summary"};
    bool ask = false;
    for (i32 p = 0; p < SETUP_PARTS; p++) {
        row_label(names[p]);
        if (!su->file_count[p]) {
            weak("No models downloaded");
            continue;
        }
        f32 left = ImGui::GetCursorPosX();
        f32 right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (i32 i = 0; i < su->file_count[p]; i++) {
            AiFile* f = &su->files[p][i];
            const char* text = tmp("%s  %s  %s##rm%d_%d", f->name, size_text((i64)f->bytes), ICON_TRASH, p, i);
            wrap_button(i, text, left, right);
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(text)) {
                s->remove_part = (SetupPart)p;
                SET(s->remove_model, str(f->name));
                s->remove_all = false;
                ask = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete this model");
        }
    }

    row_label("Uninstall");
    ImGui::BeginDisabled(busy || su->total_bytes <= 0);
    if (ImGui::Button(label(ICON_TRASH, "Remove all"))) {
        s->remove_all = true;
        ask = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    weak(fit("Uninstalling lifelog leaves these here", ImGui::GetContentRegionAvail().x));

    if (ask) ImGui::OpenPopup("##ai_remove");
    if (ImGui::BeginPopup("##ai_remove")) {
        f64 bytes = su->total_bytes;
        if (!s->remove_all) {
            const AiFile* f = ai_file(app, s->remove_part, s->remove_model);
            bytes = f ? f->bytes : 0;
        }
        ImGui::TextUnformatted(s->remove_all ? tmp("Remove all of lifelog's AI (%s)?", size_text((i64)bytes)) : tmp("Delete %s (%s)?", s->remove_model, size_text((i64)bytes)));
        ImGui::TextColored(weak_text(), "%s",
                           s->remove_all ? "Both models and engines. Recordings and transcripts stay; Settings can download it again."
                                         : "Settings can download it again.");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.24f, 0.22f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.30f, 0.27f, 1));
        if (ImGui::Button(label(ICON_TRASH, "Delete"))) {
            if (s->remove_all) setup_remove_all(app);
            else setup_remove(app, s->remove_part, s->remove_model);
            ai_scan(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    settings_card_end();
}

// ---------------------------------------------------------------------------------------------

static void settings_draw(App* app, SettingsState* s, f32* next_frame_ms) {
    // Ctrl+Z, unless a text box has the keys (it has its own undo).
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo_last(app, s);

    // Opening Settings looks again at everything; after that, only at what changed.
    bool opened = s->drawn_frame != ImGui::GetFrameCount() - 1;
    s->drawn_frame = ImGui::GetFrameCount();
    if (opened || s->stale || s->loaded_version != app->apps.version) apps_load(app, s);
    // What the AI folder has: setup's changes ask for a look (scanned_ts = 0). Files moved in by
    // hand show the next time Settings is opened.
    if (opened || !app->setup.scanned_ts) ai_scan(app);

    f32 avail = ImGui::GetContentRegionAvail().x;
    f32 width = avail < px(760) ? avail : px(760);
    f32 x = ImGui::GetCursorPosX() + (avail - width) / 2;
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetCursorPosX(x);
    apps_card(app, s, width);
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetCursorPosX(x);
    transcription_card(app, s, width);
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetCursorPosX(x);
    summary_card(app, s, width);
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetCursorPosX(x);
    voice_card(app, s, width);
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetCursorPosX(x);
    files_card(app, width);
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::SetCursorPosX(x);
    ai_card(app, s, width);
    ImGui::Dummy(ImVec2(0, px(16)));

    // A change shows at once: the apps page is made again on the next frame.
    if (s->stale) *next_frame_ms = 0;
}

static void settings_free(SettingsState* s) {
    list_free(&s->undo);
}
