// clip.cpp: copying a stretch of what was heard (see clip.h), and the reading and mixing of kept
// audio that playback (player.cpp) shares. The worker's clips are 16 kHz mono 16-bit WAV with a
// plain 44-byte header, under data_dir\audio\YYYY-MM-DD\.

static const f64 AUDIO_RATE = 16000.0;
// More than a very long day; keeps a mistaken huge range from freezing the window.
static const i32 CLIP_MAX_LINES = 20000;
static const f64 MIX_MAX_SECS = 3.0 * 3600.0;

// ---------------------------------------------------------------------------------------------
// Ranges

bool clip_range(Str range, f64 now, f64* from, char* label, i32 label_cap) {
    Mark m = mark(scratch());
    Str r = str_lower(scratch(), str_trim(range));
    bool ok = false;
    if (str_eq(r, str("today"))) {
        LocalTime t = local_time(now);
        *from = local_ts(t.year, t.month, t.day, 0, 0);
        ok = true;
    } else if (r.len >= 2 && r.len <= 10) {
        i64 unit = 0;
        switch (r.ptr[r.len - 1]) {
        case 's': unit = 1; break;
        case 'm': unit = 60; break;
        case 'h': unit = 3600; break;
        case 'd': unit = 86400; break;
        }
        i64 n = 0;
        bool digits = unit != 0;
        for (i64 i = 0; i < r.len - 1 && digits; i++) {
            if (r.ptr[i] < '0' || r.ptr[i] > '9') digits = false;
            else n = n * 10 + (r.ptr[i] - '0');
        }
        if (digits && n > 0) {
            *from = now - (f64)(n * unit);
            ok = true;
        }
    }
    if (ok && label) {
        if (str_eq(r, str("today"))) set(label, label_cap, r);
        else set(label, label_cap, str_fmt(scratch(), "last-%s", r.ptr));
    }
    rewind(m);
    return ok;
}


// A worker clip's path under the audio folder. False if it would leave the folder (the worker
// writes these, but a path from the database is never followed out of it).
static bool audio_file_path(const wchar_t* audio_dir, Str rel, wchar_t out[MAX_PATH]) {
    if (rel.len == 0 || str_find(rel, str("..")) >= 0 || str_find(rel, str(":")) >= 0) return false;
    Mark m = mark(scratch());
    wchar_t* w = wide(scratch(), rel);
    for (wchar_t* c = w; *c; c++)
        if (*c == L'/') *c = L'\\';
    swprintf(out, MAX_PATH, L"%ls\\%ls", audio_dir, w);
    rewind(m);
    return true;
}

static bool is_worker_wav(Str wav) {
    return wav.len >= WAV_HEADER && memcmp(wav.ptr, "RIFF", 4) == 0 && memcmp(wav.ptr + 36, "data", 4) == 0;
}

// ---------------------------------------------------------------------------------------------
// Mixing

// Where a mix's time jumps: seconds into the audio, and the recording time there. One per stretch
// played without a cut; in between, time runs on as normal.
struct TimeJump {
    f64 at;
    f64 ts;
};

// Samples are on the heap (mix_free); a zeroed Mix is empty.
struct Mix {
    i16* samples;
    i64 count;
    List<TimeJump> jumps;
};

static void mix_free(Mix* m) {
    free(m->samples);
    list_free(&m->jumps);
    *m = {};
}

// Whose audio goes into a mix: just `only` when set, else everyone not in `hidden`.
struct Keep {
    Names* hidden;
    Str only;
};

static bool keeps(Keep* k, Str speaker) {
    if (k->only.len) return str_eq(speaker, k->only);
    return !k->hidden || !names_has(k->hidden, speaker);
}

struct MixPart {
    i64 start, end;  // positions (samples since 1970, as recordings.start)
    const i16* samples;
    i64 at;          // where it goes in the mix, in samples
};

static int mix_part_order(const void* a, const void* b) {
    i64 x = ((const MixPart*)a)->start, y = ((const MixPart*)b)->start;
    return x < y ? -1 : x > y ? 1 : 0;
}

