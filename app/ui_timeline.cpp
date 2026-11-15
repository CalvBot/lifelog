// ui_timeline.cpp: the Timeline tab (port of ui/src/browser). One page per day, oldest at the
// top; today starts at the bottom and grows as lines arrive. Search, the Show filter by app,
// picking a stretch to copy, playback, and the files pasted in.
//
// A day's lines are read in one query when it opens, and again when the worker writes lines
// (Worker.lines_version) or a file is pasted (Storage.files_version). The rows are laid out by
// hand and only those near the screen are drawn, so a day of 20k lines stays fast; each row's
// height is measured once per text width. Copying, image decoding and reading text previews run
// on threads (g_tl_jobs), never under the lock.

#include <wincodec.h>

static const f32 TL_APP_W = 120;          // the app column in the list: icon and name
static const f32 TL_COLUMN = 1180;       // the widest the column gets; wider windows centre it
static const f64 TL_GAP = 300;           // a gap this long between two things heard gets its own row
static const i32 TL_SEARCH_LIMIT = 1000; // the most matches a search fetches (newest first)
static const i32 TL_DAY_LIMIT = 20000;
static const f32 TL_SMALL = 12;          // small text, before the DPI scale
static const ImU32 TL_FOUND = IM_COL32(204, 161, 17, 150);  // search matches, marked like Ctrl+F

enum TlRowKind : u8 {
    TL_ROW_GAP,
    TL_ROW_LINE,
    TL_ROW_FILE,
};

struct TlRow {
    TlRowKind kind;
    i32 index;  // into lines or files
    i64 key;    // the line or file id, or a gap's start in ms: keeps a height across reloads
    f64 ts;     // where it sits in the day
    f64 to;     // a gap's end
    f32 h;      // its height; an estimate until measured
    f32 width;  // the text width `h` was measured at (0: not measured)
};

// An app heard on the page, for the Show chips.
struct TlChip {
    Str speaker;
    i32 lines;
    bool visible;  // speaker_visible when the page was last filtered
};

// A line that just arrived; it writes itself out word by word.
struct TlFresh {
    i64 id;
    f64 at;
};

struct TimelineState {
    Arena arena;           // the shown day's lines, files and chips
    char day[11];          // the day on screen
    char loaded_day[11];   // the day `lines` are of
    u64 lines_version;     // App.worker.lines_version when they were loaded
    u64 files_version;
    bool loaded;
    Slice<Line> lines;
    Slice<FileItem> files;
    i32* line_chip;        // per line: its speaker in `chips`
    Slice<TlChip> chips;   // most lines first
    i64 max_id;            // the newest line loaded: anything above it arrived since

    List<TlRow> rows;
    bool rows_dirty;
    u64 filter_sig;        // the Show filter the rows were built with
    List<TlFresh> fresh;

    bool new_page;         // a day was just opened: fresh scroll (today at the bottom)
    bool to_bottom;        // "Now": scroll to the end of today
    i64 scroll_to;         // a line to bring to the middle of the list
    i64 hover_play;        // the line whose play button is under the mouse
    bool picked;           // a stretch is picked, from the line starting at pick_a to pick_b
    f64 pick_a, pick_b;
    ImVec2 list_min, list_max;

    char search[256];
    f64 search_due;        // when to run the search typed (0: nothing waiting)
    Arena search_arena;
    Slice<Line> results;   // every match the filter lets through, newest first
    i32 find_at;           // the match on screen, 1-based (0: none yet)
    bool find_pending;     // Enter was pressed before the matches arrived
    bool focus_search;     // Ctrl+F
    bool search_refocus;   // Enter keeps the box focused
    bool search_cursor_end;

    bool calendar_open;
    i32 cal_year, cal_month;
    Arena days_arena;
    Slice<DayCount> days;
    ImVec2 cal_button_min, cal_button_max;
    ImVec2 cal_min, cal_max;

    bool apps_open;
    Arena apps_arena;
    Slice<SpeakerCount> apps;
    char apps_filter[64];
};

// ---------------------------------------------------------------------------------------------
// Jobs on threads: copying (slow: it mixes audio), the clipboard, and file previews.

enum TlJobKind : u8 {
    TL_JOB_CLIP,     // clip_copy a stretch
    TL_JOB_TEXT,     // put `text` on the clipboard
    TL_JOB_PREVIEW,  // decode an image or read a text file for its row
};

struct TlJob {
    TlJobKind kind;
    ClipKind clip;
    f64 from, to;
    Names exclude;     // freed by the thread
    char* text;        // malloc'd, freed by the thread
    char said[64];     // the toast when the text is copied
    i64 file_id;
    bool image;
    wchar_t path[MAX_PATH];
};
static List<TlJob> g_tl_jobs;  // waiting for their thread; under App.lock

// A pasted file's preview, kept while the window is open. Under App.lock.
struct TlPreview {
    i64 id;
    bool done;
    i32 w, h;
    u8* rgba;   // decoded, until it becomes a texture
    ID3D11ShaderResourceView* view;
    char* text; // a text file's start (malloc'd)
};
static List<TlPreview> g_tl_previews;

// An image as RGBA pixels, at most 960 wide, through WIC. The caller frees *rgba.
static bool tl_decode_image(const wchar_t* path, u8** rgba, i32* w, i32* h) {
    IWICImagingFactory* wic = 0;
    IWICBitmapDecoder* decoder = 0;
    IWICBitmapFrameDecode* frame = 0;
    IWICBitmapScaler* scaler = 0;
    IWICFormatConverter* converter = 0;
    UINT fw = 0, fh = 0;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(path, 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&fw, &fh);
    if (SUCCEEDED(hr) && (fw == 0 || fh == 0)) hr = E_FAIL;
    IWICBitmapSource* source = frame;
    if (SUCCEEDED(hr) && fw > 960) {
        UINT sh = (UINT)((u64)fh * 960 / fw);
        if (sh == 0) sh = 1;
        hr = wic->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(frame, 960, sh, WICBitmapInterpolationModeFant);
        if (SUCCEEDED(hr)) {
            source = scaler;
            fw = 960;
            fh = sh;
        }
    }
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, 0, 0, WICBitmapPaletteTypeCustom);
    u8* pixels = 0;
    if (SUCCEEDED(hr)) {
        pixels = (u8*)malloc((size_t)fw * fh * 4);
        CHECK(pixels, "memory for a %ux%u preview", fw, fh);
        hr = converter->CopyPixels(0, fw * 4, fw * fh * 4, pixels);
    }
    if (converter) converter->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (wic) wic->Release();
    if (FAILED(hr)) {
        free(pixels);
        return false;
    }
    *rgba = pixels;
    *w = (i32)fw;
    *h = (i32)fh;
    return true;
}

// The first `chars` characters of a UTF-8 text (cut on a character).
static i64 utf8_offset(Str s, i64 chars) {
    i64 at = 0;
    for (i64 n = 0; at < s.len && n < chars; n++) {
        at++;
        while (at < s.len && ((u8)s.ptr[at] & 0xC0) == 0x80) at++;
    }
    return at;
}

static i64 utf8_count(Str s) {
    i64 n = 0;
    for (i64 i = 0; i < s.len; i++) n += ((u8)s.ptr[i] & 0xC0) != 0x80;
    return n;
}

static void tl_job_preview(App* app, TlJob* job) {
    u8* rgba = 0;
    i32 w = 0, h = 0;
    char* text = 0;
    if (job->image) {
        CoInitializeEx(0, COINIT_MULTITHREADED);
        tl_decode_image(job->path, &rgba, &w, &h);
        CoUninitialize();
    } else {
        Arena a = {};
        Str t = read_file(&a, job->path);
        if (t.ptr) {
            i64 n = utf8_offset(t, 4000);
            text = (char*)malloc((size_t)n + 1);
            CHECK(text, "memory for a text preview");
            memcpy(text, t.ptr, (size_t)n);
            text[n] = 0;
        }
        arena_release(&a);
    }
    app_lock(app);
    TlPreview* p = 0;
    for (i32 i = 0; i < g_tl_previews.count; i++)
        if (g_tl_previews[i].id == job->file_id) p = &g_tl_previews[i];
    if (p) {
        p->done = true;
        p->rgba = rgba;
        p->w = w;
        p->h = h;
        p->text = text;
    } else {
        // The window closed meanwhile.
        free(rgba);
        free(text);
    }
    app_unlock(app);
    wake_window(app);
}

static DWORD WINAPI tl_job_thread(void* p) {
    App* app = (App*)p;
    app_lock(app);
    CHECK(g_tl_jobs.count > 0, "a timeline job waiting for the thread started for it");
    TlJob job = g_tl_jobs.items[0];
    list_remove(&g_tl_jobs, 0);
    app_unlock(app);

    if (job.kind == TL_JOB_CLIP) {
        ClipResult r = clip_copy(app, job.from, job.to, &job.exclude, job.clip);
        app_lock(app);
        if (!r.ok) ui_say(app, "%s", r.error);
        else if (job.clip == CLIP_TEXT) ui_say(app, "Copied %d lines", r.lines);
        else if (job.clip == CLIP_FILE) ui_say(app, "Copied %d lines as a file", r.lines);
        else if (r.seconds < 60) ui_say(app, "Copied %.0f s of audio", ceil(r.seconds));
        else ui_say(app, "Copied %s of audio", duration_text(r.seconds));
        app_unlock(app);
    } else if (job.kind == TL_JOB_TEXT) {
        bool ok = clipboard_set_text(str(job.text));
        app_lock(app);
        ui_say(app, "%s", ok ? job.said : "The clipboard is busy");
        app_unlock(app);
    } else {
        tl_job_preview(app, &job);
    }
    list_free(&job.exclude);
    free(job.text);
    return 0;
}

