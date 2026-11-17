// apps.cpp: see apps.h. Which processes run, which game runs and where each exe lives come from
// the capture watcher's scan (apps_seen, once a second); then the icons of apps that are open
// are found on the pool, and every minute or so the database thread counts the lines heard per
// app. The "apps-scan" thread reads paths.txt at start, then at start and every six hours the
// Start menu and the game launchers' records; it posts what it found to the main thread.

#include <tlhelp32.h>
#include <shobjidl.h>

#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")

static const char* OWN_EXES[] = {"lifelog.exe", "lifelogw.exe"};

bool is_own_exe(Str exe) {
    for (i32 i = 0; i < COUNT(OWN_EXES); i++)
        if (str_eq_nocase(exe, str(OWN_EXES[i]))) return true;
    return false;
}

AppInfo* app_find(App* app, Str exe) {
    for (i32 i = 0; i < app->apps.list.count; i++)
        if (str_eq_nocase(str(app->apps.list[i].exe), exe)) return &app->apps.list[i];
    return 0;
}

AppInfo* app_get(App* app, Str exe) {
    AppInfo* a = app_find(app, exe);
    if (a) return a;
    a = list_push(&app->apps.list);
    app->apps.version++;
    Mark m = mark(scratch());
    SET(a->exe, str_lower(scratch(), exe));
    rewind(m);
    const char* path = app_path(app, exe);
    if (path) SET(a->path, str(path));
    return a;
}

const char* app_path(App* app, Str exe) {
    for (i32 i = 0; i < app->apps.paths.count; i++)
        if (str_eq_nocase(str(app->apps.paths[i].exe), exe)) return app->apps.paths[i].path;
    return 0;
}

void apps_from_config(App* app) {
    Config* c = &app->config;
    app->apps.version++;
    for (i32 i = 0; i < c->capture.included_apps.count; i++) app_get(app, str(c->capture.included_apps[i].s))->decision = DECISION_INCLUDED;
    for (i32 i = 0; i < c->capture.excluded_apps.count; i++) app_get(app, str(c->capture.excluded_apps[i].s))->decision = DECISION_EXCLUDED;
    for (i32 i = 0; i < c->processing.games.count; i++) app_get(app, str(c->processing.games[i].s))->game = GAME_YES;
    for (i32 i = 0; i < c->processing.not_games.count; i++) app_get(app, str(c->processing.not_games[i].s))->game = GAME_DISMISSED;
}

void app_set_decision(App* app, Str exe, Decision d) {
    app_get(app, exe)->decision = d;
    app->apps.version++;
    Config* c = &app->config;
    names_set(&c->capture.included_apps, exe, d == DECISION_INCLUDED);
    names_set(&c->capture.excluded_apps, exe, d == DECISION_EXCLUDED);
    config_save_names("capture", "included_apps", &c->capture.included_apps);
    config_save_names("capture", "excluded_apps", &c->capture.excluded_apps);
}

Str app_label(App* app, Str speaker) {
    AppInfo* a = app_find(app, speaker);
    if (a && a->name[0]) return str(a->name);
    return strip_exe(speaker);
}

AppLabels app_labels(App* app, Arena* a) {
    List<AppInfo>* list = &app->apps.list;
    AppLabels out = {PUSH_ARRAY(a, AppLabel, list->count), 0};
    for (i32 i = 0; i < list->count; i++) {
        if (!list->items[i].name[0]) continue;
        AppLabel* l = &out.items[out.count++];
        SET(l->exe, str(list->items[i].exe));
        SET(l->name, str(list->items[i].name));
    }
    return out;
}

Str label_of(AppLabels* labels, Str speaker) {
    for (i32 k = 0; labels && k < labels->count; k++)
        if (str_eq_nocase(str(labels->items[k].exe), speaker)) return str(labels->items[k].name);
    return strip_exe(speaker);
}

bool proc_path(u32 pid, char* out, i32 cap) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH * 2];
    DWORD n = COUNT(path);
    bool ok = QueryFullProcessImageNameW(h, 0, path, &n) != 0;
    CloseHandle(h);
    if (ok) {
        Mark m = mark(scratch());
        set(out, cap, narrow(scratch(), path));
        rewind(m);
    }
    return ok;
}

void app_set_game(App* app, Str exe, GameMark g) {
    app_get(app, exe)->game = g;
    app->apps.version++;
    Config* c = &app->config;
    names_set(&c->processing.games, exe, g == GAME_YES);
    names_set(&c->processing.not_games, exe, g == GAME_DISMISSED);
    config_save_names("processing", "games", &c->processing.games);
    config_save_names("processing", "not_games", &c->processing.not_games);
}

void app_remove(App* app, Str exe) {
    AppInfo* a = app_get(app, exe);
    if (a->decision != DECISION_NEW) app_set_decision(app, exe, DECISION_NEW);
    if (a->game != GAME_NO) app_set_game(app, exe, GAME_NO);
    names_set(&app->config.capture.removed_apps, exe, true);
    config_save_names("capture", "removed_apps", &app->config.capture.removed_apps);
}

void app_unremove(App* app, Str exe) {
    if (!names_has(&app->config.capture.removed_apps, exe)) return;
    names_set(&app->config.capture.removed_apps, exe, false);
    config_save_names("capture", "removed_apps", &app->config.capture.removed_apps);
}

bool app_removed(App* app, Str exe) { return names_has(&app->config.capture.removed_apps, exe); }

// ---------------------------------------------------------------------------------------------
// Folder rules

bool path_under(Str path, Str folder) {
    while (folder.len && (folder.ptr[folder.len - 1] == '\\' || folder.ptr[folder.len - 1] == '/')) folder.len--;
    if (!folder.len || path.len < folder.len) return false;
    for (i64 i = 0; i < folder.len; i++) {
        char a = path.ptr[i] == '/' ? '\\' : path.ptr[i];
        char b = folder.ptr[i] == '/' ? '\\' : folder.ptr[i];
        if (tolower((u8)a) != tolower((u8)b)) return false;
    }
    return path.len == folder.len || path.ptr[folder.len] == '\\' || path.ptr[folder.len] == '/';
}

// `folder` with \ throughout and no trailing one.
static Str folder_clean(Arena* a, Str folder) {
    folder = str_trim(folder);
    char* out = PUSH_ARRAY(a, char, folder.len + 1);
    i64 n = 0;
    for (i64 i = 0; i < folder.len; i++) out[n++] = folder.ptr[i] == '/' ? '\\' : folder.ptr[i];
    while (n && out[n - 1] == '\\') n--;
    out[n] = 0;
    return {out, n};
}

