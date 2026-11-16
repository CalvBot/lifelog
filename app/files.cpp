// files.cpp: "Paste here" and opening what was pasted (see clip.h). Copies live in
// <data_dir>\files\YYYY-MM-DD\<HHMMSS>-<name>; the timeline shows them between the lines.
// Pasted text that is all file or folder paths or web addresses becomes links instead, with no
// copy (good for very large files).

#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

static const char* kind_of(Str name) {
    i64 dot = -1;
    for (i64 i = 0; i < name.len; i++) {
        if (name.ptr[i] == '.') dot = i;
        if (name.ptr[i] == '\\' || name.ptr[i] == '/') dot = -1;
    }
    if (dot < 0) return "file";
    Mark m = mark(scratch());
    Str ext = str_lower(scratch(), str_sub(name, dot + 1));
    static const char* image[] = {"png", "jpg", "jpeg", "gif", "webp", "bmp", "avif", "svg"};
    static const char* video[] = {"mp4", "webm", "mov", "m4v"};
    static const char* audio[] = {"mp3", "wav", "ogg", "m4a", "flac", "opus"};
    static const char* text[] = {"txt", "md"};
    const char* kind = "file";
    for (i32 i = 0; i < COUNT(image); i++) if (str_eq(ext, str(image[i]))) kind = "image";
    for (i32 i = 0; i < COUNT(video); i++) if (str_eq(ext, str(video[i]))) kind = "video";
    for (i32 i = 0; i < COUNT(audio); i++) if (str_eq(ext, str(audio[i]))) kind = "audio";
    for (i32 i = 0; i < COUNT(text); i++) if (str_eq(ext, str(text[i]))) kind = "text";
    rewind(m);
    return kind;
}

static bool starts_nocase(Str s, const char* prefix) {
    Str p = str(prefix);
    return s.len >= p.len && str_eq_nocase(str_sub(s, 0, p.len), p);
}

// A pasted line that is a web address: "https://…", or "www.…" (made https). Empty if not.
static Str as_url(Arena* a, Str line) {
    for (i64 i = 0; i < line.len; i++)
        if ((u8)line.ptr[i] <= ' ' || line.ptr[i] == '\\') return {};
    if (starts_nocase(line, "http://") || starts_nocase(line, "https://")) return str_copy(a, line);
    if (starts_nocase(line, "www.")) return str_fmt(a, "https://%.*s", (int)line.len, line.ptr);
    return {};
}

// A pasted line that is an existing file or folder: "C:\…", "\\server\…", quoted or not, or a
// file:/// address. Null if not.
static wchar_t* as_path(Arena* a, Str line) {
    line = str_trim(line);
    while (line.len && line.ptr[0] == '"') line = str_sub(line, 1);
    while (line.len && line.ptr[line.len - 1] == '"') line.len--;
    line = str_trim(line);
    bool url = str_starts(line, str("file:///"));
    if (url) line = str_sub(line, 8);
    wchar_t* w = wide(a, line);
    if (url) {
        wchar_t* o = w;
        for (wchar_t* c = w; *c; c++) {
            if (c[0] == L'%' && c[1] == L'2' && c[2] == L'0') {
                *o++ = L' ';
                c += 2;
            } else {
                *o++ = *c == L'/' ? L'\\' : *c;
            }
        }
        *o = 0;
    }
    bool drive = iswalpha(w[0]) && w[1] == L':' && (w[2] == L'\\' || w[2] == L'/');
    bool unc = w[0] == L'\\' && w[1] == L'\\';
    if (!drive && !unc) return 0;
    if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) return 0;
    return w;
}

// The last part of a path ("C:\a\b.txt" → "b.txt"); the whole path if it has none ("C:\").
static Str path_name(Str path) {
    while (path.len > 1 && (path.ptr[path.len - 1] == '\\' || path.ptr[path.len - 1] == '/')) path.len--;
    i64 from = 0;
    for (i64 i = 0; i < path.len; i++)
        if (path.ptr[i] == '\\' || path.ptr[i] == '/') from = i + 1;
    Str name = str_sub(path, from);
    if (name.len == 0 || name.ptr[name.len - 1] == ':') return path;
    return name;
}

static i64 file_size(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) return 0;
    return ((i64)info.nFileSizeHigh << 32) | info.nFileSizeLow;
}