static void tl_start_job(App* app, TlJob* job) {
    list_push(&g_tl_jobs, *job);
    start_thread(app, "timeline job", tl_job_thread);
}

static void tl_copy_range(App* app, f64 from, f64 to, Names exclude, ClipKind kind) {
    TlJob job = {};
    job.kind = TL_JOB_CLIP;
    job.clip = kind;
    job.from = from - 0.05;
    job.to = to + 0.05;
    job.exclude = exclude;
    tl_start_job(app, &job);
}

static void tl_copy_text(App* app, Str text, const char* said) {
    TlJob job = {};
    job.kind = TL_JOB_TEXT;
    job.text = (char*)malloc((size_t)text.len + 1);
    CHECK(job.text, "memory for %lld bytes of text", text.len);
    memcpy(job.text, text.ptr, (size_t)text.len);
    job.text[text.len] = 0;
    SET(job.said, str(said));
    tl_start_job(app, &job);
}

// The preview of file `f`, asking a thread for it the first time. Null while it loads.
static TlPreview* tl_preview(App* app, FileItem* f) {
    for (i32 i = 0; i < g_tl_previews.count; i++) {
        TlPreview* p = &g_tl_previews[i];
        if (p->id != f->id) continue;
        if (!p->done) return 0;
        if (p->rgba) {
            p->view = texture_from_rgba(p->rgba, p->w, p->h);
            free(p->rgba);
            p->rgba = 0;
        }
        return p;
    }
    TlJob job = {};
    job.kind = TL_JOB_PREVIEW;
    job.file_id = f->id;
    job.image = str_eq(f->kind, str("image"));
    TlPreview* p = list_push(&g_tl_previews);
    p->id = f->id;
    if (!file_full_path(&app->config, f, job.path)) {
        p->done = true;  // nothing to show
        return p;
    }
    tl_start_job(app, &job);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Text

static void small_begin() { ImGui::PushFont(0, TL_SMALL); }
static void small_end() { ImGui::PopFont(); }

static ImU32 col_alpha(ImU32 c, f32 alpha) {
    u32 a = (u32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
    return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// The next visual line of a text wrapped to `width`: [at, *stop) is drawn; returns where the
// line after starts. Paragraphs break at '\n'.
static const char* wrap_line(const char* at, const char* end, f32 width, const char** stop) {
    ImFont* font = ImGui::GetFont();
    f32 size = ImGui::GetFontSize();
    const char* nl = (const char*)memchr(at, '\n', (size_t)(end - at));
    const char* para = nl ? nl : end;
    const char* e = at < para ? font->CalcWordWrapPosition(size, at, para, width) : at;
    if (e <= at && at < para) {
        // A word wider than the line: at least one character.
        e = at + 1;
        while (e < para && ((u8)*e & 0xC0) == 0x80) e++;
    }
    *stop = e;
    if (e >= para) return nl ? nl + 1 : end;
    while (e < para && (*e == ' ' || *e == '\t')) e++;  // the blanks a wrap leaves
    return e;
}

static i32 wrap_count(Str text, f32 width) {
    const char* at = text.ptr;
    const char* end = text.ptr + text.len;
    i32 n = 0;
    while (at < end) {
        const char* stop;
        at = wrap_line(at, end, width, &stop);
        n++;
    }
    return n > 0 ? n : 1;
}

// A run of a text in one colour (and an optional background), up to byte `to`.
struct Span {
    i64 to;
    ImU32 col;
    ImU32 bg;
};

// Draws `text` wrapped to `width`, coloured by `spans` (in order, covering it). Returns its height.
static f32 draw_spans(ImDrawList* dl, ImVec2 pos, Str text, f32 width, Span* spans, i32 count) {
    ImFont* font = ImGui::GetFont();
    f32 size = ImGui::GetFontSize();
    f32 lh = ImGui::GetTextLineHeight();
    const char* base = text.ptr;
    const char* end = text.ptr + text.len;
    const char* at = base;
    f32 y = pos.y;
    i32 lines = 0;
    while (at < end) {
        const char* stop;
        const char* next = wrap_line(at, end, width, &stop);
        i64 ls = at - base, le = stop - base;
        i64 from = 0;
        for (i32 i = 0; i < count; i++) {
            i64 a = from > ls ? from : ls;
            i64 b = spans[i].to < le ? spans[i].to : le;
            from = spans[i].to;
            if (a >= b) continue;
            f32 x = pos.x + font->CalcTextSizeA(size, FLT_MAX, 0, at, base + a).x;
            if (spans[i].bg) {
                f32 w = font->CalcTextSizeA(size, FLT_MAX, 0, base + a, base + b).x;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + lh), spans[i].bg, 2);
            }
            if (spans[i].col >> IM_COL32_A_SHIFT) dl->AddText(font, size, ImVec2(x, y), spans[i].col, base + a, base + b);
        }
        y += lh;
        lines++;
        at = next;
    }
    return (lines > 0 ? lines : 1) * lh;
}

static f32 draw_wrapped(ImDrawList* dl, ImVec2 pos, Str text, f32 width, ImU32 col) {
    Span s = {text.len, col, 0};
    return draw_spans(dl, pos, text, width, &s, 1);
}

static bool ascii_eq_nocase(const char* a, const char* b, i64 n) {
    for (i64 i = 0; i < n; i++)
        if (tolower((u8)a[i]) != tolower((u8)b[i])) return false;
    return true;
}

// Spans marking each case-insensitive `term` in `text`, into scratch.
static i32 found_spans(Str text, Str term, Span** out) {
    i32 cap = 64, n = 0;
    Span* spans = PUSH_ARRAY(scratch(), Span, cap * 2 + 1);
    ImU32 plain = ImGui::GetColorU32(ImGuiCol_Text);
    if (term.len) {
        for (i64 i = 0; i + term.len <= text.len && n < cap * 2 - 1;) {
            if (ascii_eq_nocase(text.ptr + i, term.ptr, term.len)) {
                spans[n++] = {i, plain, 0};
                spans[n++] = {i + term.len, plain, TL_FOUND};
                i += term.len;
            } else {
                i++;
            }
        }
    }
    spans[n++] = {text.len, plain, 0};
    *out = spans;
    return n;
}

// How a line's text is drawn: `shown` of it (0..1) in `done`, the rest in `rest`. While playing,
// the word being heard is in the accent colour.
static i32 progress_spans(Str text, f64 shown, ImU32 done, ImU32 rest, bool playing, Span* spans) {
    i64 total = utf8_count(text);
    if (total < 1) total = 1;
    if (shown < 0) shown = 0;
    if (shown > 1) shown = 1;
    i64 cut_chars = (i64)floor(shown * total + 0.5);
    if (cut_chars > total) cut_chars = total;
    i64 cut = utf8_offset(text, cut_chars);
    i64 said = cut, word = cut;
    if (playing && cut < text.len) {
        // The word being heard: from the cut back to the last space, on to the next one.
        said = cut;
        while (said > 0 && text.ptr[said - 1] != ' ') said--;
        word = cut;
        while (word < text.len && text.ptr[word] != ' ') word++;
    }
    i32 n = 0;
    spans[n++] = {said, done, 0};
    if (word > said) spans[n++] = {word, accent_u32(), 0};
    spans[n++] = {text.len, rest, 0};
    return n;
}

// `text` cut with "…" to fit `width`.
static Str fit_text(Str text, f32 width) {
    if (ImGui::CalcTextSize(text.ptr, text.ptr + text.len).x <= width) return text;
    f32 dots = ImGui::CalcTextSize("…").x;
    i64 n = text.len;
    while (n > 0) {
        n--;
        while (n > 0 && ((u8)text.ptr[n] & 0xC0) == 0x80) n--;
        if (ImGui::CalcTextSize(text.ptr, text.ptr + n).x + dots <= width) break;
    }
    return str_fmt(scratch(), "%.*s…", (int)n, text.ptr);
}

// How many words (3+ letters) two texts share, ignoring case and punctuation.
static i32 words_of(Str s, Str* out, i32 cap) {
    i32 n = 0;
    for (i64 i = 0; i < s.len && n < cap;) {
        auto is_word = [](u8 c) { return isalnum(c) || c == '\'' || c >= 0x80; };
        while (i < s.len && !is_word((u8)s.ptr[i])) i++;
        i64 start = i;
        while (i < s.len && is_word((u8)s.ptr[i])) i++;
        Str w = {s.ptr + start, i - start};
        if (utf8_count(w) >= 3) out[n++] = w;
    }
    return n;
}

static i32 shared_words(Str a, Str b) {
    Str* wa = PUSH_ARRAY(scratch(), Str, 256);
    Str* wb = PUSH_ARRAY(scratch(), Str, 256);
    i32 na = words_of(a, wa, 256), nb = words_of(b, wb, 256);
    i32 shared = 0;
    for (i32 i = 0; i < na; i++) {
        bool seen = false;
        for (i32 j = 0; j < i && !seen; j++) seen = wa[j].len == wa[i].len && ascii_eq_nocase(wa[j].ptr, wa[i].ptr, wa[i].len);
        if (seen) continue;
        for (i32 j = 0; j < nb; j++) {
            if (wb[j].len == wa[i].len && ascii_eq_nocase(wb[j].ptr, wa[i].ptr, wa[i].len)) {
                shared++;
                break;
            }
        }
    }
    return shared;
}

// ---------------------------------------------------------------------------------------------
// Loading a day

static bool tl_is_today(TimelineState* s) {
    char today[11];
    today_day(today);
    return strcmp(s->day, today) == 0;
}

// A fingerprint of what the Show filter lets through, to know when to rebuild the rows.
static u64 tl_filter_sig(App* app) {
    u64 h = 1469598103934665603ull;
    auto mix = [&h](Names* n) {
        for (i32 i = 0; i < n->count; i++)
            for (const char* c = n->items[i].s; *c; c++) h = (h ^ (u8)tolower((u8)*c)) * 1099511628211ull;
        h = (h ^ 0xff) * 1099511628211ull;
    };
    mix(&app->config.ui.hidden);
    mix(&app->config.capture.excluded_apps);
    return h ^ (app->config.ui.show_dismissed ? 1 : 0);
}

static int tl_chip_order(const void* a, const void* b) {
    const TlChip* x = (const TlChip*)a;
    const TlChip* y = (const TlChip*)b;
    if (x->lines != y->lines) return y->lines - x->lines;
    return strcmp(x->speaker.ptr, y->speaker.ptr);
}

static void timeline_load(App* app, TimelineState* s) {
    bool same = s->loaded && strcmp(s->loaded_day, s->day) == 0;
    i64 old_max = s->max_id;
    arena_reset(&s->arena);
    s->lines = {};
    s->files = {};
    s->chips = {};
    s->line_chip = 0;
    if (app->storage.db) {
        f64 from, to;
        day_bounds(s->day, &from, &to);
        s->lines = db_lines_between(app->storage.db, &s->arena, from, to, TL_DAY_LIMIT);
        s->files = db_files_between(app->storage.db, &s->arena, from, to);
    }
    s->max_id = 0;
    for (i32 i = 0; i < s->lines.count; i++)
        if (s->lines.items[i].id > s->max_id) s->max_id = s->lines.items[i].id;
    if (same && old_max && tl_is_today(s)) {
        f64 now = now_ts();
        for (i32 i = 0; i < s->lines.count; i++)
            if (s->lines.items[i].id > old_max) list_push(&s->fresh, {s->lines.items[i].id, now});
    }
    if (!same) {
        s->fresh.count = 0;
        s->new_page = true;
        s->scroll_to = 0;
        s->rows.count = 0;  // other ids: no heights to keep
    }

    // The apps heard on the page, most lines first.
    List<TlChip> chips = {};
    s->line_chip = PUSH_ARRAY(&s->arena, i32, s->lines.count);
    for (i32 i = 0; i < s->lines.count; i++) {
        Str sp = s->lines.items[i].speaker;
        i32 c = 0;
        while (c < chips.count && !str_eq(chips.items[c].speaker, sp)) c++;
        if (c == chips.count) list_push(&chips, {sp, 0, false});
        chips.items[c].lines++;
        s->line_chip[i] = c;
    }
    // Sorting moves the chips; the lines keep pointing at them by name.
    TlChip* sorted = PUSH_ARRAY(&s->arena, TlChip, chips.count);
    if (chips.count) memcpy(sorted, chips.items, sizeof(TlChip) * chips.count);
    qsort(sorted, (size_t)chips.count, sizeof(TlChip), tl_chip_order);
    i32* moved = PUSH_ARRAY(scratch(), i32, chips.count);
    for (i32 c = 0; c < chips.count; c++)
        for (i32 k = 0; k < chips.count; k++)
            if (str_eq(sorted[k].speaker, chips.items[c].speaker)) moved[c] = k;
    for (i32 i = 0; i < s->lines.count; i++) s->line_chip[i] = moved[s->line_chip[i]];
    s->chips = {sorted, chips.count};
    list_free(&chips);

    s->lines_version = app->worker.lines_version;
    s->files_version = app->storage.files_version;
    s->loaded = true;
    SET(s->loaded_day, str(s->day));
    s->rows_dirty = true;
}

// Oldest at the top, like reading a day: lines and files merged by time, long gaps marked.
static void tl_build_rows(App* app, TimelineState* s) {
    for (i32 c = 0; c < s->chips.count; c++) s->chips.items[c].visible = speaker_visible(app, s->chips.items[c].speaker);
    List<TlRow> old = s->rows;
    s->rows = {};
    i32 li = 0, fi = 0;
    bool have_last = false;
    f64 last = 0;
    while (li < s->lines.count || fi < s->files.count) {
        bool line = li < s->lines.count && (fi >= s->files.count || s->lines.items[li].start_ts <= s->files.items[fi].ts);
        TlRow r = {};
        if (line) {
            if (!s->chips.items[s->line_chip[li]].visible) {
                li++;
                continue;
            }
            r.kind = TL_ROW_LINE;
            r.index = li;
            r.key = s->lines.items[li].id;
            r.ts = s->lines.items[li].start_ts;
            li++;
        } else {
            r.kind = TL_ROW_FILE;
            r.index = fi;
            r.key = s->files.items[fi].id;
            r.ts = s->files.items[fi].ts;
            fi++;
        }
        if (have_last && r.ts - last > TL_GAP) {
            TlRow g = {};
            g.kind = TL_ROW_GAP;
            g.key = (i64)(last * 1000);
            g.ts = last;
            g.to = r.ts;
            list_push(&s->rows, g);
        }
        list_push(&s->rows, r);
        have_last = true;
        last = r.ts;
    }
    // Keep the heights already measured (the old rows are in the same order).
    i32 j = 0;
    for (i32 i = 0; i < s->rows.count; i++) {
        TlRow* r = &s->rows.items[i];
        while (j < old.count && old.items[j].ts < r->ts) j++;
        for (i32 k = j - 1 < 0 ? 0 : j - 1; k < old.count && k < j + 4; k++) {
            if (old.items[k].kind == r->kind && old.items[k].key == r->key) {
                r->h = old.items[k].h;
                r->width = old.items[k].width;
                break;
            }
        }
    }
    list_free(&old);
    s->filter_sig = tl_filter_sig(app);
    s->rows_dirty = false;
}

static void tl_open_day(App* app, TimelineState* s, const char* day) {
    if (strcmp(day, s->day) != 0) s->picked = false;
    SET(s->day, str(day));
    s->calendar_open = false;
    timeline_load(app, s);
}

static void tl_step_day(App* app, TimelineState* s, i32 by) {
    f64 from, to;
    day_bounds(s->day, &from, &to);
    char day[11], today[11];
    day_of(from + 12 * 3600 + by * 86400.0, day);
    today_day(today);
    if (strcmp(day, today) <= 0) tl_open_day(app, s, day);
}

// Scrolls to and picks the line to show: by id if given, else the one within two minutes of `ts`
// sharing the most of `words` (a quote from the journal), else the nearest.
static void tl_show(TimelineState* s, f64 ts, i64 id, Str words) {
    Line* by_id = 0;
    Line* nearest = 0;
    Line* best = 0;
    i32 best_n = 0;
    for (i32 i = 0; i < s->lines.count; i++) {
        Line* l = &s->lines.items[i];
        if (id && l->id == id) by_id = l;
        if (!nearest || fabs(l->start_ts - ts) < fabs(nearest->start_ts - ts)) nearest = l;
        if (words.len && fabs(l->start_ts - ts) < 120) {
            i32 n = shared_words(l->text, words);
            if (n > best_n || (n == best_n && n > 0 && fabs(l->start_ts - ts) < fabs(best->start_ts - ts))) {
                best = l;
                best_n = n;
            }
        }
    }
    Line* line = by_id ? by_id : best ? best : nearest;
    if (!line) return;
    s->scroll_to = line->id;
    s->picked = true;
    s->pick_a = s->pick_b = line->start_ts;
}

// Opens the page of `ts`'s day (if not on it) and shows the line.
static void tl_go(App* app, TimelineState* s, f64 ts, i64 id, Str words) {
    char day[11];
    day_of(ts, day);
    if (!(s->loaded && strcmp(day, s->day) == 0)) tl_open_day(app, s, day);
    tl_show(s, ts, id, words);
}

// Show the moment `ts` (and the line where `said` was said, if found).
static void timeline_jump(App* app, TimelineState* s, f64 ts, Str said) {
    tl_go(app, s, ts, 0, said);
}

// The picked stretch: its start and end times and how many lines it holds. False if none.
static bool tl_picked(TimelineState* s, f64* from, f64* to, i32* count) {
    if (!s->picked) return false;
    f64 lo = s->pick_a < s->pick_b ? s->pick_a : s->pick_b;
    f64 hi = s->pick_a < s->pick_b ? s->pick_b : s->pick_a;
    f64 end = hi;
    i32 n = 0;
    for (i32 i = 0; i < s->lines.count; i++) {
        Line* l = &s->lines.items[i];
        if (l->start_ts < lo || l->start_ts > hi) continue;
        n++;
        if (l->end_ts > end) end = l->end_ts;
    }
    *from = lo;
    *to = end;
    *count = n;
    return true;
}

static void tl_copy_picked(App* app, TimelineState* s, ClipKind kind) {
    f64 from, to;
    i32 n;
    if (!tl_picked(s, &from, &to, &n)) return;
    tl_copy_range(app, from, to, hidden_speakers(app), kind);
}

// One line's audio: its own app only, nothing else heard at the time.
static void tl_copy_line_audio(App* app, TimelineState* s, Line* line) {
    Names others = {};
    for (i32 c = 0; c < s->chips.count; c++)
        if (!str_eq(s->chips.items[c].speaker, line->speaker)) names_set(&others, s->chips.items[c].speaker, true);
    tl_copy_range(app, line->start_ts, line->end_ts, others, CLIP_AUDIO);
}

static PlayMode tl_mode(bool shift, bool ctrl) {
    if (shift && ctrl) return PLAY_LINE;
    if (shift) return PLAY_ALL_AT_LINE;
    if (ctrl) return PLAY_STREAM_ON;
    return PLAY_ALL_ON;
}

// Plays from `line`, or stops it if it is already playing that way.
static void tl_play(App* app, Line* line, PlayMode mode) {
    if (app->clips.playing_line == line->id && app->clips.play_mode == mode) {
        play_stop(app);
        return;
    }
    Names hidden = hidden_speakers(app);
    play(app, line->id, mode, &hidden);
    list_free(&hidden);
}

// Whether `l` would be heard if `from`'s play button were pressed in `mode`.
static bool would_play(Line* from, PlayMode mode, Line* l) {
    switch (mode) {
    case PLAY_LINE: return l->id == from->id;
    case PLAY_ALL_AT_LINE: return l->end_ts >= from->start_ts - 0.3 && l->start_ts <= from->end_ts + 0.4;
    case PLAY_STREAM_ON: return str_eq(l->speaker, from->speaker) && l->start_ts >= from->start_ts;
    case PLAY_ALL_ON: return l->start_ts >= from->start_ts;
    }
    return false;
}

static Line* tl_line_by_id(TimelineState* s, i64 id) {
    for (i32 i = 0; i < s->lines.count; i++)
        if (s->lines.items[i].id == id) return &s->lines.items[i];
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Search

static void tl_go_to_match(App* app, TimelineState* s, i32 index) {
    if (index < 0 || index >= s->results.count) return;
    Line* l = &s->results.items[index];
    s->find_at = index + 1;
    tl_go(app, s, l->start_ts, l->id, {});
}

// Enter / Shift+Enter: the next older or newer match, wrapping around.
static void tl_find_step(App* app, TimelineState* s, bool older) {
    i32 n = s->results.count;
    if (n == 0) {
        s->find_pending = s->search_due > 0;
        return;
    }
    i32 index = s->find_at == 0 ? 0 : older ? s->find_at % n : (s->find_at - 2 + n) % n;
    tl_go_to_match(app, s, index);
}

static void tl_run_search(App* app, TimelineState* s) {
    s->search_due = 0;
    arena_reset(&s->search_arena);
    s->results = {};
    s->find_at = 0;
    Str q = str_trim(str(s->search));
    if (!q.len || !app->storage.db) return;
    Slice<Line> all = db_search(app->storage.db, &s->search_arena, q, TL_SEARCH_LIMIT);
    // Only what the Show filter lets through, so every match can be shown.
    i32 n = 0;
    for (i32 i = 0; i < all.count; i++)
        if (speaker_visible(app, all.items[i].speaker)) all.items[n++] = all.items[i];
    s->results = {all.items, n};
    if (s->find_pending) {
        s->find_pending = false;
        tl_find_step(app, s, true);
    }
}

static int tl_search_callback(ImGuiInputTextCallbackData* d) {
    TimelineState* s = (TimelineState*)d->UserData;
    if (s->search_cursor_end) {
        d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
        s->search_cursor_end = false;
    }
    return 0;
}

static void tl_clear_search(TimelineState* s) {
    s->search[0] = 0;
    s->search_due = 0;
    arena_reset(&s->search_arena);
    s->results = {};
    s->find_at = 0;
    s->find_pending = false;
}

static bool tl_button(const char* text, ImVec2 size, bool enabled = true, bool selected = false) {
    ImGui::BeginDisabled(!enabled);
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    bool clicked = ImGui::Button(text, size);
    if (selected) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    return clicked;
}

// The search box, the match count and arrows; ◀ day ▶ Today on the right.
static void tl_search_bar(App* app, TimelineState* s, f32 left, f32 right, f32* next_frame_ms) {
    f32 u = ImGui::GetStyle().FontScaleDpi;
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(weak_text(), "%s", ICON_SEARCH);
    ImGui::SameLine();
    if (s->focus_search) {
        ImGui::SetKeyboardFocusHere();  // with what is there selected, ready to type over
        s->focus_search = false;
    } else if (s->search_refocus) {
        ImGui::SetKeyboardFocusHere();
        s->search_refocus = false;
        s->search_cursor_end = true;
    }
    ImGui::SetNextItemWidth(320 * u);
    char before[256];
    SET(before, str(s->search));
    bool enter = ImGui::InputTextWithHint("##tl_search", "Search everything heard", s->search, sizeof(s->search),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                          tl_search_callback, s);
    if (strcmp(before, s->search) != 0) {
        s->search_due = now_ts() + 0.15;
        s->find_at = 0;
        s->find_pending = false;
        if (!str_trim(str(s->search)).len) tl_clear_search(s);
    }
    bool has_query = str_trim(str(s->search)).len > 0;
    i32 step = 0;  // 1 older, -1 newer
    // Enter: the next older match (the newest first); Shift+Enter goes back. The box keeps focus.
    if (enter && has_query) {
        step = ImGui::GetIO().KeyShift ? -1 : 1;
        s->search_refocus = true;
    }
    if (has_query) {
        i32 n = s->results.count;
        const char* count;
        if (s->search_due > 0 && n == 0) count = "…";
        else if (n == 0) count = "No matches";
        else {
            const char* total = n >= TL_SEARCH_LIMIT ? tmp("%d+", n) : tmp("%d", n);
            count = s->find_at ? tmp("%d of %s", s->find_at, total) : total;
        }
        // Fixed width, so the arrows don't move as the count changes.
        ImGui::SameLine();
        f32 w = 86 * u, tw = ImGui::CalcTextSize(count).x;
        f32 x = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x + (w - tw) / 2);
        ImGui::TextColored(weak_text(), "%s", count);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(x + w);
        if (icon_button(ICON_CHEVRON_UP, "Newer", "tl_newer")) step = -1;
        ImGui::SameLine(0, 2 * u);
        if (icon_button(ICON_CHEVRON_DOWN, "Older", "tl_older")) step = 1;
        ImGui::SameLine(0, 2 * u);
        if (icon_button(ICON_X, "Clear", "tl_clear")) tl_clear_search(s);
    }
    f32 after_search = ImGui::GetItemRectMax().x;

    // The day: ◀ day ▶ Today, from the right.
    char today[11];
    today_day(today);
    bool on_today = strcmp(s->day, today) == 0;
    f32 sp = st.ItemSpacing.x;
    f32 w_today = ImGui::CalcTextSize("Today").x + st.FramePadding.x * 2;
    f32 w_arrow = 28 * u, w_day = 210 * u;
    f32 x_today = right - w_today, x_next = x_today - sp - w_arrow, x_day = x_next - sp - w_day, x_prev = x_day - sp - w_arrow;
    if (x_prev < after_search + sp) x_prev = after_search + sp;  // a narrow window: no overlap
    x_day = x_prev + w_arrow + sp;
    x_next = x_day + w_day + sp;
    x_today = x_next + w_arrow + sp;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(x_prev, ImGui::GetCursorScreenPos().y));
    if (tl_button(tmp("%s##tl_prev", ICON_CHEVRON_LEFT), ImVec2(w_arrow, 0))) tl_step_day(app, s, -1);
    ImGui::SetItemTooltip("Day before (←)");
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(x_day, ImGui::GetCursorScreenPos().y));
    bool was_open = s->calendar_open;
    if (tl_button(tmp("%s##tl_day", label(ICON_CALENDAR, page_name(s->day))), ImVec2(w_day, 0), true, s->calendar_open)) {
        if (was_open) {
            s->calendar_open = false;
        } else {
            i32 y, m, d;
            CHECK(sscanf_s(s->day, "%d-%d-%d", &y, &m, &d) == 3, "the day shown as YYYY-MM-DD");
            s->cal_year = y;
            s->cal_month = m;
            s->calendar_open = true;
            arena_reset(&s->days_arena);
            s->days = app->storage.db ? db_days(app->storage.db, &s->days_arena) : Slice<DayCount>{};
        }
    }
    ImGui::SetItemTooltip("Pick a day");
    s->cal_button_min = ImGui::GetItemRectMin();
    s->cal_button_max = ImGui::GetItemRectMax();
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(x_next, ImGui::GetCursorScreenPos().y));
    if (tl_button(tmp("%s##tl_next", ICON_CHEVRON_RIGHT), ImVec2(w_arrow, 0), !on_today)) tl_step_day(app, s, 1);
    ImGui::SetItemTooltip("Next day (→)");
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(x_today, ImGui::GetCursorScreenPos().y));
    if (tl_button("Today##tl_today", ImVec2(w_today, 0), !on_today)) tl_open_day(app, s, today);

    // Searching waits for typing to pause.
    if (s->search_due > 0) {
        f64 left_s = s->search_due - now_ts();
        if (left_s <= 0) tl_run_search(app, s);
        else {
            f32 ms = (f32)(left_s * 1000) + 10;
            if (*next_frame_ms < 0 || *next_frame_ms > ms) *next_frame_ms = ms;
        }
    }
    if (step) tl_find_step(app, s, step > 0);
}