static bool same_folder(Str a, Str b) {
    while (a.len && (a.ptr[a.len - 1] == '\\' || a.ptr[a.len - 1] == '/')) a.len--;
    while (b.len && (b.ptr[b.len - 1] == '\\' || b.ptr[b.len - 1] == '/')) b.len--;
    return a.len == b.len && path_under(a, b);
}

Decision app_decision(App* app, AppInfo* a) {
    if (a->decision != DECISION_NEW || !a->path[0]) return a->decision;
    Config* c = &app->config;
    Decision d = DECISION_NEW;
    i64 deepest = -1;
    for (i32 list = 0; list < 2; list++) {
        Folders* f = list ? &c->capture.excluded_folders : &c->capture.included_folders;
        for (i32 i = 0; i < f->count; i++) {
            Str folder = str(f->items[i].s);
            // The same folder in both lists: excluded wins.
            if (path_under(str(a->path), folder) && (folder.len > deepest || (list && folder.len == deepest))) {
                deepest = folder.len;
                d = list ? DECISION_EXCLUDED : DECISION_INCLUDED;
            }
        }
    }
    return d;
}

Decision folder_decision(Config* c, Str folder) {
    for (i32 i = 0; i < c->capture.excluded_folders.count; i++)
        if (same_folder(str(c->capture.excluded_folders[i].s), folder)) return DECISION_EXCLUDED;
    for (i32 i = 0; i < c->capture.included_folders.count; i++)
        if (same_folder(str(c->capture.included_folders[i].s), folder)) return DECISION_INCLUDED;
    return DECISION_NEW;
}

void app_set_folder(App* app, Str folder, Decision d) {
    Mark m = mark(scratch());
    folder = folder_clean(scratch(), folder);
    Config* c = &app->config;
    for (i32 list = 0; list < 2; list++) {
        Folders* f = list ? &c->capture.excluded_folders : &c->capture.included_folders;
        for (i32 i = 0; i < f->count; i++)
            if (same_folder(str(f->items[i].s), folder)) list_remove(f, i--);
    }
    if (folder.len && d != DECISION_NEW) {
        PathName* f = list_push(d == DECISION_INCLUDED ? &c->capture.included_folders : &c->capture.excluded_folders);
        SET(f->s, folder);
    }
    config_save_folders("capture", "included_folders", &c->capture.included_folders);
    config_save_folders("capture", "excluded_folders", &c->capture.excluded_folders);
    LOG("everything in %s: %s", folder.ptr, d == DECISION_INCLUDED ? "included" : d == DECISION_EXCLUDED ? "excluded" : "no rule");
    rewind(m);
}

// ---------------------------------------------------------------------------------------------
// Processes and windows

Procs processes(Arena* a) {
    List<Proc> list = {};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W e = {sizeof(e)};
        for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
            Proc* p = list_push(&list);
            p->pid = e.th32ProcessID;
            p->parent = e.th32ParentProcessID;
            Mark m = mark(scratch());
            SET(p->exe, str_lower(scratch(), narrow(scratch(), e.szExeFile)));
            rewind(m);
        }
        CloseHandle(snap);
    }
    Procs out = {PUSH_ARRAY(a, Proc, list.count), list.count};
    if (list.count) memcpy(out.items, list.items, sizeof(Proc) * list.count);
    list_free(&list);
    return out;
}

Proc* proc_find(Procs* p, u32 pid) {
    for (i32 i = 0; i < p->count; i++)
        if (p->items[i].pid == pid) return &p->items[i];
    return 0;
}

u32 proc_root(Procs* p, u32 pid) {
    Proc* at = proc_find(p, pid);
    if (!at) return pid;
    for (i32 i = 0; i < 32; i++) {
        Proc* parent = proc_find(p, at->parent);
        if (!parent || parent == at || strcmp(parent->exe, at->exe) != 0) break;
        at = parent;
    }
    return at->pid;
}

struct WinsBuild {
    List<Win> list;
    HWND front;
};

static BOOL CALLBACK each_window(HWND hwnd, LPARAM lp) {
    WinsBuild* b = (WinsBuild*)lp;
    // Tool windows are overlays and helpers, not something one thinks of as an app.
    bool tool = (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    if (!IsWindowVisible(hwnd) || tool) return TRUE;
    wchar_t buf[512];
    int len = GetWindowTextW(hwnd, buf, COUNT(buf));
    if (len <= 0) return TRUE;
    Win w = {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    w.pid = pid;
    Mark m = mark(scratch());
    SET(w.title, narrow(scratch(), buf, len));
    rewind(m);
    if (hwnd == b->front) {
        list_push(&b->list);
        memmove(&b->list.items[1], &b->list.items[0], sizeof(Win) * (b->list.count - 1));
        b->list.items[0] = w;
    } else {
        list_push(&b->list, w);
    }
    return TRUE;
}

Wins windows_now(Arena* a) {
    WinsBuild b = {};
    b.front = GetForegroundWindow();
    EnumWindows(each_window, (LPARAM)&b);
    Wins out = {PUSH_ARRAY(a, Win, b.list.count), b.list.count};
    if (b.list.count) memcpy(out.items, b.list.items, sizeof(Win) * b.list.count);
    list_free(&b.list);
    return out;
}

Str title_for(Str exe, Procs* p, Wins* w) {
    for (i32 i = 0; i < w->count; i++) {
        Proc* proc = proc_find(p, w->items[i].pid);
        if (proc && str_eq_nocase(str(proc->exe), exe)) return str(w->items[i].title);
    }
    return {"", 0};
}

// ---------------------------------------------------------------------------------------------
// The cache: icons\<exe>.rgba (32×32 RGBA, as the old lifelog kept them) and paths.txt
// ("exe<TAB>path" a line). Under %LOCALAPPDATA%\lifelog, or beside the config when LIFELOG_CONFIG
// points elsewhere, so a test run keeps to its own folder.

static const wchar_t* cache_dir() {
    static wchar_t buf[MAX_PATH];
    if (buf[0]) return buf;
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"LIFELOG_CONFIG", env, COUNT(env)) == 0) {
        swprintf(buf, MAX_PATH, L"%s", local_dir());
    } else {
        swprintf(buf, MAX_PATH, L"%s", config_path());
        wchar_t* slash = wcsrchr(buf, L'\\');
        if (slash) *slash = 0;
    }
    return buf;
}