static bool files_insert(sqlite3* db, f64 ts, Str name, Str path, const char* kind, i64 size, bool link) {
    sqlite3_stmt* st = prepare(db, "INSERT INTO files (ts, name, path, kind, size, link) VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    if (!st) return false;
    sqlite3_bind_double(st, 1, ts);
    sqlite3_bind_text(st, 2, name.ptr, (int)name.len, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, path.ptr, (int)path.len, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, kind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, size);
    sqlite3_bind_int(st, 6, link ? 1 : 0);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) ERR("saving a pasted file: %s", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Bitmaps

static u32 le32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

// A clipboard bitmap (BITMAPINFOHEADER + pixels, 24 or 32 bit) written as a PNG, through WIC.
static bool png_from_dib(const u8* dib, i64 len, const wchar_t* path, char* error, i32 cap) {
    if (len < 40) {
        snprintf(error, cap, "bad bitmap");
        return false;
    }
    i64 header = le32(dib);
    i32 width = (i32)le32(dib + 4), height = (i32)le32(dib + 8);
    u16 bits = (u16)(dib[14] | dib[15] << 8);
    u32 compression = le32(dib + 16);
    if (bits != 24 && bits != 32) {
        snprintf(error, cap, "unsupported bitmap (%d bit)", bits);
        return false;
    }
    i64 masks = compression == 3 && header == 40 ? 12 : 0;
    i64 w = width < 0 ? -(i64)width : width, h = height < 0 ? -(i64)height : height;
    i64 stride = (w * bits / 8 + 3) & ~3LL;
    if (w == 0 || h == 0 || header + masks > len || len - header - masks < stride * h) {
        snprintf(error, cap, "the bitmap is cut short");
        return false;
    }
    const u8* pixels = dib + header + masks;
    u8* bgra = (u8*)malloc((size_t)(w * h * 4));
    CHECK(bgra, "memory for a %lldx%lld image", w, h);
    for (i64 y = 0; y < h; y++) {
        // Positive height: bottom row first.
        const u8* row = pixels + (height > 0 ? h - 1 - y : y) * stride;
        u8* o = bgra + y * w * 4;
        for (i64 x = 0; x < w; x++) {
            const u8* p = row + x * bits / 8;
            o[x * 4 + 0] = p[0];
            o[x * 4 + 1] = p[1];
            o[x * 4 + 2] = p[2];
            o[x * 4 + 3] = 255;
        }
    }
    IWICImagingFactory* wic = 0;
    IWICBitmap* bitmap = 0;
    IWICStream* stream = 0;
    IWICBitmapEncoder* encoder = 0;
    IWICBitmapFrameEncode* frame = 0;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGRA, (UINT)(w * 4), (UINT)(w * h * 4), bgra, &bitmap);
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, 0, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, 0);
    if (SUCCEEDED(hr)) hr = frame->Initialize(0);
    if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)w, (UINT)h);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap, 0);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (bitmap) bitmap->Release();
    if (wic) wic->Release();
    free(bgra);
    if (FAILED(hr)) snprintf(error, cap, "could not save the image (0x%08lx)", (unsigned long)hr);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------------------------
// Paste here

enum PasteKind : u8 {
    PASTE_FILES,  // data: the paths, each zero-terminated (wide)
    PASTE_PNG,    // data: a PNG
    PASTE_DIB,    // data: a BITMAPINFOHEADER and pixels
    PASTE_TEXT,   // data: the text, zero-terminated (wide)
};

// What was on the clipboard, handed to the thread that saves it. Freed by that thread.
struct PasteJob {
    App* app;     // for settings() and db_open only
    PasteKind kind;
    f64 ts;       // the moment it was pasted
    u8* data;     // malloc'd
    i64 size;
    i32 count;    // paths, for PASTE_FILES
    i32 saved;    // the results
    char error[256];
};

// A copy of a clipboard handle's bytes (with two zero bytes after them, so text always ends).
static u8* clip_bytes(HANDLE h, i64* size) {
    if (!h) return 0;
    void* p = GlobalLock(h);
    if (!p) return 0;
    *size = (i64)GlobalSize(h);
    u8* out = (u8*)calloc((size_t)*size + 2, 1);
    CHECK(out, "memory for %lld bytes from the clipboard", *size);
    memcpy(out, p, (size_t)*size);
    GlobalUnlock(h);
    return out;
}