// ---------------------------------------------------------------------------------------------
// The Show filter

static void tl_show_chips(App* app, TimelineState* s, f32 left, f32 right) {
    f32 u = ImGui::GetStyle().FontScaleDpi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y));
    // The fixed controls come first, so chips appearing or going never move them.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(weak_text(), "Show");
    ImGui::SameLine();
    if (ImGui::Button(label(ICON_LIST, "All apps…"))) {
        // Its own window, opened by the main thread (main.cpp) and drawn by timeline_apps_draw.
        s->apps_open = true;
        ui_open_apps(app);
        arena_reset(&s->apps_arena);
        s->apps = app->storage.db ? db_speakers(app->storage.db, &s->apps_arena) : Slice<SpeakerCount>{};
    }
    ImGui::SameLine();
    ImVec2 at = ImGui::GetCursorScreenPos();
    f32 fh = ImGui::GetFrameHeight();
    dl->AddLine(ImVec2(at.x, at.y + 3 * u), ImVec2(at.x, at.y + fh - 3 * u), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(1, fh));
    f32 gap = 6 * u;
    f32 chip_h = 24 * u;
    f32 top_pad = (fh - chip_h) / 2;
    ImGui::PushFont(0, 13);
    for (i32 c = 0; c < s->chips.count; c++) {
        TlChip* chip = &s->chips.items[c];
        bool dismissed = names_has(&app->config.capture.excluded_apps, chip->speaker);
        if (dismissed && !app->config.ui.show_dismissed) continue;
        bool on = !names_has(&app->config.ui.hidden, chip->speaker);
        Str name = app_name(chip->speaker);
        f32 w = ImGui::CalcTextSize(name.ptr, name.ptr + name.len).x + 38 * u;
        // Wrap: on this line if it fits, else the next.
        f32 last_x = ImGui::GetItemRectMax().x;
        if (last_x + gap + w <= right) ImGui::SameLine(0, gap);
        ImVec2 p = ImGui::GetCursorScreenPos();
        p.y += top_pad;
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(c);
        bool clicked = ImGui::InvisibleButton("chip", ImVec2(w, chip_h));
        bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("%d lines loaded%s", chip->lines, dismissed ? " · excluded" : "");
        ImGui::PopID();
        ImVec2 q = ImVec2(p.x + w, p.y + chip_h);
        ImU32 fill = on ? accent_u32(0.25f) : ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
        dl->AddRectFilled(p, q, fill, 12 * u);
        if (hovered) dl->AddRect(p, q, ImGui::GetColorU32(ImGuiCol_Border), 12 * u);
        ImU32 tc = on ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        f32 is = 16 * u;
        ImVec2 ic = ImVec2(p.x + 16 * u - is / 2, p.y + (chip_h - is) / 2);
        ImTextureID tex = str_eq(chip->speaker, str("me")) ? 0 : app_icon(app, chip->speaker);
        if (tex) dl->AddImage(ImTextureRef(tex), ic, ImVec2(ic.x + is, ic.y + is));
        else {
            // The glyph at the icons' size, centred where an icon would be.
            const char* glyph = str_eq(chip->speaker, str("me")) ? ICON_MIC : ICON_APP_WINDOW;
            ImFont* font = ImGui::GetFont();
            f32 gs = 16 * u;
            ImVec2 gsz = font->CalcTextSizeA(gs, FLT_MAX, 0, glyph);
            dl->AddText(font, gs, ImVec2(ic.x + (is - gsz.x) / 2, p.y + (chip_h - gsz.y) / 2), tc, glyph);
        }
        dl->AddText(ImVec2(p.x + 29 * u, p.y + (chip_h - ImGui::GetFontSize()) / 2), tc, name.ptr, name.ptr + name.len);
        if (clicked) set_hidden(app, chip->speaker, on);
    }
    ImGui::PopFont();
}