// `version` 1 is the old lifelog's file (<exe>.rgba), which cropped icons bigger than 32 px; 2
// (<exe>.2.rgba) is scaled to fit. The old one is only used when the exe can't be found any more.
static void icon_file(Str exe, i32 version, wchar_t out[MAX_PATH]) {
    char safe[81];
    i32 n = 0;
    for (i64 i = 0; i < exe.len && n < 80; i++) {
        char c = exe.ptr[i];
        safe[n++] = (isalnum((u8)c) || c == '-' || c == '_' || c == '.' || (u8)c >= 0x80) ? c : '_';
    }
    safe[n] = 0;
    Mark m = mark(scratch());
    swprintf(out, MAX_PATH, L"%s\\icons\\%s%s.rgba", cache_dir(), wide(scratch(), str(safe)), version == 1 ? L"" : L".2");
    rewind(m);
}

static void paths_file(wchar_t out[MAX_PATH]) { swprintf(out, MAX_PATH, L"%s\\paths.txt", cache_dir()); }

// Adds lines ("exe\tpath\n" each) to paths.txt.
static void paths_append(Str lines) {
    wchar_t file[MAX_PATH];
    paths_file(file);
    disk_append(file, lines);  // the file writer does it
}

// Remembers where `exe` lives, and returns true if that was new. The main thread.
// Stores where `exe` lives. True if that is new: not known, or it moved (an exe can be run from
// another folder, like a plugin's helper after an update). paths.txt keeps the latest line.
static bool learn_path(App* app, Str exe, Str path) {
    if (!exe.len || !path.len || str_find(exe, str("\t")) >= 0 || path.len >= MAX_PATH) return false;
    ExePath* p = 0;
    for (i32 i = 0; i < app->apps.paths.count && !p; i++)
        if (str_eq_nocase(str(app->apps.paths[i].exe), exe)) p = &app->apps.paths[i];
    if (p && str_eq_nocase(str(p->path), path)) return false;
    bool moved = p != 0;
    if (!p) {
        p = list_push(&app->apps.paths);
        Mark m = mark(scratch());
        SET(p->exe, str_lower(scratch(), exe));
        rewind(m);
    }
    SET(p->path, path);
    AppInfo* a = app_find(app, exe);
    if (a && (moved || !a->path[0])) {
        SET(a->path, path);
        // Its name and icon were read from the old file: read them again from the new one.
        a->icon_recheck = true;
        if (a->name_from_exe) a->name[0] = 0;
    }
    return true;
}

// The main thread: the paths read from paths.txt (job->data is the Str).
static void paths_apply(App* app, Job* job) {
    Str text = *(Str*)job->data;
    i32 n = 0;
    for (i64 at = 0; at < text.len;) {
        i64 end = at;
        while (end < text.len && text.ptr[end] != '\n') end++;
        Str line = str_trim(str_sub(text, at, end - at));
        i64 tab = str_find(line, str("\t"));
        if (tab > 0) n += learn_path(app, str_sub(line, 0, tab), str_trim(str_sub(line, tab + 1)));
        at = end + 1;
    }
    LOG("apps: %d exe paths from paths.txt", n);
}

// The scan thread, first: reads paths.txt and hands it to the main thread.
static void paths_load(App* app) {
    wchar_t file[MAX_PATH];
    paths_file(file);
    Job* job = job_new(paths_apply);
    Str* text = PUSH(&job->arena, Str);
    *text = read_file(&job->arena, file);
    if (!text->ptr) {
        job_drop(job);
        return;
    }
    job->data = text;
    job_post_wait(app, job);
}

void app_learn_path(App* app, Str exe, Str path) {
    if (!learn_path(app, exe, path)) return;
    Mark m = mark(scratch());
    paths_append(str_fmt(scratch(), "%.*s\t%.*s\n", (int)exe.len, exe.ptr, (int)path.len, path.ptr));
    rewind(m);
}

// The shell's large icon of a file as 32×32 RGBA, into `out` (4096 bytes). False if it has none.
// lifelog is DPI-aware, so "large" is 32 px only at 100 % scaling (48 at 150 %): read the icon
// at its own size and average it down, or a bigger one would be cropped to its top-left corner.
static bool extract_icon(const wchar_t* path, u8* out) {
    const i32 N = 32;
    SHFILEINFOW info = {};
    if (!SHGetFileInfoW(path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon) return false;
    ICONINFO icon = {};
    BOOL got = GetIconInfo(info.hIcon, &icon);
    DestroyIcon(info.hIcon);
    if (!got) return false;
    BITMAP bm = {};
    // A colour icon's size is its colour bitmap's; a mono one's mask holds both halves, stacked.
    if (icon.hbmColor) GetObjectW(icon.hbmColor, sizeof(bm), &bm);
    else if (icon.hbmMask) GetObjectW(icon.hbmMask, sizeof(bm), &bm), bm.bmHeight /= 2;
    i32 w = bm.bmWidth, h = bm.bmHeight;
    bool ok = icon.hbmColor && w > 0 && h > 0 && w <= 512 && h <= 512;
    Mark m = mark(scratch());
    u8* src = ok ? PUSH_ARRAY(scratch(), u8, (i64)w * h * 4) : 0;
    u8* mask = ok ? PUSH_ARRAY(scratch(), u8, (i64)w * h * 4) : 0;
    bool have_mask = false;
    if (ok) {
        HDC dc = GetDC(0);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;  // top row first
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        ok = GetDIBits(dc, icon.hbmColor, 0, (UINT)h, src, &bmi, DIB_RGB_COLORS) == h;
        have_mask = icon.hbmMask && GetDIBits(dc, icon.hbmMask, 0, (UINT)h, mask, &bmi, DIB_RGB_COLORS) == h;
        ReleaseDC(0, dc);
    }
    if (icon.hbmColor) DeleteObject(icon.hbmColor);
    if (icon.hbmMask) DeleteObject(icon.hbmMask);
    if (ok) {
        // Old icons have no alpha channel: take it from the mask (black = opaque).
        bool any_alpha = false;
        for (i32 i = 0; i < w * h; i++) any_alpha |= src[i * 4 + 3] != 0;
        if (!any_alpha && !have_mask) ok = false;
        if (ok && !any_alpha)
            for (i32 i = 0; i < w * h; i++) src[i * 4 + 3] = mask[i * 4] == 0 ? 255 : 0;
    }
    if (ok) {
        // Each output pixel averages the source box it covers, weighted by alpha so the clear
        // edges don't darken the colour. BGRA in, RGBA out.
        for (i32 y = 0; y < N; y++) {
            for (i32 x = 0; x < N; x++) {
                i32 x0 = x * w / N, x1 = (x + 1) * w / N, y0 = y * h / N, y1 = (y + 1) * h / N;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                u32 r = 0, g = 0, b = 0, a = 0, n = 0;
                for (i32 sy = y0; sy < y1; sy++) {
                    for (i32 sx = x0; sx < x1; sx++) {
                        u8* p = src + ((i64)sy * w + sx) * 4;
                        b += p[0] * p[3];
                        g += p[1] * p[3];
                        r += p[2] * p[3];
                        a += p[3];
                        n++;
                    }
                }
                u8* o = out + (y * N + x) * 4;
                o[0] = a ? (u8)(r / a) : 0;
                o[1] = a ? (u8)(g / a) : 0;
                o[2] = a ? (u8)(b / a) : 0;
                o[3] = (u8)(a / n);
            }
        }
    }
    rewind(m);
    return ok;
}

// An exe's own description ("Discord", "Mozilla Firefox") from its version info, or "".
static Str exe_description(Arena* a, const wchar_t* path) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (!size) return {"", 0};
    Mark m = mark(scratch());
    void* data = arena_push(scratch(), size);
    Str out = {"", 0};
    struct Lang {
        WORD lang, page;
    }* langs = 0;
    UINT len = 0;
    if (GetFileVersionInfoW(path, 0, size, data) &&
        VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void**)&langs, &len) && len >= sizeof(Lang)) {
        wchar_t key[64];
        swprintf(key, COUNT(key), L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].page);
        wchar_t* text = 0;
        UINT n = 0;
        if (VerQueryValueW(data, key, (void**)&text, &n) && n > 1) out = str_trim(narrow(a, text));
    }
    rewind(m);
    return out;
}