// The longest a recording can be (capture's hard limit is 30 s): how far before `from` to look.
static const i64 RECORDING_MAX = 120 * 16000;

// The kept audio of [from, to) from the speakers `keep` accepts, all mixed together, with
// silences longer than a second shortened to one. `out` is empty if nothing was kept. False (with
// `error`) if the database failed or the mix would pass 3 hours.
static bool mix_audio(sqlite3* db, const wchar_t* audio_dir, f64 from, f64 to, Keep* keep, Mix* out, char* error, i32 cap) {
    *out = {};
    i64 lo = (i64)floor(from * AUDIO_RATE), hi = (i64)ceil(to * AUDIO_RATE);
    sqlite3_stmt* st = prepare(db, "SELECT file, start, length, speaker FROM recordings "
                                   "WHERE start > ?1 - ?3 AND start < ?2 AND start + length > ?1 AND sample_rate = 16000");
    if (!st) {
        snprintf(error, cap, "could not read the database");
        return false;
    }
    sqlite3_bind_int64(st, 1, lo);
    sqlite3_bind_int64(st, 2, hi);
    sqlite3_bind_int64(st, 3, RECORDING_MAX);

    // The clips' bytes live in their own arena, given back at the end (a long mix reads a lot).
    Arena files = {};
    List<MixPart> parts = {};
    while (sqlite3_step(st) == SQLITE_ROW) {
        Str file = col_str(st, 0, &files);
        i64 start = sqlite3_column_int64(st, 1);
        Str speaker = col_str(st, 3, &files);
        wchar_t path[MAX_PATH];
        if (!keeps(keep, speaker) || !audio_file_path(audio_dir, file, path)) continue;
        Str wav = read_file(&files, path);
        if (!wav.ptr || !is_worker_wav(wav)) continue;  // deleted after capture.keep_audio_days
        const i16* pcm = (const i16*)(wav.ptr + WAV_HEADER);
        i64 n = (wav.len - WAV_HEADER) / 2;
        i64 length = sqlite3_column_int64(st, 2);
        if (n > length) n = length;
        i64 a = start > lo ? start : lo, b = start + n < hi ? start + n : hi;
        if (a >= b) continue;
        MixPart p = {a, b, pcm + (a - start), 0};
        list_push(&parts, p);
    }
    sqlite3_finalize(st);

    bool ok = true;
    if (parts.count) {
        qsort(parts.items, parts.count, sizeof(MixPart), mix_part_order);
        // Lay the parts on a timeline, closing gaps down to one second.
        const i64 second = (i64)AUDIO_RATE;
        i64 shift = parts[0].start, last_end = parts[0].start;
        list_push(&out->jumps, {0.0, (f64)parts[0].start / AUDIO_RATE});
        i64 total = 0;
        for (i32 i = 0; i < parts.count; i++) {
            MixPart* p = &parts.items[i];
            if (p->start > last_end + second) {
                shift += p->start - last_end - second;
                list_push(&out->jumps, {(f64)(p->start - shift) / AUDIO_RATE, (f64)p->start / AUDIO_RATE});
            }
            p->at = p->start - shift;
            if (p->at + (p->end - p->start) > total) total = p->at + (p->end - p->start);
            if (p->end > last_end) last_end = p->end;
        }
        if ((f64)total / AUDIO_RATE > MIX_MAX_SECS) {
            snprintf(error, cap, "that is more than 3 hours of audio; pick a shorter range");
            list_free(&out->jumps);
            ok = false;
        } else {
            out->samples = (i16*)calloc((size_t)total, sizeof(i16));
            CHECK(out->samples, "memory for %lld samples of audio", total);
            out->count = total;
            for (i32 i = 0; i < parts.count; i++) {
                MixPart* p = &parts.items[i];
                i16* o = out->samples + p->at;
                for (i64 j = 0; j < p->end - p->start; j++) {
                    i32 s = (i32)o[j] + (i32)p->samples[j];
                    o[j] = (i16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
                }
            }
        }
    }
    list_free(&parts);
    arena_release(&files);
    return ok;
}

// For playing on and on: up to `secs` of kept audio from `from` (skipping ahead over stretches
// where nothing was kept), and where the next piece starts. False when there is nothing further
// (with `error` empty) or the mix failed (with `error` set).
static bool mix_from(sqlite3* db, const wchar_t* audio_dir, f64 from, f64 secs, Keep* keep, Mix* out, f64* next,
                     char* error, i32 cap) {
    error[0] = 0;
    for (i32 tries = 0; tries < 20; tries++) {
        if (!mix_audio(db, audio_dir, from, from + secs, keep, out, error, cap)) return false;
        if (out->count) {
            *next = from + secs;
            return true;
        }
        mix_free(out);
        // Nothing here: jump to the next recording from the speakers wanted.
        sqlite3_stmt* st = prepare(db, "SELECT start, speaker FROM recordings WHERE start >= ?1 AND sample_rate = 16000 "
                                       "ORDER BY start LIMIT 2000");
        if (!st) {
            snprintf(error, cap, "could not read the database");
            return false;
        }
        sqlite3_bind_int64(st, 1, (i64)ceil((from + secs) * AUDIO_RATE));
        bool found = false;
        Mark m = mark(scratch());
        while (!found && sqlite3_step(st) == SQLITE_ROW) {
            Str speaker = col_str(st, 1, scratch());
            if (keeps(keep, speaker)) {
                from = (f64)sqlite3_column_int64(st, 0) / AUDIO_RATE;
                found = true;
            }
        }
        rewind(m);
        sqlite3_finalize(st);
        if (!found) return false;
    }
    return false;
}

// One line's audio, with a little room either side so words are not clipped.
static bool line_audio(sqlite3* db, const wchar_t* audio_dir, i64 id, Mix* out, char* error, i32 cap) {
    *out = {};
    sqlite3_stmt* st = prepare(db, "SELECT r.file, r.start, r.length, r.sample_rate, s.start_sample, s.end_sample "
                                   "FROM segments s JOIN recordings r ON r.id = s.recording WHERE s.id = ?1");
    if (!st) {
        snprintf(error, cap, "could not read the database");
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    Mark m = mark(scratch());
    bool ok = false;
    if (sqlite3_step(st) != SQLITE_ROW) {
        snprintf(error, cap, "no audio was kept for this line");
    } else if (sqlite3_column_int(st, 3) != 16000) {
        snprintf(error, cap, "unexpected audio file");
    } else {
        Str file = col_str(st, 0, scratch());
        i64 start = sqlite3_column_int64(st, 1);
        i64 length = sqlite3_column_int64(st, 2);
        i64 from = sqlite3_column_int64(st, 4), to = sqlite3_column_int64(st, 5);
        wchar_t path[MAX_PATH];
        Str wav = {};
        if (!audio_file_path(audio_dir, file, path)) snprintf(error, cap, "bad audio path");
        else if (!(wav = read_file(scratch(), path)).ptr) snprintf(error, cap, "the audio was deleted (audio is kept for capture.keep_audio_days days, 7 by default)");
        else if (!is_worker_wav(wav)) snprintf(error, cap, "unexpected audio file");
        else {
            i64 n = (wav.len - WAV_HEADER) / 2;
            if (n > length) n = length;
            const i64 before = (i64)(0.3 * AUDIO_RATE), after = (i64)(0.4 * AUDIO_RATE);
            i64 a = from - before > 0 ? from - before : 0;
            i64 b = to + after < n ? to + after : n;
            if (a > n) a = n;
            if (b < a) b = a;
            out->count = b - a;
            out->samples = (i16*)malloc((size_t)(out->count ? out->count : 1) * sizeof(i16));
            CHECK(out->samples, "memory for %lld samples of audio", out->count);
            if (out->count) memcpy(out->samples, wav.ptr + WAV_HEADER + a * 2, (size_t)out->count * 2);
            list_push(&out->jumps, {0.0, (f64)(start + a) / AUDIO_RATE});
            ok = true;
        }
    }
    rewind(m);
    sqlite3_finalize(st);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Files and the clipboard

// 16 kHz mono 16-bit samples as a WAV file.
static bool wav_write(const wchar_t* path, const i16* samples, i64 count) {
    u32 data = (u32)(count * 2);
    u8 h[WAV_HEADER];
    wav_header(h, 16000, data);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0, wrote2 = 0;
    BOOL ok = WriteFile(f, h, WAV_HEADER, &wrote, 0) && WriteFile(f, samples, data, &wrote2, 0);
    CloseHandle(f);
    return ok && wrote == WAV_HEADER && wrote2 == data;
}

static void temp_path(Str name, wchar_t out[MAX_PATH]) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);  // ends with a backslash
    Mark m = mark(scratch());
    swprintf(out, MAX_PATH, L"%ls%ls", dir, wide(scratch(), name));
    rewind(m);
}

// Opens the clipboard with a window of our own (SetClipboardData wants an owner). Another program
// may hold it for a moment, so it tries for half a second. Close with clipboard_close.
static bool clipboard_open(HWND* owner) {
    *owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, GetModuleHandleW(0), 0);
    for (i32 i = 0; i < 20; i++) {
        if (OpenClipboard(*owner)) return true;
        Sleep(25);
    }
    if (*owner) DestroyWindow(*owner);
    *owner = 0;
    return false;
}