// The "All apps…" window (its own OS window, "lifelog apps"): every app ever heard, with a
// filter, Show all, Hide all, and "Show excluded apps". False once it should close.
static bool timeline_apps_draw(App* app, TimelineState* s) {
    if (!s->apps_open) return false;
    f32 u = ImGui::GetStyle().FontScaleDpi;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    if (ImGui::Begin("apps", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(0, 20);
        ImGui::TextUnformatted("Apps shown in the timeline");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2 * u));
        ImGui::SetNextItemWidth(160 * u);
        ImGui::InputTextWithHint("##tl_apps_filter", "Filter", s->apps_filter, sizeof(s->apps_filter));
        ImGui::SameLine();
        if (ImGui::Button("Show all")) {
            app->config.ui.hidden.count = 0;
            config_save_names("ui", "hidden", &app->config.ui.hidden);
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide all")) {
            for (i32 i = 0; i < s->apps.count; i++) names_set(&app->config.ui.hidden, s->apps.items[i].speaker, true);
            config_save_names("ui", "hidden", &app->config.ui.hidden);
        }
        bool show = app->config.ui.show_dismissed;
        if (ImGui::Checkbox("Show excluded apps", &show)) set_show_dismissed(app, show);
        ImGui::Separator();
        ImGui::BeginChild("##tl_apps_list");
        if (!s->apps.count) ImGui::TextDisabled("Nothing heard yet");
        Str filter = str_lower(scratch(), str_trim(str(s->apps_filter)));
        for (i32 i = 0; i < s->apps.count; i++) {
            SpeakerCount* sc = &s->apps.items[i];
            if (filter.len && str_find(str_lower(scratch(), sc->speaker), filter) < 0) continue;
            ImGui::PushID(i);
            bool on = !names_has(&app->config.ui.hidden, sc->speaker);
            if (ImGui::Checkbox("##on", &on)) set_hidden(app, sc->speaker, !on);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            draw_app_icon(app, sc->speaker, 18 * u);
            ImGui::SameLine();
            Str name = app_name(sc->speaker);
            ImGui::TextUnformatted(name.ptr, name.ptr + name.len);
            if (names_has(&app->config.capture.excluded_apps, sc->speaker)) {
                ImGui::SameLine();
                small_begin();
                ImGui::TextDisabled("excluded");
                small_end();
            }
            small_begin();
            const char* info = tmp("%lld lines · %s", sc->lines, ago_text(sc->last_ts));
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(info).x);
            ImGui::TextDisabled("%s", info);
            small_end();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::End();
    return true;
}