// When `path` was last written, or 0 if it isn't there.
static u64 file_time(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) return 0;
    return ((u64)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime;
}

struct IconJob {
    char exe[64];
    char path[MAX_PATH];
    // Found:
    u8* rgba;        // malloc'd; null if none
    char name[128];  // the exe's description, "" if none
    bool stale;      // the exe is newer than its cached icon
};

// Icons and missing names, found on the pool (from the cache, or from the exe and then cached).
struct IconsJob {
    App* app;  // for stopping() only
    IconJob* items;
    i32 count;
};

static void icons_run(Job* job) {
    IconsJob* j = (IconsJob*)job->data;
    for (i32 i = 0; i < j->count && !stopping(j->app); i++) {
        IconJob* ic = &j->items[i];
        Mark m = mark(scratch());
        Str exe = str(ic->exe);
        wchar_t file[MAX_PATH], old_file[MAX_PATH];
        icon_file(exe, 2, file);
        icon_file(exe, 1, old_file);
        u8* rgba = (u8*)malloc(32 * 32 * 4);
        CHECK(rgba, "memory for an icon");
        wchar_t* path = ic->path[0] ? wide(scratch(), str(ic->path)) : 0;
        // A cached icon older than the exe is from before it changed (an update, or an exe rebuilt
        // with a new icon): read again.
        bool stale = path && file_time(path) > file_time(file);
        Str cached = stale ? Str{} : read_file(scratch(), file);
        bool ok = cached.ptr && cached.len == 32 * 32 * 4;
        if (ok) memcpy(rgba, cached.ptr, 32 * 32 * 4);
        if (!ok && path && extract_icon(path, rgba)) {
            ok = true;
            wchar_t dir[MAX_PATH];
            swprintf(dir, MAX_PATH, L"%s\\icons", cache_dir());
            CreateDirectoryW(dir, 0);
            if (!write_file(file, {(const char*)rgba, 32 * 32 * 4})) WARN("apps: could not cache the icon of %s", ic->exe);
        }
        if (!ok) {
            cached = read_file(scratch(), old_file);
            ok = cached.ptr && cached.len == 32 * 32 * 4;
            if (ok) memcpy(rgba, cached.ptr, 32 * 32 * 4);
        }
        if (path) SET(ic->name, exe_description(scratch(), path));
        if (ok) ic->rgba = rgba;
        else free(rgba);
        ic->stale = stale;
        rewind(m);
    }
}

static void icons_apply(App* app, Job* job) {
    IconsJob* j = (IconsJob*)job->data;
    bool found = false;
    for (i32 i = 0; i < j->count; i++) {
        IconJob* ic = &j->items[i];
        AppInfo* a = app_find(app, str(ic->exe));
        if (a) {
            found = true;
            // A new icon, or a changed one.
            u8* rgba = ic->rgba;
            if (rgba && (!a->icon || memcmp(a->icon, rgba, 32 * 32 * 4) != 0)) {
                free(a->icon);
                a->icon = rgba;
                a->icon_version++;
                ic->rgba = 0;
            }
            // A changed exe may carry a new description; a name from the Start menu stays.
            if (ic->name[0] && (!a->name[0] || (ic->stale && a->name_from_exe))) {
                SET(a->name, str(ic->name));
                a->name_from_exe = true;
            }
        }
        free(ic->rgba);
    }
    if (found) wake_window(app);
}

static bool icon_wanted(AppInfo* a, bool only_running) {
    if ((a->icon || a->icon_tried) && !a->icon_recheck) return false;
    return !only_running || a->running || a->playing;
}

// Icons for apps that have none yet and were not tried; with `only_running`, just the ones open
// now. The main thread; they are found on the pool.
static void icons_for(App* app, bool only_running) {
    i32 n = 0;
    for (i32 i = 0; i < app->apps.list.count; i++) n += icon_wanted(&app->apps.list[i], only_running);
    if (!n) return;
    Job* job = job_new(icons_apply);
    IconsJob* j = PUSH(&job->arena, IconsJob);
    j->app = app;
    j->items = PUSH_ARRAY(&job->arena, IconJob, n);
    for (i32 i = 0; i < app->apps.list.count; i++) {
        AppInfo* a = &app->apps.list[i];
        if (!icon_wanted(a, only_running)) continue;
        a->icon_tried = true;
        a->icon_recheck = false;
        IconJob* ic = &j->items[j->count++];
        SET(ic->exe, str(a->exe));
        SET(ic->path, str(a->path));
    }
    job->data = j;
    pool_post(job, icons_run);
}