static bool paste_read_clipboard(PasteJob* job, char* error, i32 cap) {
    HWND owner;
    if (!clipboard_open(&owner)) {
        snprintf(error, cap, "the clipboard is busy");
        return false;
    }
    UINT png = RegisterClipboardFormatW(L"PNG");
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP drop = (HDROP)GetClipboardData(CF_HDROP);
        UINT n = drop ? DragQueryFileW(drop, 0xFFFFFFFF, 0, 0) : 0;
        List<wchar_t> paths = {};
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(drop, i, 0, 0);
            i32 at = paths.count;
            for (UINT j = 0; j <= len; j++) list_push(&paths, L'\0');
            DragQueryFileW(drop, i, paths.items + at, len + 1);
        }
        job->kind = PASTE_FILES;
        job->data = (u8*)paths.items;
        job->size = paths.count * (i64)sizeof(wchar_t);
        job->count = (i32)n;
    } else if (png && IsClipboardFormatAvailable(png)) {
        // Screenshots and browsers usually offer a ready PNG.
        job->kind = PASTE_PNG;
        job->data = clip_bytes(GetClipboardData(png), &job->size);
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        job->kind = PASTE_DIB;
        job->data = clip_bytes(GetClipboardData(CF_DIB), &job->size);
    } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        job->kind = PASTE_TEXT;
        job->data = clip_bytes(GetClipboardData(CF_UNICODETEXT), &job->size);
        bool blank = true;
        for (wchar_t* c = (wchar_t*)job->data; c && *c && blank; c++) blank = *c <= L' ';
        if (blank) {
            free(job->data);
            job->data = 0;
        }
    }
    clipboard_close(owner);
    if (!job->data || (job->kind == PASTE_FILES && job->count == 0)) {
        free(job->data);
        job->data = 0;
        snprintf(error, cap, "nothing to paste: copy a file, an image or some text first");
        return false;
    }
    return true;
}

// Pasted text that is only paths and web addresses (1 to 50 lines): saved as links. -1 if the
// text is something else; else how many were saved.
static i32 paste_links(sqlite3* db, f64 ts, Str text) {
    Mark m = mark(scratch());
    struct Link {
        Str target;  // the URL, or the path as UTF-8
        wchar_t* path;  // null for a URL
    };
    Link links[50];
    i32 count = 0;
    bool all = true;
    for (i64 at = 0; at <= text.len && all;) {
        i64 end = at;
        while (end < text.len && text.ptr[end] != '\n') end++;
        Str line = str_trim(str_sub(text, at, end - at));
        at = end + 1;
        if (!line.len) continue;
        if (count == COUNT(links)) {
            all = false;
            break;
        }
        Link l = {};
        l.target = as_url(scratch(), line);
        if (!l.target.len) {
            l.path = as_path(scratch(), line);
            if (l.path) l.target = narrow(scratch(), l.path);
        }
        if (!l.target.len) all = false;
        else links[count++] = l;
    }
    i32 saved = -1;
    if (all && count) {
        saved = 0;
        for (i32 i = 0; i < count; i++) {
            Link* l = &links[i];
            bool ok;
            if (!l->path) {
                ok = files_insert(db, ts, l->target, l->target, "url", 0, true);
            } else {
                Str name = path_name(l->target);
                DWORD attrs = GetFileAttributesW(l->path);
                bool folder = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
                ok = files_insert(db, ts, name, l->target, folder ? "folder" : kind_of(name), folder ? 0 : file_size(l->path), true);
            }
            if (ok) {
                LOG("linked %s in the timeline", l->target.ptr);
                saved++;
            }
        }
    }
    rewind(m);
    return saved;
}