// ---------------------------------------------------------------------------------------------
// The calendar: a month to pick a day from; days with something heard stand out.

static i32 tl_day_lines(TimelineState* s, const char* day) {
    for (i32 i = 0; i < s->days.count; i++)
        if (strcmp(s->days.items[i].day, day) == 0) return s->days.items[i].lines;
    return 0;
}

static void tl_calendar(App* app, TimelineState* s) {
    if (!s->calendar_open) return;
    f32 u = ImGui::GetStyle().FontScaleDpi;
    char today[11];
    today_day(today);
    i32 ty, tm, td;
    CHECK(sscanf_s(today, "%d-%d-%d", &ty, &tm, &td) == 3, "today as YYYY-MM-DD");
    const char* picked = 0;
    ImGui::SetNextWindowPos(ImVec2(s->cal_button_max.x - (7 * 36 + 12) * u, s->cal_button_max.y + 4 * u));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * u);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2 * u, 2 * u));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
    ImGui::Begin("##tl_calendar", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    i32 year = s->cal_year, month = s->cal_month;
    if (ImGui::Button(tmp("%s##cal_prev", ICON_CHEVRON_LEFT), ImVec2(34 * u, 26 * u))) {
        if (--s->cal_month < 1) {
            s->cal_month = 12;
            s->cal_year--;
        }
    }
    ImGui::SetItemTooltip("Month before");
    ImGui::SameLine();
    const char* title = tmp("%s %d", MONTHS[month - 1], year);
    f32 tw = 5 * 36 * u - 4 * u;
    f32 x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + (tw - ImGui::CalcTextSize(title).x) / 2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(x + tw + 2 * u);
    bool later = year * 12 + month < ty * 12 + tm;
    if (tl_button(tmp("%s##cal_next", ICON_CHEVRON_RIGHT), ImVec2(34 * u, 26 * u), later)) {
        if (++s->cal_month > 12) {
            s->cal_month = 1;
            s->cal_year++;
        }
    }
    ImGui::SetItemTooltip("Month after");
    ImGui::Dummy(ImVec2(0, 2 * u));
    static const char* NAMES[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    small_begin();
    for (i32 i = 0; i < 7; i++) {
        if (i) ImGui::SameLine();
        f32 cx = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(cx + (34 * u - ImGui::CalcTextSize(NAMES[i]).x) / 2);
        ImGui::TextDisabled("%s", NAMES[i]);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(cx + 34 * u);
        ImGui::Dummy(ImVec2(0, 0));
    }
    small_end();
    f64 first = local_ts(year, month, 1, 12, 0);
    i32 lead = local_time(first).weekday;
    i32 col = 0;
    for (i32 i = 0; i < lead; i++) {
        if (col) ImGui::SameLine();
        ImGui::Dummy(ImVec2(34 * u, 30 * u));
        col++;
    }
    for (i32 d = 1; d <= 31; d++) {
        LocalTime t = local_time(local_ts(year, month, d, 12, 0));
        if (t.month != month) break;
        char day[11];
        snprintf(day, sizeof(day), "%04d-%02d-%02d", year, month, d);
        i32 n = tl_day_lines(s, day);
        bool selected = strcmp(day, s->day) == 0;
        bool is_today = strcmp(day, today) == 0;
        if (col) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, n > 0 ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : weak_text());
        if (n == 0 && !selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (is_today) {
            ImGui::PushStyleColor(ImGuiCol_Border, ACCENT);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
        }
        if (tl_button(tmp("%d##cal%d", d, d), ImVec2(34 * u, 30 * u), strcmp(day, today) <= 0, selected)) picked = tmp("%s", day);
        if (is_today) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        if (n == 0 && !selected) ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        if (n > 0) ImGui::SetItemTooltip("%d lines", n);
        col = (col + 1) % 7;
    }
    ImGui::Dummy(ImVec2(0, 2 * u));
    if (ImGui::Button("Today##cal_today")) picked = tmp("%s", today);
    s->cal_min = ImGui::GetWindowPos();
    s->cal_max = ImVec2(s->cal_min.x + ImGui::GetWindowSize().x, s->cal_min.y + ImGui::GetWindowSize().y);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (picked) {
        tl_open_day(app, s, picked);
        return;
    }
    // A click anywhere else closes it (the day button toggles it itself).
    if (ImGui::IsMouseClicked(0)) {
        ImVec2 m = ImGui::GetMousePos();
        bool inside = m.x >= s->cal_min.x && m.x < s->cal_max.x && m.y >= s->cal_min.y && m.y < s->cal_max.y;
        bool on_button = m.x >= s->cal_button_min.x && m.x < s->cal_button_max.x && m.y >= s->cal_button_min.y && m.y < s->cal_button_max.y;
        if (!inside && !on_button) s->calendar_open = false;
    }
}

// ---------------------------------------------------------------------------------------------
// The list

struct TlLayout {
    f32 u;
    f32 left, right;  // the column, in screen x
    f32 x_play, x_time, x_app, x_text, text_w;
    f32 x_file, file_w;
    f32 lh, small_lh;
};

// What happened in the list this frame, acted on after it is drawn.
enum TlActKind : u8 {
    TL_ACT_NONE,
    TL_ACT_COPY_PICKED,
    TL_ACT_LINE_TEXT,
    TL_ACT_LINE_AUDIO,
    TL_ACT_UNPICK,
};

struct TlFrame {
    TlActKind act;
    ClipKind act_clip;
    i32 act_line;
    bool clicked;
    f64 clicked_ts;
    bool clicked_shift;
    i32 play_line;     // index + 1
    PlayMode play_mode;
    i64 open_file;
    bool open_in_folder;
    i64 copy_path_file;
    i64 hover_play;
    bool animating;
    // Playback and the hover preview.
    f64 play_ts;       // the moment being heard (0: none)
    i64* heard;        // the lines heard at play_ts
    i32 heard_count;
    Line* preview_from;
    PlayMode preview_mode;
    // The picked stretch.
    bool has_stretch;
    f64 st_from, st_to;
    i32 st_count;
    Str term;          // the search, lower case
};

static bool tl_heard(TlFrame* f, i64 id) {
    for (i32 i = 0; i < f->heard_count; i++)
        if (f->heard[i] == id) return true;
    return false;
}

static f32 tl_gap_row(TimelineState* s, TlRow* r, TlLayout* L, f32 y, bool draw) {
    (void)s;
    f32 h = 4 * L->u + L->small_lh + 4 * L->u;
    if (draw) {
        small_begin();
        const char* t = tmp("· %s quiet ·", duration_text(r->to - r->ts));
        f32 w = ImGui::CalcTextSize(t).x;
        ImGui::GetWindowDrawList()->AddText(ImVec2((L->left + L->right - w) / 2, y + 4 * L->u), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
        small_end();
    }
    return h;
}

static f32 tl_line_row(App* app, TimelineState* s, TlRow* r, TlLayout* L, f32 y, TlFrame* f, bool draw) {
    Line* line = &s->lines.items[r->index];
    Str text = str_trim(line->text);
    f32 u = L->u;
    f32 text_h = wrap_count(text, L->text_w) * L->lh;
    f32 pad = (24 * u - L->lh) / 2;  // the first line sits centred on the play button
    f32 inner = 24 * u > pad + text_h ? 24 * u : pad + text_h;
    f32 h = 3 * u + inner + 3 * u;
    if (!draw) return h;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 rmin = ImVec2(L->left, y), rmax = ImVec2(L->right, y + h);
    ImGui::PushID((int)line->id);
    ImGui::PushID((int)(line->id >> 32));
    ImGui::SetCursorScreenPos(rmin);
    ImGui::SetNextItemAllowOverlap();
    bool clicked = ImGui::InvisibleButton("row", ImVec2(L->right - L->left, h));
    bool hovered = ImGui::IsItemHovered();
    bool picked = s->picked && line->start_ts >= (s->pick_a < s->pick_b ? s->pick_a : s->pick_b) &&
                  line->start_ts <= (s->pick_a < s->pick_b ? s->pick_b : s->pick_a);
    if (clicked) {
        f->clicked = true;
        f->clicked_ts = line->start_ts;
        f->clicked_shift = ImGui::GetIO().KeyShift;
    }
    // On a line inside a picked stretch of several: the stretch. Anywhere else: that line.
    bool stretch = f->has_stretch && f->st_count > 1 && picked;
    if (ImGui::BeginPopupContextItem("menu")) {
        if (stretch) {
            small_begin();
            ImGui::TextDisabled("%d lines picked · %s → %s", f->st_count, hm_text(f->st_from), hm_text(f->st_to));
            small_end();
            if (ImGui::MenuItem(label(ICON_COPY, tmp("Copy %d selected lines", f->st_count)))) {
                f->act = TL_ACT_COPY_PICKED;
                f->act_clip = CLIP_TEXT;
            }
            if (ImGui::MenuItem(label(ICON_AUDIO_LINES, "Copy selected audio"))) {
                f->act = TL_ACT_COPY_PICKED;
                f->act_clip = CLIP_AUDIO;
            }
            if (ImGui::MenuItem(label(ICON_FILE_TEXT, "Copy selected as a file"), "Ctrl+Enter")) {
                f->act = TL_ACT_COPY_PICKED;
                f->act_clip = CLIP_FILE;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(label(ICON_COPY, "Copy just this line"))) {
                f->act = TL_ACT_LINE_TEXT;
                f->act_line = r->index;
            }
            if (ImGui::MenuItem(label(ICON_X, "Clear selection"), "Esc")) f->act = TL_ACT_UNPICK;
        } else {
            if (ImGui::MenuItem(label(ICON_COPY, "Copy line"))) {
                f->act = TL_ACT_LINE_TEXT;
                f->act_line = r->index;
            }
            if (ImGui::MenuItem(label(ICON_AUDIO_LINES, "Copy line's audio"), 0, false, line->audio)) {
                f->act = TL_ACT_LINE_AUDIO;
                f->act_line = r->index;
            }
            ImGui::Separator();
            small_begin();
            ImGui::TextDisabled("Click a line, Shift+click another\nto pick several");
            small_end();
        }
        ImGui::EndPopup();
    }

    bool hearing = f->play_ts > 0 && tl_heard(f, line->id);
    bool coming = f->preview_from && line->audio && would_play(f->preview_from, f->preview_mode, line);
    ImU32 fill = 0;
    if (picked) fill = accent_u32(0.18f);
    else if (hearing || coming) fill = accent_u32(0.10f);
    else if (hovered) fill = ImGui::GetColorU32(ImGuiCol_FrameBg);
    if (fill) dl->AddRectFilled(ImVec2(rmin.x + 4 * u, rmin.y), ImVec2(rmax.x - 4 * u, rmax.y), fill, 4 * u);
    if (coming) dl->AddRectFilled(ImVec2(rmin.x + 4 * u, rmin.y + 2 * u), ImVec2(rmin.x + 7 * u, rmax.y - 2 * u), accent_u32(), 1);

    f32 top = y + 3 * u;
    if (line->audio) {
        bool is_playing = app->clips.playing_line == line->id;
        ImGui::SetCursorScreenPos(ImVec2(L->x_play, top));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushFont(0, 15);
        // No tooltip: hovering already marks what it would play.
        bool pressed = ImGui::Button(is_playing ? ICON_SQUARE "##play" : ICON_PLAY "##play", ImVec2(24 * u, 24 * u));
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) f->hover_play = line->id;
        if (pressed) {
            f->play_line = r->index + 1;
            f->play_mode = tl_mode(ImGui::GetIO().KeyShift, ImGui::GetIO().KeyCtrl);
        }
    }
    const char* clock = clock_text(line->start_ts);
    f32 cw = ImGui::CalcTextSize(clock).x;
    f32 ty = top + pad;
    dl->AddText(ImVec2(L->x_time + (58 * u - cw) / 2, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), clock);
    f32 is = 16 * u;
    ImVec2 ic = ImVec2(L->x_app, top + (24 * u - is) / 2);
    ImTextureID tex = str_eq(line->speaker, str("me")) ? 0 : app_icon(app, line->speaker);
    if (tex) dl->AddImage(ImTextureRef(tex), ic, ImVec2(ic.x + is, ic.y + is));
    else dl->AddText(ImVec2(ic.x, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), str_eq(line->speaker, str("me")) ? ICON_MIC : ICON_APP_WINDOW);
    Str name = fit_text(app_name(line->speaker), TL_APP_W * u - is - 8 * u);
    dl->AddText(ImVec2(L->x_app + is + 8 * u, ty), ImGui::GetColorU32(ImGuiCol_Text), name.ptr, name.ptr + name.len);

    // The text: being heard, just arrived (writing itself out), or with the search marked.
    ImVec2 tp = ImVec2(L->x_text, ty);
    Span spans[3];
    f64 reveal = -1;
    for (i32 i = 0; i < s->fresh.count; i++) {
        if (s->fresh.items[i].id != line->id) continue;
        f64 secs = utf8_count(text) / 30.0;
        f64 most = line->end_ts - line->start_ts;
        most = most < 0.6 ? 0.6 : most > 4 ? 4 : most;
        secs = secs < 0.6 ? 0.6 : secs > most ? most : secs;
        reveal = (now_ts() - s->fresh.items[i].at) / secs;
    }
    if (hearing) {
        f64 shown = (f->play_ts - line->start_ts) / (line->end_ts - line->start_ts > 0.3 ? line->end_ts - line->start_ts : 0.3);
        i32 n = progress_spans(text, shown, ImGui::GetColorU32(ImGuiCol_Text), ImGui::GetColorU32(ImGuiCol_TextDisabled), true, spans);
        draw_spans(dl, tp, text, L->text_w, spans, n);
    } else if (reveal >= 0 && reveal < 1) {
        f->animating = true;
        i32 n = progress_spans(text, reveal, ImGui::GetColorU32(ImGuiCol_Text), 0, false, spans);
        draw_spans(dl, tp, text, L->text_w, spans, n);
    } else {
        Span* found;
        i32 n = found_spans(text, f->term, &found);
        draw_spans(dl, tp, text, L->text_w, found, n);
    }
    ImGui::PopID();
    ImGui::PopID();
    return h;
}

static f32 tl_file_row(App* app, TimelineState* s, TlRow* r, TlLayout* L, f32 y, TlFrame* f, bool draw) {
    FileItem* file = &s->files.items[r->index];
    f32 u = L->u;
    bool url = str_eq(file->kind, str("url"));
    bool image = str_eq(file->kind, str("image"));
    bool text = str_eq(file->kind, str("text")) && !file->link;
    TlPreview* pv = 0;
    for (i32 i = 0; i < g_tl_previews.count; i++)
        if (g_tl_previews[i].id == file->id) pv = &g_tl_previews[i];
    if (draw && (image || text)) pv = tl_preview(app, file);
    f32 header = ImGui::GetFrameHeight();
    f32 h = 6 * u + header;
    if (file->link && !url) h += L->small_lh;
    ImVec2 img = ImVec2(0, 0);
    Str preview = {};
    if (pv && pv->done && image && pv->view) {
        f32 iw = pv->w * u, ih = pv->h * u;
        f32 scale = 480 * u / iw;
        if (320 * u / ih < scale) scale = 320 * u / ih;
        if (scale > 1) scale = 1;
        img = ImVec2(iw * scale, ih * scale);
        h += img.y;
    } else if (pv && pv->done && pv->text) {
        Str all = str(pv->text);
        preview = str_trim(str_sub(all, 0, utf8_offset(all, 600)));
        h += wrap_count(preview, L->file_w) * L->lh;
    }
    h += 6 * u;
    if (!draw) return h;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    f32 top = y + 6 * u;
    const char* clock = clock_text(file->ts);
    f32 cw = ImGui::CalcTextSize(clock).x;
    dl->AddText(ImVec2(L->x_time + (58 * u - cw) / 2, top + (header - L->lh) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), clock);
    ImGui::PushID((int)file->id);
    ImGui::PushID("file");
    ImGui::SetCursorScreenPos(ImVec2(L->x_file, top));
    const char* glyph = image                                 ? ICON_IMAGE
                        : str_eq(file->kind, str("video"))    ? ICON_VIDEO
                        : str_eq(file->kind, str("text"))     ? ICON_FILE_TEXT
                        : str_eq(file->kind, str("folder"))   ? ICON_FOLDER
                        : url                                 ? ICON_GLOBE
                                                              : ICON_FILE;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ACCENT, "%s", glyph);
    ImGui::SameLine();
    Str name = file->name;
    if (url) {
        i64 at = str_find(name, str("://"));
        if (at >= 0) name = str_sub(name, at + 3);
        while (name.len && name.ptr[name.len - 1] == '/') name.len--;
    }
    if (utf8_count(name) > 70) name = str_fmt(scratch(), "%.*s…", (int)utf8_offset(name, 70), name.ptr);
    ImGui::TextUnformatted(name.ptr, name.ptr + name.len);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", file->link ? file->path.ptr : file->name.ptr);
    }
    if (ImGui::IsItemClicked()) f->open_file = file->id;
    if (file->size > 0) {
        ImGui::SameLine();
        small_begin();
        ImGui::TextDisabled("%s", size_text(file->size));
        small_end();
    }
    if (file->link && !url) {
        ImGui::SameLine();
        small_begin();
        ImGui::TextDisabled("%s", ICON_LINK);
        small_end();
        ImGui::SetItemTooltip("A link to the original; no copy kept");
    }
    ImGui::SameLine();
    if (icon_button(ICON_EXTERNAL_LINK, url ? "Open in the browser" : "Open", "open")) f->open_file = file->id;
    if (!url) {
        ImGui::SameLine(0, 2 * u);
        if (icon_button(ICON_FOLDER_OPEN, "Show in folder", "folder")) {
            f->open_file = file->id;
            f->open_in_folder = true;
        }
    }
    if (file->link) {
        ImGui::SameLine(0, 2 * u);
        if (icon_button(ICON_COPY, "Copy the path", "path")) f->copy_path_file = file->id;
    }
    f32 below = top + header;
    if (file->link && !url) {
        small_begin();
        dl->AddText(ImVec2(L->x_file, below), ImGui::GetColorU32(ImGuiCol_TextDisabled), file->path.ptr, file->path.ptr + file->path.len);
        small_end();
        below += L->small_lh;
    }
    if (img.y > 0) {
        ImGui::SetCursorScreenPos(ImVec2(L->x_file, below));
        ImGui::Image(ImTextureRef((ImTextureID)pv->view), img);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) f->open_file = file->id;
    } else if (preview.len) {
        draw_wrapped(dl, ImVec2(L->x_file, below), preview, L->file_w, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }
    ImGui::PopID();
    ImGui::PopID();
    return h;
}