// ---------------------------------------------------------------------------------------------
// Lines heard per app, from the database: asked for when the worker wrote some, at most once a
// minute. The main thread's.

static u64 g_lines_version = (u64)-1;  // Worker.lines_version when last asked
static f64 g_lines_ts;
static bool g_lines_asked;  // a count is on the way

struct LinesJob {
    u64 version;
    Slice<SpeakerCount> counts;
};

static void lines_run(sqlite3* db, Job* job) {
    LinesJob* l = (LinesJob*)job->data;
    if (db) l->counts = db_speakers(db, &job->arena);
}

static void lines_apply(App* app, Job* job) {
    LinesJob* l = (LinesJob*)job->data;
    g_lines_asked = false;
    g_lines_version = l->version;
    for (i32 i = 0; i < l->counts.count; i++) {
        Str who = l->counts.items[i].speaker;
        // Speakers are "me", people in a call, or the exe of an app.
        if (!str_ends(str_lower(scratch(), who), str(".exe")) || is_own_exe(who)) continue;
        AppInfo* a = app_get(app, who);
        a->lines = l->counts.items[i].lines;
        a->last_ts = l->counts.items[i].last_ts;
    }
    app->apps.version++;
    wake_window(app);
}

static void lines_check(App* app) {
    f64 now = now_ts();
    u64 version = app->worker.lines_version;
    if (g_lines_asked || !app->storage.ready || version == g_lines_version || now - g_lines_ts < 60) return;
    g_lines_asked = true;
    g_lines_ts = now;
    Job* job = job_new(lines_apply);
    LinesJob* l = PUSH(&job->arena, LinesJob);
    l->version = version;
    job->data = l;
    db_post(job, lines_run);
}

void apps_seen(App* app, Procs* procs, SeenPath* fresh, i32 fresh_count) {
    // Where each exe seen for the first time this run lives.
    Str lines = {"", 0};
    for (i32 i = 0; i < fresh_count; i++)
        if (!app_path(app, str(fresh[i].exe)) && learn_path(app, str(fresh[i].exe), str(fresh[i].path)))
            lines = str_fmt(scratch(), "%s%s\t%s\n", lines.ptr, fresh[i].exe, fresh[i].path);
    paths_append(lines);

    // Which apps run, and whether a game does. A game counts even when it is excluded.
    bool changed = false;
    const char* game = "";
    for (i32 i = 0; i < app->apps.list.count; i++) {
        AppInfo* a = &app->apps.list[i];
        bool running = false;
        for (i32 j = 0; j < procs->count && !running; j++) running = strcmp(procs->items[j].exe, a->exe) == 0;
        changed |= running != a->running;
        a->running = running;
        if (running && a->game == GAME_YES && !game[0]) game = a->exe;
    }
    if (strcmp(game, app->apps.game_running) != 0) {
        if (game[0]) LOG("%s is running: transcription waits, audio keeps queueing", game);
        else LOG("no game running any more");
        SET(app->apps.game_running, str(game));
        atomic_set(&app->apps.game_on, game[0] != 0);
        worker_wake(app);
        changed = true;
    }
    if (changed) wake_window(app);
    icons_for(app, true);
    lines_check(app);
}

// ---------------------------------------------------------------------------------------------
// The scan: what is installed. Games are found through the launchers' own records and marked as
// games (unless dismissed as "not a game" once, which sticks); apps are the Start menu's shortcuts,
// so the Apps list can show programs before they ever play.

struct Found {
    char name[128];
    char exe[64];
    char path[MAX_PATH];
    char game[128];  // the game it belongs to, "" for a Start menu app
};

struct Scan {
    List<Found> found;
    Names game_names;  // lower case, so a game listed by two launchers counts once
};

// ---- Registry

static List<Str> reg_subkeys(Arena* a, HKEY root, const wchar_t* path) {
    List<Str> out = {};
    HKEY key = 0;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; i++) {
        wchar_t buf[256];
        DWORD len = COUNT(buf);
        if (RegEnumKeyExW(key, i, buf, &len, 0, 0, 0, 0) != ERROR_SUCCESS) break;
        list_push(&out, narrow(a, buf, len));
    }
    RegCloseKey(key);
    return out;
}

static Str reg_string(Arena* a, HKEY root, Str path, const wchar_t* value) {
    wchar_t buf[2048];
    DWORD bytes = sizeof(buf);
    Mark m = mark(scratch());
    LSTATUS r = RegGetValueW(root, wide(scratch(), path), value, RRF_RT_REG_SZ, 0, buf, &bytes);
    rewind(m);
    if (r != ERROR_SUCCESS) return {};
    i64 len = bytes / 2;
    while (len > 0 && buf[len - 1] == 0) len--;
    Str s = str_trim(narrow(a, buf, len));
    return s.len ? s : Str{};
}

static bool file_exists(Str path) {
    Mark m = mark(scratch());
    DWORD at = GetFileAttributesW(wide(scratch(), path));
    rewind(m);
    return at != INVALID_FILE_ATTRIBUTES;
}

static Str file_name(Str path) {
    i64 i = path.len;
    while (i > 0 && path.ptr[i - 1] != '\\' && path.ptr[i - 1] != '/') i--;
    return str_sub(path, i);
}

static Str env(Arena* a, const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return narrow(a, buf, n);
}

static Str slashes_to_back(Str s) {
    for (i64 i = 0; i < s.len; i++)
        if (s.ptr[i] == '/') ((char*)s.ptr)[i] = '\\';
    return s;
}

// ---- A folder's listing

struct Entry {
    Str name;   // as on disk
    bool dir;
    u64 size;
};