static void clipboard_close(HWND owner) {
    CloseClipboard();
    if (owner) DestroyWindow(owner);
}

// Replaces the clipboard with `mem` as `format`. Takes `mem` either way.
static bool clipboard_put(UINT format, HGLOBAL mem) {
    HWND owner;
    if (!clipboard_open(&owner)) {
        GlobalFree(mem);
        return false;
    }
    bool ok = EmptyClipboard() && SetClipboardData(format, mem);
    clipboard_close(owner);
    if (!ok) GlobalFree(mem);  // still ours if it was not taken
    return ok;
}

// Puts a file on the clipboard (CF_HDROP), like copying it in Explorer.
static bool clipboard_set_file(const wchar_t* path) {
    size_t chars = wcslen(path);
    // DROPFILES, then the path, then a double NUL ending the list.
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (chars + 2) * sizeof(wchar_t));
    if (!mem) return false;
    u8* p = (u8*)GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return false;
    }
    DROPFILES* df = (DROPFILES*)p;
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(p + sizeof(DROPFILES), path, chars * sizeof(wchar_t));
    GlobalUnlock(mem);
    return clipboard_put(CF_HDROP, mem);
}

bool clipboard_set_text(Str text) {
    Mark m = mark(scratch());
    wchar_t* w = wide(scratch(), text);
    size_t chars = wcslen(w);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (chars + 1) * sizeof(wchar_t));
    bool ok = false;
    if (mem) {
        void* p = GlobalLock(mem);
        if (p) {
            memcpy(p, w, (chars + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            ok = clipboard_put(CF_UNICODETEXT, mem);
        } else {
            GlobalFree(mem);
        }
    }
    rewind(m);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Copying

// The text of a clip: a header, then one line per transcribed line.
static void clip_format(Buf* t, Slice<Line> lines, f64 from, f64 to) {
    LocalTime a = local_time(from), b = local_time(to);
    buf_fmt(t, "lifelog — %04d-%02d-%02d %02d:%02d to %04d-%02d-%02d %02d:%02d\n\n", a.year, a.month, a.day, a.hour,
             a.minute, b.year, b.month, b.day, b.hour, b.minute);
    bool many_days = a.year != b.year || a.month != b.month || a.day != b.day;
    for (i32 i = 0; i < lines.count; i++) {
        Line* l = &lines.items[i];
        LocalTime t0 = local_time(l->start_ts);
        char stamp[32];
        if (many_days) snprintf(stamp, sizeof(stamp), "%02d-%02d %02d:%02d:%02d", t0.month, t0.day, t0.hour, t0.minute, t0.second);
        else snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", t0.hour, t0.minute, t0.second);
        Str text = str_trim(l->text);
        if (l->source != SOURCE_MIC && l->title.len)
            buf_fmt(t, "[%s] %.*s (%.*s): %.*s\n", stamp, (int)l->speaker.len, l->speaker.ptr, (int)l->title.len,
                     l->title.ptr, (int)text.len, text.ptr);
        else
            buf_fmt(t, "[%s] %.*s: %.*s\n", stamp, (int)l->speaker.len, l->speaker.ptr, (int)text.len, text.ptr);
    }
}

ClipResult clip_copy(AppLabels* labels, f64 from, f64 to, Names* exclude, ClipKind kind, const char* label) {
    ClipResult r = {};
    if (!(isfinite(from) && isfinite(to) && from < to)) {
        snprintf(r.error, sizeof(r.error), "the range must end after it starts");
        return r;
    }
    sqlite3* db = db_open_path(true);
    if (!db) {
        snprintf(r.error, sizeof(r.error), "could not open the database");
        return r;
    }
    Mark m = mark(scratch());
    LocalTime a = local_time(from), b = local_time(to);
    Str times = str_fmt(scratch(), "%04d-%02d-%02d-%02d%02d-to-%02d%02d", a.year, a.month, a.day, a.hour, a.minute, b.hour, b.minute);
    wchar_t path[MAX_PATH] = {};

    if (kind == CLIP_AUDIO) {
        wchar_t adir[MAX_PATH];
        audio_dir(adir);
        Keep keep = {exclude, {}};
        Mix mix = {};
        if (mix_audio(db, adir, from, to, &keep, &mix, r.error, sizeof(r.error))) {
            if (!mix.count) {
                snprintf(r.error, sizeof(r.error), "no audio kept for this range (audio is kept for capture.keep_audio_days days, 7 by default)");
            } else {
                temp_path(str_fmt(scratch(), "lifelog-%s.wav", label ? label : times.ptr), path);
                if (!wav_write(path, mix.samples, mix.count)) snprintf(r.error, sizeof(r.error), "could not write the audio file");
                else if (!clipboard_set_file(path)) snprintf(r.error, sizeof(r.error), "the clipboard is busy");
                else {
                    r.ok = true;
                    r.seconds = (f64)mix.count / AUDIO_RATE;
                }
            }
        }
        mix_free(&mix);
    } else {
        Slice<Line> all = {};
        sqlite3_stmt* st = prepare(db, "SELECT " LINE_COLUMNS " FROM segments s WHERE s.end_ts >= ?1 AND s.start_ts <= ?2 "
                                       "ORDER BY s.start_ts, s.id LIMIT 20000");
        if (st) {
            sqlite3_bind_double(st, 1, from);
            sqlite3_bind_double(st, 2, to);
            all = read_lines(st, scratch(), CLIP_MAX_LINES);
        }
        // Leave out the excluded speakers, keeping the order.
        Slice<Line> lines = {PUSH_ARRAY(scratch(), Line, all.count), 0};
        for (i32 i = 0; i < all.count; i++)
            if (!exclude || !names_has(exclude, all.items[i].speaker)) lines.items[lines.count++] = all.items[i];
        // Each speaker as the app list shows it ("zaddish", "Brave Browser"), not its exe.
        for (i32 i = 0; i < lines.count; i++) lines.items[i].speaker = str_copy(scratch(), label_of(labels, lines.items[i].speaker));
        Buf text = {};
        clip_format(&text, lines, from, to);
        Str body = {text.chars.items, text.chars.count};
        temp_path(str_fmt(scratch(), "lifelog-%s.txt", label ? label : times.ptr), path);
        if (!st) snprintf(r.error, sizeof(r.error), "could not read the database");
        else if (!write_file(path, body)) snprintf(r.error, sizeof(r.error), "could not write the clip file");
        else if (kind == CLIP_TEXT ? !clipboard_set_text(body) : !clipboard_set_file(path))
            snprintf(r.error, sizeof(r.error), "the clipboard is busy");
        else {
            r.ok = true;
            r.lines = lines.count;
        }
        buf_free(&text);
    }
    SET(r.path, narrow(scratch(), path));
    rewind(m);
    sqlite3_close(db);
    if (r.ok) LOG("copied %s to the clipboard", r.path);
    else WARN("copying failed: %s", r.error);
    return r;
}

struct ClipJob {
    AppLabels labels;
    f64 from, to;
    Names exclude;
    ClipKind kind;
    char label[64];
    ClipDone* done;
    i64 ctx;
    ClipResult result;
};

static void clip_job_run(Job* job) {
    ClipJob* c = (ClipJob*)job->data;
    c->result = clip_copy(&c->labels, c->from, c->to, &c->exclude, c->kind, c->label[0] ? c->label : 0);
    list_free(&c->exclude);
}

static void clip_job_apply(App* app, Job* job) {
    ClipJob* c = (ClipJob*)job->data;
    c->done(app, &c->result, c->kind, c->ctx);
}

void clip_copy_later(App* app, f64 from, f64 to, Names exclude, ClipKind kind, const char* label, ClipDone* done, i64 ctx) {
    Job* job = job_new(clip_job_apply);
    ClipJob* c = PUSH(&job->arena, ClipJob);
    c->labels = app_labels(app, &job->arena);
    c->from = from;
    c->to = to;
    c->exclude = exclude;
    c->kind = kind;
    if (label) SET(c->label, str(label));
    c->done = done;
    c->ctx = ctx;
    job->data = c;
    pool_post(job, clip_job_run);
}

// ---------------------------------------------------------------------------------------------
// Deleting old audio

struct PruneJob {
    char cutoff[11];  // folders named before this day go
    i32 folders;
    i32 files;
    i32 failed;
};

static void prune_run(Job* job) {
    PruneJob* p = (PruneJob*)job->data;
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    audio_dir(dir);
    if (_snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", dir) < 0) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(fd.cFileName) != 10) continue;
        char day[11];
        for (i32 i = 0; i < 10; i++) day[i] = fd.cFileName[i] < 128 ? (char)fd.cFileName[i] : '?';
        day[10] = 0;
        i32 y, m, d;
        if (!parse_day(day, &y, &m, &d) || strcmp(day, p->cutoff) >= 0) continue;
        wchar_t folder[MAX_PATH], wavs[MAX_PATH];
        if (_snwprintf_s(folder, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName) < 0) continue;
        if (_snwprintf_s(wavs, MAX_PATH, _TRUNCATE, L"%s\\*.wav", folder) < 0) continue;
        WIN32_FIND_DATAW wd;
        HANDLE w = FindFirstFileW(wavs, &wd);
        if (w != INVALID_HANDLE_VALUE) {
            do {
                if (wd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t path[MAX_PATH];
                if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", folder, wd.cFileName) < 0) continue;
                if (DeleteFileW(path)) p->files++;
                else p->failed++;
            } while (FindNextFileW(w, &wd));
            FindClose(w);
        }
        // Only goes if nothing else is in it.
        if (RemoveDirectoryW(folder)) p->folders++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void prune_apply(App* app, Job* job) {
    (void)app;
    PruneJob* p = (PruneJob*)job->data;
    if (p->files || p->folders)
        LOG("deleted the audio of days before %s: %d files, %d folders", p->cutoff, p->files, p->folders);
    if (p->failed) WARN("%d old audio files could not be deleted", p->failed);
}

void audio_prune(App* app) {
    u32 keep = app->config.capture.keep_audio_days;
    if (!keep) return;
    char today[11];
    i32 y, m, d;
    day_of(now_ts(), today);
    if (!parse_day(today, &y, &m, &d)) return;
    Job* job = job_new(prune_apply);
    PruneJob* p = PUSH(&job->arena, PruneJob);
    // Today and the keep-1 days before it stay.
    day_of(local_ts(y, m, d - (i32)(keep - 1), 12, 0), p->cutoff);
    job->data = p;
    pool_post(job, prune_run);
}
