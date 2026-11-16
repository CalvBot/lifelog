// ui_settings.cpp: the Settings tab: which apps are recorded and which
// are games, transcription, the daily summary, and where files live. Ctrl+Z takes back the last
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
    f64 loaded_ts;
    i32 loaded_apps;  // App.apps.list.count when it was made
    char search[128];
    bool show_by_folder;  // list apps that only follow a folder rule too
    i32 by_folder;        // how many of those the list on screen has (shown or not)

    // The fields being typed in or dragged; set from the config while not being edited.
    char ask_at[16];
    i32 max_words;
    i32 gpu;
    bool ask_at_active;
    char wendpoint[512];  // transcription's own server
    bool wendpoint_active;
    char wmodel[128];
    bool wmodel_active;
    char endpoint[512];
    bool endpoint_active;
    char model[128];
    bool model_active;

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

// A card `width` wide (ui_common's card_begin always takes the whole window's width).
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

// `text` cut to fit `width`, with "…" when it was cut. Into scratch.
static const char* fit(const char* text, f32 width) {
    if (ImGui::CalcTextSize(text).x <= width) return text;
    Str s = str(text);
    f32 dots = ImGui::CalcTextSize("…").x;
    i64 len = s.len;
    while (len > 0) {
        len--;
        while (len > 0 && (s.ptr[len] & 0xC0) == 0x80) len--;  // whole UTF-8 characters only
        if (ImGui::CalcTextSize(s.ptr, s.ptr + len).x + dots <= width) break;
    }
    return str_fmt(scratch(), "%.*s…", (int)len, s.ptr).ptr;
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
    INT_PTR ok;
    if (o->select) {
        wchar_t args[MAX_PATH + 16];
        swprintf(args, COUNT(args), L"/select,\"%s\"", o->path);
        ok = (INT_PTR)ShellExecuteW(0, L"open", L"explorer.exe", args, 0, SW_SHOWNORMAL);
    } else {
        ok = (INT_PTR)ShellExecuteW(0, L"open", o->path, 0, 0, SW_SHOWNORMAL);
    }
    o->ok = ok > 32;
}

static void open_path_apply(App* app, Job* job) {
    OpenPathJob* o = (OpenPathJob*)job->data;
    if (o->ok) return;
    Str path = narrow(scratch(), o->path);
    WARN("could not open %s", path.ptr);
    ui_say(app, "Could not open %s", path.ptr);
}

static void open_path(App*, const wchar_t* path, bool select) {
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
    Str name = app_name(app, exe);
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
    case UNDO_NEVER: summary_add_never(app, str(u.text), 0, {}); break;
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
    s->loaded_ts = now;
    s->loaded_apps = app->apps.list.count;
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
static char g_picked[MAX_PATH];

struct PickJob {
    Decision decision;
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
        d->SetTitle(j->decision == DECISION_INCLUDED ? L"Include every app in a folder" : L"Exclude every app in a folder");
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
    if (!g_picking && g_picked[0]) {
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
    if (used < full) ImGui::Dummy(ImVec2(0, full - used - ImGui::GetStyle().ItemSpacing.y));
    ImGui::BeginDisabled(g_picking);
    for (i32 k = 0; k < 2; k++) {
        if (k) ImGui::SameLine();
        if (ImGui::Button(label(ICON_PLUS, k ? "Exclude a folder" : "Include a folder"))) {
            g_pick_decision = k ? DECISION_EXCLUDED : DECISION_INCLUDED;
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
    else if (act_folder[0]) open_path(app, wide(scratch(), str(act_folder)), false);
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
        const char* name = str_copy(scratch(), app_name(app, exe)).ptr;
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
        if (used < full) ImGui::Dummy(ImVec2(0, full - used - ImGui::GetStyle().ItemSpacing.y));
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
        Str name = app_name(app, exe);
        ui_say(app, "Removed %.*s. It comes back under New if it plays sound again.", (int)name.len, name.ptr);
    }
        break;
    case ACT_REVEAL: {
        Mark m = mark(scratch());
        open_path(app, wide(scratch(), str(a->path)), true);
        rewind(m);
    } break;
    case ACT_NONE: break;
    }
    if (act != ACT_REVEAL) s->stale = true;
}

// ---------------------------------------------------------------------------------------------
// Setting up the AI (setup.h)

// "Set up" while `part` is missing: how far it has got while it runs, why it failed after.
static void setup_row(App* app, SetupPart part, const char* what, const char* size) {
    SetupState* st = &app->setup.part[part];
    if (st->running) {
        weak(st->text);
        return;
    }
    if (ImGui::Button(tmp("%s##setup%d", label(ICON_ARROW_DOWN, st->failed ? "Try again" : "Set up"), (int)part))) setup_start(app, part);
    ImGui::SameLine();
    if (st->failed) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.86f, 0.33f, 0.31f, 1), "%s", st->text);
        ImGui::PopTextWrapPos();
    } else {
        ImGui::AlignTextToFramePadding();
        weak(tmp("Downloads %s (%s) from where it is published", what, size));
    }
}