static f32 tl_row(App* app, TimelineState* s, TlRow* r, TlLayout* L, f32 y, TlFrame* f, bool draw) {
    switch (r->kind) {
    case TL_ROW_GAP: return tl_gap_row(s, r, L, y, draw);
    case TL_ROW_LINE: return tl_line_row(app, s, r, L, y, f, draw);
    case TL_ROW_FILE: return tl_file_row(app, s, r, L, y, f, draw);
    }
    return 0;
}

static void tl_measure(App* app, TimelineState* s, TlRow* r, TlLayout* L, TlFrame* f) {
    r->h = tl_row(app, s, r, L, 0, f, false);
    r->width = L->text_w;
}

// Buttons over the list's bottom-right corner: stop and find what is playing, and jump to now.
static void tl_float_buttons(App* app, TimelineState* s, bool at_bottom, i64 follow) {
    bool playing = app->clips.playing_line != 0;
    if (!playing && at_bottom) return;
    f32 u = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowPos(ImVec2(s->list_max.x - 16 * u, s->list_max.y - 16 * u), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * u);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * u, 4 * u));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
    ImGui::Begin("##tl_float", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    bool first = true;
    if (playing) {
        if (ImGui::Button(label(ICON_SQUARE, "Stop"))) play_stop(app);
        ImGui::SetItemTooltip("Stop playing (Esc)");
        ImGui::SameLine();
        if (ImGui::Button(label(ICON_LOCATE, "Playing"))) {
            f64 ts = app->clips.play_ts;
            if (ts > 0) {
                char day[11];
                day_of(ts, day);
                if (follow && strcmp(day, s->day) == 0 && tl_line_by_id(s, follow)) s->scroll_to = follow;
                else tl_go(app, s, ts, follow, {});
            }
        }
        ImGui::SetItemTooltip("Show what is playing");
        first = false;
    }
    if (!at_bottom) {
        if (!first) ImGui::SameLine();
        if (ImGui::Button(label(ICON_ARROW_DOWN, "Now"))) {
            if (!tl_is_today(s)) {
                char today[11];
                today_day(today);
                tl_open_day(app, s, today);
            }
            s->to_bottom = true;
        }
        ImGui::SetItemTooltip("Jump to now");
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

static void tl_list(App* app, TimelineState* s, f32* next_frame_ms) {
    f32 u = ImGui::GetStyle().FontScaleDpi;
    bool live = tl_is_today(s);
    TlFrame f = {};
    f.term = str_lower(scratch(), str_trim(str(s->search)));

    // What is being heard, given how playback was started; the newest of it is followed.
    i64 follow = 0;
    if (app->clips.playing_line && app->clips.play_ts > 0) {
        f.play_ts = app->clips.play_ts;
        Line* from = tl_line_by_id(s, app->clips.playing_line);
        f.heard = PUSH_ARRAY(scratch(), i64, 64);
        for (i32 i = 0; i < s->lines.count && f.heard_count < 64; i++) {
            Line* l = &s->lines.items[i];
            f64 end = l->end_ts > l->start_ts + 0.5 ? l->end_ts : l->start_ts + 0.5;
            if (!(l->start_ts <= f.play_ts && f.play_ts < end)) continue;
            PlayMode m = app->clips.play_mode;
            if (m == PLAY_LINE && l->id != app->clips.playing_line) continue;
            if (m == PLAY_STREAM_ON && !(from && str_eq(from->speaker, l->speaker))) continue;
            f.heard[f.heard_count++] = l->id;
        }
        if (f.heard_count) follow = f.heard[f.heard_count - 1];
    }
    // Hovering a play button: mark what pressing it would play, following Shift and Ctrl.
    if (s->hover_play && s->hover_play != app->clips.playing_line) {
        f.preview_from = tl_line_by_id(s, s->hover_play);
        f.preview_mode = tl_mode(ImGui::GetIO().KeyShift, ImGui::GetIO().KeyCtrl);
    }
    f.has_stretch = tl_picked(s, &f.st_from, &f.st_to, &f.st_count);

    // Each page its own scroll: today starts at the bottom and keeps up; other days at the top.
    if (s->new_page && !live) ImGui::SetNextWindowScroll(ImVec2(0, 0));
    // No padding: rows are placed by hand. Popped at once so menus keep the usual style.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##tl_list", ImVec2(0, 0), 0, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();
    s->list_min = ImGui::GetWindowPos();
    s->list_max = ImVec2(s->list_min.x + ImGui::GetWindowSize().x, s->list_min.y + ImGui::GetWindowSize().y);
    f32 view_y = ImGui::GetScrollY(), view_h = ImGui::GetWindowHeight();
    bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4 * u;
    bool near_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 60 * u;
    bool stick = live && (at_end || s->new_page || s->to_bottom);
    ImVec2 origin = ImGui::GetCursorScreenPos();  // the content's top left (scrolled)
    f32 avail = ImGui::GetContentRegionAvail().x;
    f32 cw = (TL_COLUMN + 24) * u < avail ? (TL_COLUMN + 24) * u : avail;

    TlLayout L = {};
    L.u = u;
    L.left = origin.x + (avail - cw) / 2;
    L.right = L.left + cw;
    L.x_play = L.left + 8 * u;
    L.x_time = L.x_play + 24 * u + 8 * u;
    L.x_app = L.x_time + 58 * u + 8 * u;
    L.x_text = L.x_app + TL_APP_W * u + 8 * u;
    L.text_w = L.right - 12 * u - L.x_text;
    if (L.text_w < 60 * u) L.text_w = 60 * u;
    L.x_file = L.x_time + 58 * u + 8 * u;
    L.file_w = L.right - 12 * u - L.x_file;
    if (L.file_w < 60 * u) L.file_w = 60 * u;
    L.lh = ImGui::GetTextLineHeight();
    small_begin();
    L.small_lh = ImGui::GetTextLineHeight();
    small_end();

    // Heights: rows measured at another width (or never) are measured now, a few thousand a frame;
    // all of them when a row is to be scrolled to, so it lands where it should.
    i32 budget = s->scroll_to ? INT32_MAX : 4000;
    bool more = false;
    for (i32 i = s->rows.count - 1; i >= 0; i--) {
        TlRow* r = &s->rows.items[i];
        if (r->width == L.text_w && r->kind != TL_ROW_FILE) continue;
        if (budget-- <= 0) {
            more = true;
            break;
        }
        tl_measure(app, s, r, &L, &f);
    }
    if (more) *next_frame_ms = 0;

    f32 y = 8 * u;
    if (!s->rows.count) {
        y += 40 * u;
        const char* t = !app->storage.db ? "The database could not be opened. The log says why."
                        : live           ? "Nothing heard yet today"
                                         : "Nothing heard this day";
        f32 w = ImGui::CalcTextSize(t).x;
        ImGui::GetWindowDrawList()->AddText(ImVec2((L.left + L.right - w) / 2, origin.y + y), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
        y += L.lh;
    }
    f32 margin = 300 * u;
    for (i32 i = 0; i < s->rows.count; i++) {
        TlRow* r = &s->rows.items[i];
        if (r->h <= 0) r->h = r->kind == TL_ROW_GAP ? 24 * u : r->kind == TL_ROW_LINE ? 28 * u : 60 * u;
        bool target = r->kind == TL_ROW_LINE && r->key == s->scroll_to;
        if (y + r->h >= view_y - margin && y <= view_y + view_h + margin) {
            if (r->width != L.text_w) tl_measure(app, s, r, &L, &f);
            r->h = tl_row(app, s, r, &L, origin.y + y, &f, true);
        }
        if (target) {
            ImGui::SetScrollY(y + r->h / 2 - view_h / 2);
            s->scroll_to = 0;
            stick = false;
        }
        y += r->h;
    }
    s->scroll_to = 0;  // not among the rows (its app is hidden): don't keep looking
    if (live && app->storage.db) {
        // Where new lines turn up: what the recorder is doing right now.
        y += 10 * u;
        f32 rh = 22 * u;
        f32 ty = origin.y + y + (rh - L.lh) / 2;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 weak = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (app->capture.paused) {
            dl->AddText(ImVec2(L.x_time, ty), weak, "Paused");
        } else {
            f32 pulse = (f32)sin(ImGui::GetTime() * 2) * 0.5f + 0.5f;
            dl->AddCircleFilled(ImVec2(L.x_time + 7 * u, origin.y + y + rh / 2), 4 * u, accent_u32(0.35f + 0.65f * pulse));
            i32 queued = app->worker.queued;
            const char* t = queued > 0 ? tmp("Listening · writing up %d more…", queued) : "Listening";
            dl->AddText(ImVec2(L.x_time + 14 * u + 8 * u, ty), weak, t);
            bool on_screen = y <= view_y + view_h && y + rh >= view_y;
            if (on_screen && (*next_frame_ms < 0 || *next_frame_ms > 60)) *next_frame_ms = 60;
        }
        y += rh;
    }
    y += 16 * u;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + y));
    ImGui::Dummy(ImVec2(1, 0));
    if (stick) ImGui::SetScrollY(y);
    ImGui::EndChild();
    s->new_page = false;
    s->to_bottom = false;

    tl_float_buttons(app, s, live && near_end, follow);

    s->hover_play = f.hover_play;
    // Shift and Ctrl change what would play, and the word being heard moves: keep drawing.
    if (f.hover_play || app->clips.playing_line) {
        if (*next_frame_ms < 0 || *next_frame_ms > 50) *next_frame_ms = 50;
    }
    if (f.animating && (*next_frame_ms < 0 || *next_frame_ms > 30)) *next_frame_ms = 30;

    if (f.clicked) {
        f64 ts = f.clicked_ts;
        if (s->picked && f.clicked_shift) s->pick_b = ts;
        else if (s->picked && s->pick_a == ts && s->pick_b == ts) s->picked = false;
        else {
            s->picked = true;
            s->pick_a = s->pick_b = ts;
        }
    }
    switch (f.act) {
    case TL_ACT_NONE: break;
    case TL_ACT_COPY_PICKED: tl_copy_picked(app, s, f.act_clip); break;
    case TL_ACT_LINE_TEXT: tl_copy_text(app, str_trim(s->lines.items[f.act_line].text), "Copied the line"); break;
    case TL_ACT_LINE_AUDIO: tl_copy_line_audio(app, s, &s->lines.items[f.act_line]); break;
    case TL_ACT_UNPICK: s->picked = false; break;
    }
    if (f.play_line) tl_play(app, &s->lines.items[f.play_line - 1], f.play_mode);
    if (f.open_file) file_open(app, f.open_file, f.open_in_folder);
    if (f.copy_path_file) {
        for (i32 i = 0; i < s->files.count; i++)
            if (s->files.items[i].id == f.copy_path_file) tl_copy_text(app, s->files.items[i].path, "Copied the path");
    }
}

// ---------------------------------------------------------------------------------------------
// Keys and the frame

static void tl_keys(App* app, TimelineState* s) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) s->focus_search = true;
    // Not while typing, or while a menu is open.
    if (io.WantTextInput || ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(0, ImGuiPopupFlags_AnyPopupId)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (s->calendar_open) s->calendar_open = false;
        else {
            s->picked = false;
            play_stop(app);
        }
    }
    // Left and right: the day before and after.
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && !io.KeyCtrl) tl_step_day(app, s, -1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) && !io.KeyCtrl) tl_step_day(app, s, 1);
    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        tl_copy_picked(app, s, CLIP_FILE);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        Line* line = 0;
        for (i32 i = 0; s->picked && i < s->lines.count && !line; i++)
            if (s->lines.items[i].start_ts == s->pick_a && s->lines.items[i].audio) line = &s->lines.items[i];
        if (line) tl_play(app, line, tl_mode(io.KeyShift, io.KeyCtrl));
        else play_stop(app);
    }
}