// Saves what the job holds. Returns how many were saved; `error` is set if something failed.
static i32 paste_save(PasteJob* job, sqlite3* db, char* error, i32 cap) {
    App* app = job->app;
    Mark m = mark(scratch());
    LocalTime t = local_time(job->ts);
    Str day = str_fmt(scratch(), "%04d-%02d-%02d", t.year, t.month, t.day);
    char stamp[8];
    snprintf(stamp, sizeof(stamp), "%02d%02d%02d", t.hour, t.minute, t.second);

    if (job->kind == PASTE_TEXT) {
        Str text = narrow(scratch(), (wchar_t*)job->data);
        i32 n = paste_links(db, job->ts, text);
        if (n >= 0) {
            rewind(m);
            return n;
        }
    }

    wchar_t dir[MAX_PATH];
    files_dir(settings(app), dir);
    swprintf(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"\\%ls", wide(scratch(), day));
    i32 made = SHCreateDirectoryExW(0, dir, 0);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        snprintf(error, cap, "could not make the folder %s", narrow(scratch(), dir).ptr);
        rewind(m);
        return 0;
    }

    i32 saved = 0;
    const wchar_t* src = (const wchar_t*)job->data;
    i32 items = job->kind == PASTE_FILES ? job->count : 1;
    for (i32 i = 0; i < items && !error[0]; i++) {
        Str name = {};
        if (job->kind == PASTE_FILES) {
            const wchar_t* from = src;
            src += wcslen(src) + 1;
            DWORD attrs = GetFileAttributesW(from);
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                WARN("skipped folder %s", narrow(scratch(), from).ptr);
                continue;
            }
            name = str_copy(scratch(), path_name(narrow(scratch(), from)));
            if (!name.len) name = str("file");
            // Files keep their own name, made safe for a file name.
            char* safe = (char*)str_copy(scratch(), name).ptr;
            for (char* c = safe; *c; c++)
                if (strchr("\\/:*?\"<>|", *c) || (u8)*c < 32) *c = '_';
            Str file = str_fmt(scratch(), "%s-%s", stamp, safe);
            wchar_t to[MAX_PATH];
            swprintf(to, MAX_PATH, L"%ls\\%ls", dir, wide(scratch(), file));
            if (!CopyFileW(from, to, FALSE)) {
                snprintf(error, cap, "copying %s (error %lu)", narrow(scratch(), from).ptr, GetLastError());
                break;
            }
            Str rel = str_fmt(scratch(), "%s/%s", day.ptr, file.ptr);
            i64 size = file_size(to);
            if (files_insert(db, job->ts, name, rel, kind_of(name), size, false)) {
                LOG("pasted %s into the timeline (%lld bytes)", name.ptr, size);
                saved++;
            } else {
                snprintf(error, cap, "could not save it in the database");
            }
            continue;
        }
        name = str(job->kind == PASTE_TEXT ? "note.txt" : "image.png");
        Str file = str_fmt(scratch(), "%s-%s", stamp, name.ptr);
        wchar_t to[MAX_PATH];
        swprintf(to, MAX_PATH, L"%ls\\%ls", dir, wide(scratch(), file));
        bool wrote = false;
        if (job->kind == PASTE_PNG) wrote = write_file(to, {(const char*)job->data, job->size});
        else if (job->kind == PASTE_DIB) wrote = png_from_dib(job->data, job->size, to, error, cap);
        else wrote = write_file(to, narrow(scratch(), (wchar_t*)job->data));
        if (!wrote) {
            if (!error[0]) snprintf(error, cap, "could not write %s", narrow(scratch(), to).ptr);
            break;
        }
        Str rel = str_fmt(scratch(), "%s/%s", day.ptr, file.ptr);
        i64 size = file_size(to);
        if (files_insert(db, job->ts, name, rel, kind_of(name), size, false)) {
            LOG("pasted %s into the timeline (%lld bytes)", name.ptr, size);
            saved++;
        } else {
            snprintf(error, cap, "could not save it in the database");
        }
    }
    if (!saved && !error[0]) snprintf(error, cap, "only folders were on the clipboard");
    rewind(m);
    return saved;
}

static void paste_run(Job* j) {
    PasteJob* job = (PasteJob*)j->data;
    sqlite3* db = db_open(job->app);
    if (!db) snprintf(job->error, sizeof(job->error), "could not open the database");
    else job->saved = paste_save(job, db, job->error, sizeof(job->error));
    sqlite3_close(db);
    if (job->error[0]) WARN("paste: %s", job->error);
    free(job->data);
    job->data = 0;
}