// A path or URL field: shows the config's value while not being typed in, and calls back with
// what was typed once the typing ends.
static bool setting_field(const char* id, const char* hint, char* buf, i32 cap, bool* active, const char* value, f32 width) {
    if (!*active) set(buf, cap, str(value));
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint(id, hint, buf, cap);
    *active = ImGui::IsItemActive();
    return ImGui::IsItemDeactivatedAfterEdit() && !str_eq(str_trim(str(buf)), str(value));
}

// ---------------------------------------------------------------------------------------------
// Transcription

static void transcription_card(App* app, SettingsState* s, f32 width) {
    Config* c = &app->config;
    settings_card("transcription", width);
    title("Transcription");
    const char* names[] = {"Auto", "Always", "Paused"};
    for (i32 m = 0; m < 3; m++) {
        if (m) ImGui::SameLine(0, px(4));
        if (pick(tmp("%s##mode", names[m]), c->processing.mode == m) && c->processing.mode != m) {
            SettingsUndo* u = remember(s, UNDO_MODE, "transcription %s", names[m]);
            u->n = (u32)c->processing.mode;
            worker_set_mode(app, (Mode)m);
        }
    }
    ImGui::SameLine(0, px(12));
    f64 queued = app->worker.queued_secs / 60.0;
    weak(queued >= 1 ? tmp("%s · %.0f min waiting", app->worker.state, queued) : app->worker.state);

    if (c->processing.mode == MODE_AUTO) {
        bool games = c->processing.pause_for_games;
        if (ImGui::Checkbox("Pause while games are running", &games)) {
            SettingsUndo* u = remember(s, UNDO_PAUSE_GAMES, games ? "pause for games on" : "pause for games off");
            u->on = !games;
            worker_set_pause_for_games(app, games);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Wait while the graphics card is busier than");
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
        weak(tmp("now %u%%", app->worker.gpu_3d_percent));
    }

    // lifelog's own whisper.cpp, or any OpenAI-compatible transcription server.
    if (!c->worker.endpoint[0] && (app->worker.missing || app->setup.part[SETUP_TRANSCRIBER].running))
        setup_row(app, SETUP_TRANSCRIBER, "whisper.cpp and Whisper large-v3-turbo", "about 2.3 GB");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Your own server");
    ImGui::SameLine();
    f32 model_w = px(140);
    bool url_done = setting_field("##wendpoint", "http://localhost:8000/v1 (OpenAI-compatible)", s->wendpoint, sizeof(s->wendpoint),
                                  &s->wendpoint_active, c->worker.endpoint, ImGui::GetContentRegionAvail().x - model_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SameLine();
    bool model_done = setting_field("##wmodel", "model", s->wmodel, sizeof(s->wmodel), &s->wmodel_active, c->worker.model, model_w);
    if (url_done || model_done) worker_set_endpoint(app, str(url_done ? s->wendpoint : c->worker.endpoint), str(model_done ? s->wmodel : ""));
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
    if (app->summary.phase[0]) {
        ImGui::SameLine();
        weak(app->summary.phase);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Ask at");
    ImGui::SameLine();
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
    ImGui::SameLine();
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

    const char* edit = label(ICON_FILE_TEXT, "Edit prompt");
    const char* write = label(ICON_SCROLL_TEXT, "Write today's now");
    ImGuiStyle& st = ImGui::GetStyle();
    f32 edit_w = ImGui::CalcTextSize(edit).x + st.FramePadding.x * 2;
    f32 write_w = ImGui::CalcTextSize(write).x + st.FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - write_w - st.ItemSpacing.x - edit_w);
    if (ImGui::Button(edit)) summary_edit_prompt(app);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The AI's instructions for the summary, in summary-prompt.txt.\n"
                          "Changes count from the next summary. Delete the file to go back to the built-in text.");
    ImGui::SameLine();
    if (ImGui::Button(write)) {
        Str result = summary_act(app, scratch(), "now", 0, 0);
        ui_say(app, "%s", str_eq(result, str("writing")) ? "Writing today's summary" : result.ptr);
    }

    // lifelog's own llama.cpp, or any OpenAI-compatible server (Ollama, LM Studio, a cloud one).
    if (!c->summary.endpoint[0] && (!summary_ai_ready(c->summary.model) || app->setup.part[SETUP_SUMMARY_AI].running))
        setup_row(app, SETUP_SUMMARY_AI, "llama.cpp and Qwen3 14B", "about 10 GB");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Your own AI server");
    ImGui::SameLine();
    f32 model_w = px(140);
    bool url_done = setting_field("##endpoint", "http://localhost:11434/v1 (OpenAI-compatible)", s->endpoint, sizeof(s->endpoint), &s->endpoint_active,
                                  c->summary.endpoint, ImGui::GetContentRegionAvail().x - model_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SameLine();
    bool model_done = setting_field("##model", "model", s->model, sizeof(s->model), &s->model_active, c->summary.model, model_w);
    if (url_done || model_done) summary_set_endpoint(app, str(url_done ? s->endpoint : c->summary.endpoint), str(model_done ? s->model : ""));

    bool remind = c->summary.ask_reminders;
    if (toggle("##reminders", &remind)) {
        SettingsUndo* u = remember(s, UNDO_REMINDERS, remind ? "reminder notifications on" : "reminder notifications off");
        u->on = !remind;
        summary_set_ask_reminders(app, remind);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Offer a notification when I say I'll do something later");

    List<NeverTopic>* never = &app->summary.never;
    if (never->count) {
        ImGui::PushFont(0, small_size());
        ImGui::TextColored(weak_text(), "Never mention");
        ImGui::PopFont();
        i64 remove = 0;
        f32 right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (i32 i = 0; i < never->count; i++) {
            NeverTopic* n = &(*never)[i];
            const char* text = tmp("%s  %s##never%lld", n->topic, ICON_X, n->id);
            f32 w = ImGui::CalcTextSize(text, 0, true).x + ImGui::GetStyle().FramePadding.x * 2;
            // Wrapped like words: on this line if it fits, else the next.
            if (i > 0) {
                ImGui::SameLine();
                if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
            }
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
    Mark m = mark(scratch());
    const wchar_t* targets[3] = {wide(scratch(), str(app->config.data_dir)), log_path(), config_path()};
    const char* icons[3] = {ICON_FOLDER_OPEN, ICON_FILE_TEXT, ICON_SETTINGS};
    const char* texts[3] = {"Recordings", "Log", "Config"};
    for (i32 i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        bool has = targets[i] && targets[i][0];
        ImGui::BeginDisabled(!has);
        if (ImGui::Button(label(icons[i], texts[i]))) open_path(app, targets[i], false);
        ImGui::EndDisabled();
        if (has && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", narrow(scratch(), targets[i]).ptr);
    }
    if (app->config.data_dir[0]) {
        ImGui::PushFont(0, small_size());
        ImGui::TextColored(weak_text(), "%s", app->config.data_dir);
        ImGui::PopFont();
    }
    rewind(m);
    settings_card_end();
}

// ---------------------------------------------------------------------------------------------

static void settings_draw(App* app, SettingsState* s, f32* next_frame_ms) {
    // Ctrl+Z, unless a text box has the keys (it has its own undo).
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo_last(app, s);

    f64 now = now_ts();
    if (s->stale || now - s->loaded_ts > 10 || s->loaded_apps != app->apps.list.count) apps_load(app, s);
    f32 next = (f32)((s->loaded_ts + 10 - now) * 1000) + 10;
    if (*next_frame_ms < 0 || *next_frame_ms > next) *next_frame_ms = next;

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
    files_card(app, width);
    ImGui::Dummy(ImVec2(0, px(16)));

    // A change shows at once: the apps page is made again on the next frame.
    if (s->stale) *next_frame_ms = 0;
}

static void settings_free(SettingsState* s) {
    list_free(&s->undo);
}