static List<Entry> list_dir(Arena* a, Str dir) {
    List<Entry> out = {};
    Mark m = mark(scratch());
    wchar_t* pattern = wide(scratch(), str_fmt(scratch(), "%.*s\\*", (int)dir.len, dir.ptr));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, 0, FIND_FIRST_EX_LARGE_FETCH);
    rewind(m);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        // Links and junctions are skipped, as the old scan did (it did not follow them).
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        Entry e = {};
        e.name = narrow(a, fd.cFileName);
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        list_push(&out, e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

static Str join(Arena* a, Str dir, Str name) { return str_fmt(a, "%.*s\\%.*s", (int)dir.len, dir.ptr, (int)name.len, name.ptr); }

static bool has_any(Str s, const char** words, i32 n) {
    for (i32 i = 0; i < n; i++)
        if (str_find(s, str(words[i])) >= 0) return true;
    return false;
}

// ---- Finding a game's exes

struct GameExe {
    Str exe;  // lower case
    Str path;
    u64 size;
};

// The exes of a game's folder that are not installers, crash reporters, redistributables and
// such: the biggest few, which include the game itself and any launcher it starts from.
static List<GameExe> game_exes(Arena* a, Str dir) {
    static const char* SKIP_DIRS[] = {
        "redist", "_commonredist", "commonredist", "directx", "vcredist", "dotnet", "support", "installer",
        "__installer", "easyanticheat", "battleye", "engine", "prereqs", "_redist",
    };
    // Helpers that ship next to games but aren't the game (launchers would mark idle clients as games).
    static const char* SKIP_WORDS[] = {
        "unins", "crash", "setup", "redist", "report", "install", "update", "helper", "prereq", "dxsetup", "dotnet",
        "cleanup", "service", "overlay", "bugsplat", "handler", "easyanticheat", "beservice", "battleye", "cefprocess",
        "webhelper", "notification", "launcher", "error", "diag", "console", "browser", "renderinfo", "tpprocess",
        "leagueclient", "editor",
    };
    struct Todo {
        Str dir;
        i32 depth;
    };
    List<Todo> stack = {};
    List<GameExe> found = {};
    list_push(&stack, {dir, 0});
    i32 seen = 0;
    while (stack.count && seen <= 20000) {
        Todo t = stack.items[--stack.count];
        List<Entry> entries = list_dir(a, t.dir);
        for (i32 i = 0; i < entries.count && ++seen <= 20000; i++) {
            Entry* e = &entries.items[i];
            Str name = str_lower(a, e->name);
            if (e->dir) {
                bool skip = false;
                for (i32 k = 0; k < COUNT(SKIP_DIRS) && !skip; k++) skip = str_eq(name, str(SKIP_DIRS[k]));
                if (t.depth < 4 && !skip) list_push(&stack, {join(a, t.dir, e->name), t.depth + 1});
            } else if (str_ends(name, str(".exe")) && !has_any(name, SKIP_WORDS, COUNT(SKIP_WORDS))) {
                list_push(&found, {name, join(a, t.dir, e->name), e->size});
            }
        }
        list_free(&entries);
    }
    list_free(&stack);
    // Biggest first, one per name, four at most.
    for (i32 i = 1; i < found.count; i++)
        for (i32 j = i; j > 0 && found.items[j].size > found.items[j - 1].size; j--) {
            GameExe t = found.items[j];
            found.items[j] = found.items[j - 1];
            found.items[j - 1] = t;
        }
    List<GameExe> out = {};
    for (i32 i = 0; i < found.count && out.count < 4; i++) {
        bool dup = false;
        for (i32 k = 0; k < out.count && !dup; k++) dup = str_eq(out.items[k].exe, found.items[i].exe);
        if (!dup) list_push(&out, found.items[i]);
    }
    list_free(&found);
    return out;
}

static void add_game(Scan* scan, Arena* a, Str name, Str dir, Str extra) {
    if (!name.len || !dir.len) return;
    List<GameExe> exes = game_exes(a, dir);
    if (extra.len && file_exists(extra)) {
        Str exe = str_lower(a, file_name(extra));
        bool dup = false;
        for (i32 k = 0; k < exes.count && !dup; k++) dup = str_eq(exes.items[k].exe, exe);
        if (exe.len && !dup) {
            list_push(&exes);
            memmove(&exes.items[1], &exes.items[0], sizeof(GameExe) * (exes.count - 1));
            exes.items[0] = {exe, extra, 0};
        }
    }
    if (exes.count && !names_has(&scan->game_names, name)) {
        names_set(&scan->game_names, name, true);
        for (i32 i = 0; i < exes.count; i++) {
            Found* f = list_push(&scan->found);
            SET(f->name, name);
            SET(f->game, name);
            SET(f->exe, exes.items[i].exe);
            SET(f->path, exes.items[i].path);
        }
    }
    list_free(&exes);
}

// ---- Steam: libraryfolders.vdf and appmanifest_*.acf are Valve KeyValues files

// The quoted strings of a KeyValues file, in order.
static List<Str> vdf_tokens(Arena* a, Str text) {
    List<Str> out = {};
    for (i64 i = 0; i < text.len; i++) {
        if (text.ptr[i] != '"') continue;
        i64 end = i + 1;
        while (end < text.len && text.ptr[end] != '"') end += text.ptr[end] == '\\' ? 2 : 1;
        char* s = PUSH_ARRAY(a, char, end - i + 1);
        i64 n = 0;
        for (i++; i < text.len && text.ptr[i] != '"'; i++) {
            if (text.ptr[i] == '\\' && i + 1 < text.len) i++;
            s[n++] = text.ptr[i];
        }
        list_push(&out, {s, n});
    }
    return out;
}

static Str vdf_value(List<Str>* tokens, const char* key) {
    for (i32 i = 0; i + 1 < tokens->count; i++)
        if (str_eq_nocase(tokens->items[i], str(key))) return tokens->items[i + 1];
    return {};
}

static void steam(Scan* scan, Arena* a) {
    Str root = reg_string(a, HKEY_CURRENT_USER, str("Software\\Valve\\Steam"), L"SteamPath");
    if (!root.len) return;
    root = slashes_to_back(root);
    List<Str> libraries = {};
    list_push(&libraries, root);
    Mark m = mark(scratch());
    Str text = read_file(a, wide(scratch(), join(scratch(), root, str("steamapps\\libraryfolders.vdf"))));
    rewind(m);
    if (text.ptr) {
        List<Str> tokens = vdf_tokens(a, text);
        for (i32 i = 0; i + 1 < tokens.count; i++) {
            if (!str_eq_nocase(tokens.items[i], str("path"))) continue;
            bool have = false;
            for (i32 k = 0; k < libraries.count && !have; k++) have = str_eq_nocase(libraries.items[k], tokens.items[i + 1]);
            if (!have) list_push(&libraries, tokens.items[i + 1]);
        }
        list_free(&tokens);
    }
    // Steam's own redistributables and tools that are not games.
    static const char* NOT_GAMES[] = {"228980", "1070560", "1391110"};
    for (i32 l = 0; l < libraries.count; l++) {
        Str apps = join(a, libraries.items[l], str("steamapps"));
        List<Entry> entries = list_dir(a, apps);
        for (i32 i = 0; i < entries.count; i++) {
            Str name = entries.items[i].name;
            if (!str_starts(name, str("appmanifest_")) || !str_ends(name, str(".acf"))) continue;
            Mark mm = mark(scratch());
            Str acf = read_file(a, wide(scratch(), join(scratch(), apps, name)));
            rewind(mm);
            if (!acf.ptr) continue;
            List<Str> tokens = vdf_tokens(a, acf);
            Str title = vdf_value(&tokens, "name"), dir = vdf_value(&tokens, "installdir"), id = vdf_value(&tokens, "appid");
            bool skip = !title.len || !dir.len || str_find(title, str("Proton")) >= 0;
            for (i32 k = 0; k < COUNT(NOT_GAMES) && !skip; k++) skip = str_eq(id, str(NOT_GAMES[k]));
            if (!skip) add_game(scan, a, title, join(a, join(a, apps, str("common")), dir), {});
            list_free(&tokens);
        }
        list_free(&entries);
    }
    list_free(&libraries);
}

// ---- Epic: one JSON manifest per game


static void epic(Scan* scan, Arena* a) {
    Str data = env(a, L"ProgramData");
    if (!data.len) data = str("C:\\ProgramData");
    Str dir = join(a, data, str("Epic\\EpicGamesLauncher\\Data\\Manifests"));
    List<Entry> entries = list_dir(a, dir);
    for (i32 i = 0; i < entries.count; i++) {
        if (!str_ends(str_lower(a, entries.items[i].name), str(".item"))) continue;
        Mark m = mark(scratch());
        Str text = read_file(a, wide(scratch(), join(scratch(), dir, entries.items[i].name)));
        rewind(m);
        Json manifest = {};
        if (!text.ptr || !json_parse(a, text, &manifest, 0, 0)) continue;
        // Only manifests whose AppCategories (if listed) include "games".
        const Json* cats = json_get(&manifest, "AppCategories");
        if (cats->type == JSON_ARRAY) {
            bool game = false;
            for (i32 k = 0; k < json_len(cats) && !game; k++) game = str_eq(json_text(json_at(cats, k)), str("games"));
            if (!game) continue;
        }
        Str name = json_text(json_get(&manifest, "DisplayName")), location = json_text(json_get(&manifest, "InstallLocation"));
        if (!name.len || !location.len) continue;
        location = slashes_to_back(str_copy(a, location));
        Str launch = str_copy(a, json_text(json_get(&manifest, "LaunchExecutable")));
        add_game(scan, a, name, location, launch.len ? join(a, location, slashes_to_back(launch)) : Str{});
    }
    list_free(&entries);
}

// ---- GOG, Ubisoft, and the launchers that only leave an uninstall entry

static void gog(Scan* scan, Arena* a) {
    const char* KEY = "SOFTWARE\\WOW6432Node\\GOG.com\\Games";
    List<Str> ids = reg_subkeys(a, HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games");
    for (i32 i = 0; i < ids.count; i++) {
        Str key = str_fmt(a, "%s\\%s", KEY, ids.items[i].ptr);
        Str name = reg_string(a, HKEY_LOCAL_MACHINE, key, L"gameName");
        Str path = reg_string(a, HKEY_LOCAL_MACHINE, key, L"path");
        if (!name.len || !path.len) continue;
        add_game(scan, a, name, path, reg_string(a, HKEY_LOCAL_MACHINE, key, L"exe"));
    }
    list_free(&ids);
}

static void ubisoft(Scan* scan, Arena* a) {
    const char* KEY = "SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs";
    List<Str> ids = reg_subkeys(a, HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs");
    for (i32 i = 0; i < ids.count; i++) {
        Str dir = reg_string(a, HKEY_LOCAL_MACHINE, str_fmt(a, "%s\\%s", KEY, ids.items[i].ptr), L"InstallDir");
        if (!dir.len) continue;
        dir = slashes_to_back(dir);
        while (dir.len && dir.ptr[dir.len - 1] == '\\') dir.len--;
        add_game(scan, a, file_name(dir), dir, {});
    }
    list_free(&ids);
}

// Games from launchers that only leave an ordinary uninstall entry: Battle.net, Riot and EA.
static void uninstall_entries(Scan* scan, Arena* a) {
    static const char* PUBLISHERS[] = {"Blizzard Entertainment", "Riot Games", "Electronic Arts", "Valve"};
    static const char* NOT_GAMES[] = {"battle.net", "riot client", "ea app", "origin", "steam", "riot vanguard"};
    struct Where {
        HKEY root;
        const char* base;
    };
    Where places[] = {
        {HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
    };
    for (i32 p = 0; p < COUNT(places); p++) {
        List<Str> ids = reg_subkeys(a, places[p].root, wide(a, str(places[p].base)));
        for (i32 i = 0; i < ids.count; i++) {
            Str key = str_fmt(a, "%s\\%s", places[p].base, ids.items[i].ptr);
            Str publisher = reg_string(a, places[p].root, key, L"Publisher");
            bool known = false;
            for (i32 k = 0; k < COUNT(PUBLISHERS) && !known; k++) known = str_starts(publisher, str(PUBLISHERS[k]));
            if (!known) continue;
            Str name = reg_string(a, places[p].root, key, L"DisplayName");
            Str dir = reg_string(a, places[p].root, key, L"InstallLocation");
            if (!name.len || !dir.len) continue;
            Str lower = str_lower(a, name);
            bool launcher = false;
            for (i32 k = 0; k < COUNT(NOT_GAMES) && !launcher; k++) launcher = str_eq(lower, str(NOT_GAMES[k]));
            if (launcher) continue;
            while (dir.len && dir.ptr[0] == '"') dir.ptr++, dir.len--;
            while (dir.len && dir.ptr[dir.len - 1] == '"') dir.len--;
            add_game(scan, a, name, str_copy(a, dir), {});
        }
        list_free(&ids);
    }
}

static void xbox(Scan* scan, Arena* a) {
    DWORD drives = GetLogicalDrives();
    for (i32 i = 2; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        Str root = str_fmt(a, "%c:\\XboxGames", 'A' + i);
        List<Entry> entries = list_dir(a, root);
        for (i32 k = 0; k < entries.count; k++) {
            Entry* e = &entries.items[k];
            if (!e->dir || str_eq(e->name, str("GameSave"))) continue;
            Str content = join(a, join(a, root, e->name), str("Content"));
            DWORD at = GetFileAttributesW(wide(a, content));
            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) add_game(scan, a, e->name, content, {});
        }
        list_free(&entries);
    }
}

// ---- Apps: the Start menu

static Str shortcut_target(Arena* a, Str link) {
    IShellLinkW* shell_link = 0;
    if (FAILED(CoCreateInstance(__uuidof(ShellLink), 0, CLSCTX_INPROC_SERVER, __uuidof(IShellLinkW), (void**)&shell_link))) return {};
    Str out = {};
    IPersistFile* file = 0;
    if (SUCCEEDED(shell_link->QueryInterface(__uuidof(IPersistFile), (void**)&file))) {
        Mark m = mark(scratch());
        if (SUCCEEDED(file->Load(wide(scratch(), link), STGM_READ))) {
            wchar_t buf[1024] = {};
            if (SUCCEEDED(shell_link->GetPath(buf, COUNT(buf), 0, 0)) && buf[0]) out = narrow(a, buf);
        }
        rewind(m);
        file->Release();
    }
    shell_link->Release();
    return out;
}

static void start_menu(Scan* scan, Arena* a) {
    static const char* SKIP[] = {"unins", "uninstall", "setup", "readme", "help", "manual", "website", "update"};
    struct Todo {
        Str dir;
        i32 depth;
    };
    List<Todo> stack = {};
    Str data = env(a, L"ProgramData"), roaming = env(a, L"APPDATA");
    if (data.len) list_push(&stack, {join(a, data, str("Microsoft\\Windows\\Start Menu\\Programs")), 0});
    if (roaming.len) list_push(&stack, {join(a, roaming, str("Microsoft\\Windows\\Start Menu\\Programs")), 0});
    Names exes = {};
    while (stack.count) {
        Todo t = stack.items[--stack.count];
        List<Entry> entries = list_dir(a, t.dir);
        for (i32 i = 0; i < entries.count; i++) {
            Entry* e = &entries.items[i];
            Str path = join(a, t.dir, e->name);
            if (e->dir) {
                if (t.depth < 3) list_push(&stack, {path, t.depth + 1});
                continue;
            }
            Str lower = str_lower(a, e->name);
            if (!str_ends(lower, str(".lnk"))) continue;
            Str name = str_sub(e->name, 0, e->name.len - 4);
            if (has_any(str_sub(lower, 0, lower.len - 4), SKIP, COUNT(SKIP))) continue;
            Str target = shortcut_target(a, path);
            Str exe = str_lower(a, file_name(target));
            if (!str_ends(exe, str(".exe")) || has_any(exe, SKIP, COUNT(SKIP)) || !file_exists(target)) continue;
            if (names_has(&exes, exe)) continue;
            names_set(&exes, exe, true);
            Found* f = list_push(&scan->found);
            SET(f->name, name);
            SET(f->exe, exe);
            SET(f->path, target);
        }
        list_free(&entries);
    }
    list_free(&exes);
    list_free(&stack);
}

// Stores what the scan found: names, paths, and games marked. The main thread.
static Str scan_store(App* app, Scan* scan, Arena* a, i32* new_games) {
    Config* c = &app->config;
    Str lines = {"", 0};
    *new_games = 0;
    for (i32 i = 0; i < scan->found.count; i++) {
        Found* f = &scan->found.items[i];
        if (is_own_exe(str(f->exe))) continue;
        if (learn_path(app, str(f->exe), str(f->path))) lines = str_fmt(a, "%s%s\t%s\n", lines.ptr, f->exe, f->path);
        AppInfo* info = app_get(app, str(f->exe));
        info->installed = true;
        if (!info->name[0]) SET(info->name, str(f->name));
        if (!info->path[0]) SET(info->path, str(f->path));
    }
    // A game whose exes include one dismissed as "not a game" stays dismissed as a whole.
    for (i32 i = 0; i < scan->found.count; i++) {
        Found* f = &scan->found.items[i];
        if (!f->game[0]) continue;
        bool dismissed = false;
        for (i32 k = 0; k < scan->found.count && !dismissed; k++)
            dismissed = strcmp(scan->found.items[k].game, f->game) == 0 && names_has(&c->processing.not_games, str(scan->found.items[k].exe));
        AppInfo* info = app_find(app, str(f->exe));
        if (dismissed || !info || info->game != GAME_NO) continue;
        info->game = GAME_YES;
        names_set(&c->processing.games, str(f->exe), true);
        (*new_games)++;
    }
    if (*new_games) config_save_names("processing", "games", &c->processing.games);
    app->apps.version++;
    return lines;
}

struct ScanJob {
    Scan scan;
    i32 games, apps;
    f64 seconds;
};

// The main thread: what the scan found.
static void scan_apply(App* app, Job* job) {
    ScanJob* j = (ScanJob*)job->data;
    i32 new_games = 0;
    Str lines = scan_store(app, &j->scan, &job->arena, &new_games);
    paths_append(lines);
    LOG("apps: found %d installed games and %d apps in %.1f s", j->games, j->apps, j->seconds);
    if (new_games) LOG("apps: marked %d installed games' exes as games", new_games);
    wake_window(app);
    list_free(&j->scan.found);
    list_free(&j->scan.game_names);
    icons_for(app, false);
}

// Every 6 hours: what is installed. First, the paths remembered from earlier runs.
static DWORD WINAPI apps_scan_main(void* p) {
    App* app = (App*)p;
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);  // the shell's shortcuts want it
    paths_load(app);
    do {
        f64 started = now_ts();
        Job* job = job_new(scan_apply);
        ScanJob* j = PUSH(&job->arena, ScanJob);
        Arena* arena = &job->arena;
        Scan* scan = &j->scan;
        steam(scan, arena);
        epic(scan, arena);
        gog(scan, arena);
        ubisoft(scan, arena);
        uninstall_entries(scan, arena);
        xbox(scan, arena);
        j->games = scan->game_names.count;
        start_menu(scan, arena);
        j->apps = scan->found.count;
        for (i32 i = 0; i < scan->found.count; i++) j->apps -= scan->found.items[i].game[0] != 0;
        j->seconds = now_ts() - started;
        job->data = j;
        job_post_wait(app, job);
    } while (sleep_unless_stopping(app, 6 * 3600 * 1000));
    CoUninitialize();
    return 0;
}

void apps_start(App* app) {
    start_thread(app, "apps-scan", apps_scan_main);
}