static void paste_apply(App* app, Job* j) {
    PasteJob* job = (PasteJob*)j->data;
    if (job->saved > 0) app->storage.files_version++;
    if (job->error[0]) ui_say(app, "Paste failed: %s", job->error);
    else ui_say(app, "Saved %d item%s", job->saved, job->saved == 1 ? "" : "s");
}

i32 files_paste(App* app, char* error, i32 cap) {
    Job* j = job_new(paste_apply);
    PasteJob* job = PUSH(&j->arena, PasteJob);
    job->app = app;
    job->ts = now_ts();
    if (!paste_read_clipboard(job, error, cap)) {
        job_free(j);
        return -1;
    }
    i32 n = job->kind == PASTE_FILES ? job->count : 1;
    j->data = job;
    pool_post(j, paste_run);
    return n;
}

// ---------------------------------------------------------------------------------------------
// Opening

bool file_full_path(const Config* cfg, FileItem* f, wchar_t out[MAX_PATH]) {
    out[0] = 0;
    if (str_eq(f->kind, str("url")) || !f->path.len) return false;
    Mark m = mark(scratch());
    if (f->link) {
        swprintf(out, MAX_PATH, L"%ls", wide(scratch(), f->path));
    } else {
        // Never follow a path from the database out of the files folder.
        if (str_find(f->path, str("..")) >= 0 || str_find(f->path, str(":")) >= 0) {
            rewind(m);
            return false;
        }
        wchar_t dir[MAX_PATH];
        files_dir(cfg, dir);
        swprintf(out, MAX_PATH, L"%ls\\%ls", dir, wide(scratch(), f->path));
    }
    for (wchar_t* c = out; *c; c++)
        if (*c == L'/' && !f->link) *c = L'\\';
    rewind(m);
    return true;
}

struct OpenJob {
    const Config* cfg;
    i64 id;
    bool in_folder;
    char error[256];
};

static void file_open_now(const Config* cfg, i64 id, bool in_folder, char* error, i32 cap) {
    sqlite3* db = db_open_read(cfg);
    if (!db) {
        snprintf(error, cap, "could not open the database");
        return;
    }
    Mark m = mark(scratch());
    FileItem f = db_file(db, scratch(), id);
    wchar_t path[MAX_PATH];
    if (!f.id) {
        snprintf(error, cap, "no such file");
    } else if (str_eq(f.kind, str("url"))) {
        if (!as_url(scratch(), f.path).len) snprintf(error, cap, "not a web address");
        else if ((INT_PTR)ShellExecuteW(0, L"open", wide(scratch(), f.path), 0, 0, SW_SHOWNORMAL) <= 32)
            snprintf(error, cap, "could not open %s", f.path.ptr);
    } else if (!file_full_path(cfg, &f, path)) {
        snprintf(error, cap, "bad path");
    } else if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        snprintf(error, cap, "the file is gone from %s", narrow(scratch(), path).ptr);
    } else if (in_folder) {
        wchar_t* args = wide(scratch(), str_fmt(scratch(), "/select,\"%s\"", narrow(scratch(), path).ptr));
        if ((INT_PTR)ShellExecuteW(0, L"open", L"explorer.exe", args, 0, SW_SHOWNORMAL) <= 32)
            snprintf(error, cap, "could not show %s", narrow(scratch(), path).ptr);
    } else {
        // Whatever is registered: the file's app, or the default file manager for a folder.
        if ((INT_PTR)ShellExecuteW(0, L"open", path, 0, 0, SW_SHOWNORMAL) <= 32)
            snprintf(error, cap, "could not open %s", narrow(scratch(), path).ptr);
    }
    rewind(m);
    sqlite3_close(db);
}

static void open_run(Job* job) {
    OpenJob* o = (OpenJob*)job->data;
    file_open_now(o->cfg, o->id, o->in_folder, o->error, sizeof(o->error));
    if (o->error[0]) WARN("opening a pasted file: %s", o->error);
}

static void open_apply(App* app, Job* job) {
    OpenJob* o = (OpenJob*)job->data;
    if (o->error[0]) ui_say(app, "%s", o->error);
}

void file_open(App* app, i64 id, bool in_folder) {
    Job* job = job_new(open_apply);
    OpenJob* o = PUSH(&job->arena, OpenJob);
    o->cfg = settings(app);
    o->id = id;
    o->in_folder = in_folder;
    job->data = o;
    pool_post(job, open_run);
}