static void timeline_draw(App* app, TimelineState* s, f32* next_frame_ms) {
    if (!s->loaded) {
        char today[11];
        today_day(today);
        tl_open_day(app, s, today);
    }
    if (s->lines_version != app->worker.lines_version || s->files_version != app->storage.files_version) timeline_load(app, s);
    if (s->filter_sig != tl_filter_sig(app)) s->rows_dirty = true;
    if (s->rows_dirty) tl_build_rows(app, s);
    // Lines that arrived are written out for a few seconds, then drawn as usual.
    f64 now = now_ts();
    for (i32 i = s->fresh.count - 1; i >= 0; i--)
        if (now - s->fresh.items[i].at > 8) list_remove(&s->fresh, i);

    tl_keys(app, s);
    if (s->rows_dirty) tl_build_rows(app, s);  // the keys may have opened another day

    // The search bar and the Show chips, in a centred column.
    f32 u = ImGui::GetStyle().FontScaleDpi;
    // The list's column (tl_list), less its scrollbar, which is always there so nothing jumps.
    f32 avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    f32 cw = (TL_COLUMN + 24) * u < avail ? (TL_COLUMN + 24) * u : avail;
    f32 list_left = ImGui::GetCursorScreenPos().x + (avail - cw) / 2;
    f32 left = list_left + 14 * u;      // the play buttons' glyphs
    f32 right = list_left + cw - 12 * u;  // the end of the text
    tl_search_bar(app, s, left, right, next_frame_ms);
    ImGui::Dummy(ImVec2(0, 4 * u));
    if (s->rows_dirty) tl_build_rows(app, s);
    tl_show_chips(app, s, left, right);
    ImGui::Dummy(ImVec2(0, 4 * u));
    if (s->filter_sig != tl_filter_sig(app) || s->rows_dirty) tl_build_rows(app, s);

    tl_list(app, s, next_frame_ms);
    tl_calendar(app, s);
}

static void timeline_free(TimelineState* s) {
    arena_release(&s->arena);
    arena_release(&s->search_arena);
    arena_release(&s->days_arena);
    arena_release(&s->apps_arena);
    list_free(&s->rows);
    list_free(&s->fresh);
    // The previews go with the window (it is called with the lock held).
    for (i32 i = 0; i < g_tl_previews.count; i++) {
        TlPreview* p = &g_tl_previews[i];
        if (p->view) p->view->Release();
        free(p->rgba);
        free(p->text);
    }
    list_free(&g_tl_previews);
}
