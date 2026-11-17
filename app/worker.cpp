// worker.cpp: see worker.h. Two threads, neither touches App:
//   - the supervisor decides whether transcription may run, loads and frees the model, counts
//     the queue and transcribes the files, oldest first; it sleeps until woken;
//   - the GPU monitor samples the 3D engine's load every 2 s.

#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")

#include "vendor/whisper/whisper.h"

// ---------------------------------------------------------------------------------------------
// GPU load, from the counters Task Manager uses. Games load the 3D engine; lifelog's own load is
// left out of the sum.

struct Gpu3d {
    PDH_HQUERY query;
    PDH_HCOUNTER counter;
};

static bool gpu3d_open(Gpu3d* g) {
    *g = {};
    if (PdhOpenQueryW(nullptr, 0, &g->query) != ERROR_SUCCESS) return false;
    if (PdhAddEnglishCounterW(g->query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &g->counter) !=
        ERROR_SUCCESS) {
        PdhCloseQuery(g->query);
        *g = {};
        return false;
    }
    PdhCollectQueryData(g->query);  // rates need two samples; this is the first
    return true;
}

// Percent of the 3D engine in use since the last call, summed over every other program (so it can
// pass 100 with more than one GPU). Call at least a second apart. False if there is no reading.
static bool gpu3d_sample(Gpu3d* g, f64* load) {
    if (PdhCollectQueryData(g->query) != ERROR_SUCCESS) return false;
    DWORD size = 0, count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(g->counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if ((u32)status != (u32)PDH_MORE_DATA || size == 0) return false;
    Mark m = mark(scratch());
    // The items come first, the instance names after them in the same buffer.
    PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)arena_push(scratch(), size, 8);
    bool ok = PdhGetFormattedCounterArrayW(g->counter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS;
    // Instances are named "pid_<n>_luid_...". Our own are skipped: Whisper's kernels and the window
    // can show up as 3D work, and counting them made transcription pause itself.
    wchar_t own[32];
    i32 own_len = swprintf(own, 32, L"pid_%lu_", GetCurrentProcessId());
    f64 sum = 0;
    for (DWORD i = 0; ok && i < count; i++) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        if (items[i].szName && wcsncmp(items[i].szName, own, own_len) == 0) continue;
        sum += items[i].FmtValue.doubleValue;
    }
    rewind(m);
    *load = sum;
    return ok;
}

// Busy after ~6 s over the limit, free again once `resume_after_secs` have passed under it (so a
// loading screen or a menu does not start a transcription run mid-game).
static DWORD WINAPI gpu_monitor_thread(void* p) {
    App* app = (App*)p;
    Gpu3d gpu;
    if (!gpu3d_open(&gpu)) {
        WARN("GPU load counters unavailable: transcription will not pause for the GPU load");
        return 0;
    }
    u32 over = 0;
    f64 last_over = now_ts();
    while (sleep_unless_stopping(app, 2000)) {
        f64 load = 0;
        if (!gpu3d_sample(&gpu, &load)) continue;
        // The flags are atomics and the limits come from settings().
        const Config* cfg = settings(app);
        Worker* w = &app->worker;
        bool was_busy = atomic_get(&w->gpu_busy) != 0;
        bool busy = was_busy;
        LONG percent = (LONG)(load + 0.5);
        bool changed = percent != atomic_get(&w->gpu_3d_percent);
        atomic_set(&w->gpu_3d_percent, percent);
        if (atomic_get(&w->summary_needs_gpu)) {
            over = 0;  // the summary model's own load shows up as 3D work too; it is not a game
        } else if (load > (f64)cfg->processing.gpu_busy_percent) {
            over++;
            last_over = now_ts();
            if (over >= 3 && !busy) {
                busy = true;
                LOG("GPU busy (%.0f%% 3D): transcription waits, audio keeps queueing", load);
            }
        } else {
            over = 0;
            if (busy && now_ts() - last_over >= (f64)cfg->processing.resume_after_secs) {
                busy = false;
                LOG("GPU free again: transcription resumes");
            }
        }
        atomic_set(&w->gpu_busy, busy);
        if (was_busy != busy) worker_wake(app);
        changed |= was_busy != busy;
        if (changed) wake_window(app);
    }
    PdhCloseQuery(gpu.query);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// When transcription may run

// The WAV files in the spool folder: how many and how long (16 kHz mono 16-bit after the header),
// and the oldest one's name (names start with the time in ms; empty when there is none). Any
// output may be null.
static void scan_spool(const wchar_t* spool, i32* files, f64* secs, wchar_t oldest[MAX_PATH]) {
    if (files) *files = 0;
    if (secs) *secs = 0;
    if (oldest) oldest[0] = 0;
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.wav", spool);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &f, FindExSearchNameMatch, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        u64 size = ((u64)f.nFileSizeHigh << 32) | f.nFileSizeLow;
        if (files) (*files)++;
        if (secs && size > WAV_HEADER) *secs += (f64)(size - WAV_HEADER) / 32000.0;  // 16 kHz, 16-bit
        if (oldest && (!oldest[0] || wcscmp(f.cFileName, oldest) < 0)) wcscpy_s(oldest, MAX_PATH, f.cFileName);
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

// Whether transcription may run now, and the few words saying why (not). No lock: it reads
// atomics and settings(). Waiting for a game, the state is GAME_STATE and the main thread puts
// the game's name in.
static const char* GAME_STATE = "waiting for a game to close";

static bool worker_allowed(App* app, f64 quiet_from, char* state, i32 cap) {
    Worker* w = &app->worker;
    const Config* c = settings(app);
    Mode mode = c->processing.mode;
    f64 wait = (f64)c->processing.resume_after_secs - (now_ts() - quiet_from);
    if (atomic_get(&w->summary_needs_gpu)) {
        snprintf(state, cap, "waiting for the daily summary");
    } else if (mode == MODE_PAUSED) {
        snprintf(state, cap, "paused");
    } else if (mode == MODE_AUTO && c->processing.pause_for_games && atomic_get(&app->apps.game_on)) {
        snprintf(state, cap, "%s", GAME_STATE);
    } else if (mode == MODE_AUTO && atomic_get(&w->gpu_busy)) {
        snprintf(state, cap, "waiting for the graphics card");
    } else if (mode == MODE_AUTO && quiet_from > 0 && wait > 0) {
        snprintf(state, cap, "the game has closed: starting in %.0f s", wait);
    } else {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// whisper.cpp, in this process
//
// whisper.dll and its ggml DLLs come from the setup (setup.h), so lifelog.exe does not link to
// them: they are loaded the first time they are needed and stay loaded. The model is loaded only
// while transcription may run, and freed (giving the graphics memory back) when it has to wait.
// Only the supervisor thread touches any of this.

#define WHISPER_FN(name) static decltype(&name) p_##name;
WHISPER_FN(whisper_context_default_params)
WHISPER_FN(whisper_init_from_file_with_params)
WHISPER_FN(whisper_free)
WHISPER_FN(whisper_full_default_params)
WHISPER_FN(whisper_full)
WHISPER_FN(whisper_full_n_segments)
WHISPER_FN(whisper_full_get_segment_t0)
WHISPER_FN(whisper_full_get_segment_t1)
WHISPER_FN(whisper_full_get_segment_text)
WHISPER_FN(whisper_full_get_segment_no_speech_prob)
WHISPER_FN(whisper_full_n_tokens)
WHISPER_FN(whisper_full_get_token_data)
WHISPER_FN(whisper_token_eot)
WHISPER_FN(whisper_log_set)
WHISPER_FN(ggml_backend_load_all_from_path)
#undef WHISPER_FN

static bool g_whisper_loaded;
static bool g_whisper_aborting;  // a clip is being cut short: its "failed to decode" is expected

// Its warnings and errors go to our log; the rest (loading details, timings) is dropped.
static void whisper_log(enum ggml_log_level level, const char* text, void*) {
    if (level != GGML_LOG_LEVEL_WARN && level != GGML_LOG_LEVEL_ERROR) return;
    if (g_whisper_aborting) return;
    Str s = str_trim(str(text));
    if (!s.len) return;
    if (level == GGML_LOG_LEVEL_ERROR) ERR("whisper: %.*s", (int)s.len, s.ptr);
    else WARN("whisper: %.*s", (int)s.len, s.ptr);
}

// Loads whisper.dll and the ggml backends (the best CPU one for this processor, and CUDA) from
// the setup's folder. Once per run.
static bool whisper_load(const wchar_t* dir, char* error, i32 cap) {
    if (g_whisper_loaded) return true;
    wchar_t path[MAX_PATH];
    // whisper.dll's own imports (ggml.dll), and the CUDA libraries ggml-cuda.dll needs, are next
    // to it; ggml loads its backends with a plain LoadLibrary, so the folder goes on the search path.
    SetDllDirectoryW(dir);
    swprintf(path, MAX_PATH, L"%ls\\whisper.dll", dir);
    HMODULE whisper = LoadLibraryExW(path, 0, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    swprintf(path, MAX_PATH, L"%ls\\ggml.dll", dir);
    HMODULE ggml = whisper ? GetModuleHandleW(path) : 0;
    if (!whisper || !ggml) {
        snprintf(error, cap, "could not load whisper.dll (error %lu): set it up again in Settings", GetLastError());
        return false;
    }
    bool ok = true;
#define WHISPER_FN(dll, name) \
    ok = ok && (p_##name = (decltype(p_##name))GetProcAddress(dll, #name)) != 0;
    WHISPER_FN(whisper, whisper_context_default_params)
    WHISPER_FN(whisper, whisper_init_from_file_with_params)
    WHISPER_FN(whisper, whisper_free)
    WHISPER_FN(whisper, whisper_full_default_params)
    WHISPER_FN(whisper, whisper_full)
    WHISPER_FN(whisper, whisper_full_n_segments)
    WHISPER_FN(whisper, whisper_full_get_segment_t0)
    WHISPER_FN(whisper, whisper_full_get_segment_t1)
    WHISPER_FN(whisper, whisper_full_get_segment_text)
    WHISPER_FN(whisper, whisper_full_get_segment_no_speech_prob)
    WHISPER_FN(whisper, whisper_full_n_tokens)
    WHISPER_FN(whisper, whisper_full_get_token_data)
    WHISPER_FN(whisper, whisper_token_eot)
    WHISPER_FN(whisper, whisper_log_set)
    WHISPER_FN(ggml, ggml_backend_load_all_from_path)
#undef WHISPER_FN
    if (!ok) {
        snprintf(error, cap, "whisper.dll is not the version lifelog expects: set it up again in Settings");
        return false;
    }
    p_whisper_log_set(whisper_log, 0);  // ggml's log too
    Mark m = mark(scratch());
    p_ggml_backend_load_all_from_path(narrow(scratch(), dir).ptr);
    rewind(m);
    g_whisper_loaded = true;
    whisper_loaded_from(dir);
    return true;
}

// Loads ggml-<model>.bin onto the GPU. Null (with `error`) if it could not.
static whisper_context* whisper_open(const Config* cfg, const char* model, char* error, i32 cap) {
    wchar_t bins[MAX_PATH], bin[MAX_PATH];
    whisper_bin_dir(cfg, bins);
    if (!whisper_load(bins, error, cap)) return 0;
    model_path(cfg, SETUP_TRANSCRIBER, model, bin);
    whisper_context_params cp = p_whisper_context_default_params();
    cp.use_gpu = true;
    cp.flash_attn = true;
    f64 began = now_ts();
    Mark m = mark(scratch());
    whisper_context* ctx = p_whisper_init_from_file_with_params(narrow(scratch(), bin).ptr, cp);
    rewind(m);
    if (!ctx) snprintf(error, cap, "could not load the model ggml-%s.bin", model);
    else LOG("Whisper %s loaded in %.1f s", model, now_ts() - began);
    return ctx;
}

// Asked by whisper.cpp between steps: stop this clip? Yes when lifelog is closing, or when
// transcription has to step aside (a game started, the summary wants the GPU). Looks at most
// every quarter second.
struct AbortCheck {
    App* app;
    f64 next;
    bool aborted;
};

static bool whisper_should_abort(void* p) {
    AbortCheck* c = (AbortCheck*)p;
    if (c->aborted) return true;
    if (stopping(c->app)) return c->aborted = g_whisper_aborting = true;
    f64 now = now_ts();
    if (now < c->next) return false;
    c->next = now + 0.25;
    char why[160];
    c->aborted = !worker_allowed(c->app, 0, why, sizeof(why));
    g_whisper_aborting = c->aborted;
    return c->aborted;
}

// The 16 kHz mono 16-bit samples of a WAV file, as floats in `a`. False if it is not that.
static bool wav_samples(Arena* a, Str wav, Slice<f32>* out) {
    *out = {};
    if (wav.len < 12 || memcmp(wav.ptr, "RIFF", 4) != 0 || memcmp(wav.ptr + 8, "WAVE", 4) != 0) return false;
    bool format_ok = false;
    for (i64 at = 12; at + 8 <= wav.len;) {
        const char* chunk = wav.ptr + at;
        u32 size = 0;
        memcpy(&size, chunk + 4, 4);
        const char* body = chunk + 8;
        i64 have = wav.len - (at + 8);
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16 && have >= 16) {
            u16 format, channels, bits;
            u32 rate;
            memcpy(&format, body, 2);
            memcpy(&channels, body + 2, 2);
            memcpy(&rate, body + 4, 4);
            memcpy(&bits, body + 14, 2);
            format_ok = format == 1 && channels == 1 && rate == 16000 && bits == 16;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!format_ok) return false;
            i64 n = (size < have ? size : have) / 2;
            f32* samples = PUSH_ARRAY(a, f32, n + 1);
            for (i64 i = 0; i < n; i++) {
                i16 s;
                memcpy(&s, body + i * 2, 2);
                samples[i] = (f32)s / 32768.0f;
            }
            *out = {samples, (i32)n};
            return true;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Transcribing one file

// Where a file goes when your own server is set. Copied from the settings.
struct Target {
    char url[512];  // the API base, ending in /v1
    char key[256];
    char model[128];
    char language[16];
};

struct Heard {
    f64 start;  // seconds into the file
    f64 end;
    Str text;
};

// Phrases Whisper invents on music or noise; dropped when they are the whole line.
static bool hallucination(Str text) {
    static const char* made_up[] = {"thank you.", "thanks for watching.", "thanks for watching!", "thank you for watching.",
                                    "thank you for watching!", "you", "bye.", ".", "subtitles by the amara.org community"};
    for (i32 i = 0; i < COUNT(made_up); i++)
        if (text.len == (i64)strlen(made_up[i]) && _strnicmp(text.ptr, made_up[i], (size_t)text.len) == 0) return true;
    return false;
}

// Adds one segment to `lines` unless it is empty, made up or most likely not speech. `raw` is
// the text as Whisper gave it: one that starts inside a word ("don" + "'t want it") goes on the
// end of the line before.
static void hear(Arena* a, Heard* lines, i32* count, Str raw, f64 from, f64 to, f64 no_speech, f64 logprob) {
    Str text = str_trim(raw);
    if (!text.len) return;
    if (*count && raw.ptr[0] != ' ' && from - lines[*count - 1].end < 1.0) {
        Heard* last = &lines[*count - 1];
        last->text = str_fmt(a, "%.*s%.*s", (int)last->text.len, last->text.ptr, (int)text.len, text.ptr);
        last->end = to;
        return;
    }
    if (hallucination(text)) return;
    if (no_speech > 0.6 && logprob < -1.0) return;
    lines[(*count)++] = {from, to, str_copy(a, text)};
}

enum Sent : u8 {
    SENT_OK,
    SENT_RETRY,    // could not be done now (your server could not be reached, or is busy): try later
    SENT_BAD,      // this file cannot be transcribed: set it aside
    SENT_ABORTED,  // stopped part-way because transcription has to wait: the file stays queued
};

// With our own whisper.cpp. In `a`.
static Sent transcribe_here(App* app, Arena* a, whisper_context* ctx, const char* language, bool mic, Str wav, Slice<Heard>* out, char* error, i32 cap) {
    *out = {};
    Slice<f32> samples = {};
    if (!wav_samples(a, wav, &samples)) {
        snprintf(error, cap, "not a 16 kHz mono 16-bit WAV");
        return SENT_BAD;
    }
    whisper_full_params wp = p_whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    wp.n_threads = 4;
    wp.beam_search.beam_size = 5;
    wp.language = language[0] ? language : "auto";
    // No text carried over from the last window, so one bad line does not repeat into the next.
    wp.no_context = true;
    wp.n_max_text_ctx = 0;
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    // Silero voice detection: only speech is transcribed, so music, game noise and silence are
    // skipped (and do not get made-up lines). Timestamps stay on the file's own timeline.
    wchar_t dir[MAX_PATH], vad[MAX_PATH];
    whisper_dir(settings(app), dir);
    swprintf(vad, MAX_PATH, L"%ls\\%hs", dir, WHISPER_VAD_MODEL);
    Str vad_path = narrow(a, vad);
    wp.vad = true;
    wp.vad_model_path = vad_path.ptr;
    wp.vad_params.min_silence_duration_ms = 500;
    // The mic is only ever your own voice, close up: a lower bar than for apps (0.5), whose
    // music and game sound would get through it.
    if (mic) wp.vad_params.threshold = 0.35f;
    AbortCheck check = {app, 0, false};
    wp.abort_callback = whisper_should_abort;
    wp.abort_callback_user_data = &check;

    g_whisper_aborting = false;
    i32 result = p_whisper_full(ctx, wp, samples.items, samples.count);
    g_whisper_aborting = false;
    if (check.aborted) return SENT_ABORTED;
    if (result != 0) {
        snprintf(error, cap, "whisper.cpp failed on it (%d)", result);
        return SENT_BAD;
    }
    i32 n = p_whisper_full_n_segments(ctx);
    Heard* lines = PUSH_ARRAY(a, Heard, n + 1);
    i32 count = 0;
    i32 eot = p_whisper_token_eot(ctx);
    for (i32 i = 0; i < n; i++) {
        // The average log probability of its text tokens, as OpenAI's Whisper reports it.
        i32 tokens = p_whisper_full_n_tokens(ctx, i);
        f64 total = 0;
        for (i32 j = 0; j < tokens; j++) {
            whisper_token_data t = p_whisper_full_get_token_data(ctx, i, j);
            if (t.id < eot) total += t.plog;
        }
        f64 logprob = tokens ? total / tokens : 0;
        hear(a, lines, &count, str(p_whisper_full_get_segment_text(ctx, i)), p_whisper_full_get_segment_t0(ctx, i) * 0.01,
             p_whisper_full_get_segment_t1(ctx, i) * 0.01, p_whisper_full_get_segment_no_speech_prob(ctx, i), logprob);
    }
    *out = {lines, count};
    return SENT_OK;
}

// With your own server: POST <url>/audio/transcriptions (OpenAI's API, which most transcription
// servers speak), reading the lines out of the verbose_json answer. In `a`.
static Sent transcribe_there(Arena* a, Target* t, Str wav, Slice<Heard>* out, char* error, i32 cap) {
    *out = {};
    const char* boundary = "lifelog-3f9d2c7a51e8";
    Buf head = {};
    const char* fields[][2] = {{"model", t->model}, {"response_format", "verbose_json"}, {"temperature", "0"}, {"language", t->language}};
    for (i32 i = 0; i < COUNT(fields); i++) {
        if (!fields[i][1][0]) continue;
        buf_fmt(&head, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", boundary, fields[i][0], fields[i][1]);
    }
    buf_fmt(&head, "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n", boundary);
    Str tail = str_fmt(a, "\r\n--%s--\r\n", boundary);
    Str start = buf_take(&head, a);
    buf_free(&head);
    i64 len = start.len + wav.len + tail.len;
    char* body = PUSH_ARRAY(a, char, len);
    memcpy(body, start.ptr, (size_t)start.len);
    memcpy(body + start.len, wav.ptr, (size_t)wav.len);
    memcpy(body + start.len + wav.len, tail.ptr, (size_t)tail.len);

    Str headers = str_fmt(a, "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    if (t->key[0]) headers = str_fmt(a, "%sAuthorization: Bearer %s\r\n", headers.ptr, t->key);
    Str url = str_fmt(a, "%s/audio/transcriptions", t->url);
    HttpReply reply = {};
    char why[256];
    // Long clips take a while on a slow server.
    if (!http_call(a, "POST", url, headers, {body, len}, 10 * 60 * 1000, &reply, why, sizeof(why))) {
        snprintf(error, cap, "could not reach %s: %s", t->url, why);
        return SENT_RETRY;
    }
    if (reply.status != 200) {
        snprintf(error, cap, "%s answered %u: %.200s", t->url, reply.status, reply.body.ptr);
        return reply.status == 429 || reply.status >= 500 ? SENT_RETRY : SENT_BAD;
    }
    Json j = {};
    char parse_error[128];
    if (!json_parse(a, reply.body, &j, parse_error, sizeof(parse_error))) {
        snprintf(error, cap, "the answer is not JSON (%s)", parse_error);
        return SENT_BAD;
    }
    const Json* segments = json_get(&j, "segments");
    i32 n = json_len(segments);
    Heard* lines = PUSH_ARRAY(a, Heard, n + 1);
    i32 count = 0;
    for (i32 i = 0; i < n; i++) {
        const Json* s = json_at(segments, i);
        hear(a, lines, &count, json_text(json_get(s, "text")), json_get(s, "start")->num, json_get(s, "end")->num,
             json_get(s, "no_speech_prob")->num, json_get(s, "avg_logprob")->num);
    }
    // A server that gives only the text: one line for the whole file.
    if (!n) hear(a, lines, &count, json_text(json_get(&j, "text")), 0, json_get(&j, "duration")->num, 0, 0);
    *out = {lines, count};
    return SENT_OK;
}

// Moves the clip and its .json to spool\failed, to be looked at by hand.
static void set_aside(const wchar_t* spool, const wchar_t* wav, const wchar_t* meta) {
    wchar_t dir[MAX_PATH], to[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%ls\\failed", spool);
    CreateDirectoryW(dir, 0);
    const wchar_t* files[] = {wav, meta};
    for (i32 i = 0; i < 2; i++) {
        const wchar_t* slash = wcsrchr(files[i], L'\\');
        swprintf(to, MAX_PATH, L"%ls\\%ls", dir, slash ? slash + 1 : files[i]);
        MoveFileExW(files[i], to, MOVEFILE_REPLACE_EXISTING);
    }
}


// Transcribes spool\<name> (with `ctx`, or your server `t` when ctx is null) and saves its lines.
// SENT_OK also when the file was set aside; on SENT_RETRY and SENT_ABORTED the file stays where
// it is. Not on the main thread.
static Sent transcribe_file(App* app, sqlite3* db, whisper_context* ctx, const char* language, Target* t, const wchar_t* spool,
                            const wchar_t* name, i32* lines_out) {
    *lines_out = 0;
    Arena* a = scratch();
    Mark m = mark(a);
    wchar_t wav_path[MAX_PATH], meta_path[MAX_PATH];
    swprintf(wav_path, MAX_PATH, L"%ls\\%ls", spool, name);
    wcscpy_s(meta_path, MAX_PATH, wav_path);
    wcscpy_s(meta_path + wcslen(meta_path) - 4, 6, L".json");
    Str label = narrow(a, name);

    Str meta_text = read_file(a, meta_path);
    Str wav = read_file(a, wav_path);
    Json meta = {};
    char error[512] = "";
    if (!meta_text.ptr || !wav.ptr || !json_parse(a, meta_text, &meta, error, sizeof(error))) {
        WARN("%s: its .json is missing or unreadable (%s); set aside in spool\\failed", label.ptr, error);
        set_aside(spool, wav_path, meta_path);
        rewind(m);
        return SENT_OK;
    }
    Slice<Heard> heard = {};
    bool mic = str_eq(json_text(json_get(&meta, "source")), str("mic"));
    Sent sent = ctx ? transcribe_here(app, a, ctx, language, mic, wav, &heard, error, sizeof(error))
                    : transcribe_there(a, t, wav, &heard, error, sizeof(error));
    if (sent == SENT_RETRY || sent == SENT_ABORTED) {
        if (sent == SENT_RETRY && !stopping(app)) WARN("%s: %s; trying again later", label.ptr, error);
        rewind(m);
        return sent;
    }
    if (sent == SENT_BAD) {
        WARN("%s: %s; set aside in spool\\failed", label.ptr, error);
        set_aside(spool, wav_path, meta_path);
        rewind(m);
        return SENT_OK;
    }

    f64 start_ts = json_get(&meta, "start_ts")->num;
    Str source = json_text(json_get(&meta, "source"));
    Str exe = json_text(json_get(&meta, "exe"));
    Str named = json_text(json_get(&meta, "speaker"));  // a person in a call, if the clip says
    Str speaker = str_eq(source, str("mic")) ? str("me") : named.len ? named : exe.len ? exe : str("unknown");
    // Kept under the calendar day it was recorded on, for playback.
    LocalTime lt = local_time(start_ts);
    Str kept = heard.count ? str_fmt(a, "%04d-%02d-%02d/%s", lt.year, lt.month, lt.day, label.ptr) : Str{};

    if (heard.count) {
        sqlite3_stmt* st = 0;
        bool saved = sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0) == SQLITE_OK &&
                     sqlite3_prepare_v2(db,
                                        "INSERT INTO segments (start_ts, end_ts, source, speaker, exe, pid, title, device, text, audio, audio_offset)"
                                        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                        -1, &st, 0) == SQLITE_OK;
        const Json* pid = json_get(&meta, "pid");
        for (i32 i = 0; saved && i < heard.count; i++) {
            Heard* h = &heard.items[i];
            sqlite3_bind_double(st, 1, start_ts + h->start);
            sqlite3_bind_double(st, 2, start_ts + h->end);
            bind_str(st, 3, source);
            bind_str(st, 4, speaker);
            bind_opt(st, 5, exe);
            if (pid->type == JSON_NUMBER) sqlite3_bind_int64(st, 6, (i64)pid->num);
            else sqlite3_bind_null(st, 6);
            bind_opt(st, 7, json_text(json_get(&meta, "title")));
            bind_opt(st, 8, json_text(json_get(&meta, "device")));
            bind_str(st, 9, h->text);
            bind_opt(st, 10, kept);
            sqlite3_bind_double(st, 11, h->start);
            saved = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_reset(st);
        }
        if (!saved) WARN("%s: saving its lines failed (%s); trying again later", label.ptr, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        saved = saved && sqlite3_exec(db, "COMMIT", 0, 0, 0) == SQLITE_OK;
        if (!saved) {
            sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
            rewind(m);
            return SENT_RETRY;
        }
        wchar_t audio[MAX_PATH], day_dir[MAX_PATH], to[MAX_PATH];
        audio_dir((Config*)settings(app), audio);  // data_dir does not change while the program runs
        swprintf(day_dir, MAX_PATH, L"%ls\\%04d-%02d-%02d", audio, lt.year, lt.month, lt.day);
        SHCreateDirectoryExW(0, day_dir, 0);
        swprintf(to, MAX_PATH, L"%ls\\%ls", day_dir, name);
        if (!MoveFileExW(wav_path, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) DeleteFileW(wav_path);
    } else {
        DeleteFileW(wav_path);
    }
    DeleteFileW(meta_path);
    *lines_out = heard.count;
    rewind(m);
    return SENT_OK;
}

// ---------------------------------------------------------------------------------------------
// The supervisor
//
// It takes no lock. It reads atomics and settings(), and posts what it is doing to the main
// thread when that changes. It sleeps until worker_wake (a new WAV, a game, the GPU, the summary,
// a setting) or a timer it needs (a retry, the countdown after a game) says to look again.

// What the window shows of it: the supervisor's status record, posted through Worker.seen when
// it changes, so only the newest waits for the main thread.
struct WorkerSeen {
    char state[256];
    i32 queued;
    f64 queued_secs;
    bool running, missing, waiting, game;
};

// The main thread.
static void worker_seen_apply(App* app, Event* e) {
    Worker* w = &app->worker;
    WorkerSeen seen = latest_take<WorkerSeen>(e);
    w->queued = seen.queued;
    w->queued_secs = seen.queued_secs;
    w->running = seen.running;
    w->missing = seen.missing;
    if (!seen.waiting) w->waiting_since = 0;
    else if (!w->waiting_since) w->waiting_since = now_ts();
    Str game = strip_exe(str(app->apps.game_running));
    if (seen.game && game.len) {
        snprintf(w->state, sizeof(w->state), "waiting for %.*s to close", (int)game.len, game.ptr);
    } else {
        SET(w->state, str(seen.state));
    }
    wake_window(app);
}

static void worker_lines_apply(App* app, Event*) {
    app->worker.lines_version++;
    wake_window(app);
}

static void worker_post_seen(App* app, WorkerSeen* now, WorkerSeen* last) {
    atomic_set(&app->worker.live_queued, now->queued);
    atomic_set(&app->worker.live_running, now->running);
    if (memcmp(now, last, sizeof(WorkerSeen)) == 0) return;
    *last = *now;
    latest_send(app, &app->worker.seen, worker_seen_apply, *now);
}

static DWORD WINAPI supervisor_thread(void* p) {
    App* app = (App*)p;
    wchar_t spool[MAX_PATH];
    spool_dir((Config*)settings(app), spool);  // data_dir does not change while the program runs
    sqlite3* db = 0;
    whisper_context* ctx = 0;  // our model, while it may use the GPU
    char ctx_model[128] = "";

    f64 next_load = 0;   // after a failed load, wait before trying again
    f64 retry_at = 0;    // after your server could not be reached
    f64 quiet_from = 0;  // when the last game closed, in auto mode; 0 if none has
    bool had_game = false, first = true, count = true;
    char trouble[256] = "";  // why the last try failed, while waiting to try again
    WorkerSeen seen = {}, last = {};
    memset(&seen, 0, sizeof(seen));  // compared with memcmp: no stray padding bytes
    memset(&last, 0, sizeof(last));
    last.queued = -1;  // so the first is posted
    while (!stopping(app)) {
        f64 now = now_ts();
        if (count) {  // the data drive may be slow, so only when woken or after a file
            scan_spool(spool, &seen.queued, &seen.queued_secs, 0);
            count = false;
        }

        bool load = false, unload = false, work = false;
        char state[256], model[128], language[16];
        Target t = {};
        const Config* c = settings(app);
        bool yours = c->worker.endpoint[0] != 0;
        bool ready = yours || whisper_ready(c, c->worker.model);
        if (ready == seen.missing || first) {
            if (!ready) WARN("transcription is not set up (Settings, Transcription): audio will queue up untranscribed");
            else if (!first) LOG("transcription is set up");
        }
        seen.missing = !ready;
        first = false;
        bool game = c->processing.pause_for_games && atomic_get(&app->apps.game_on);
        if (had_game && !game) quiet_from = now;
        had_game = game;
        SET(model, str(c->worker.model));
        SET(language, str(c->worker.language));
        bool want = ready && worker_allowed(app, quiet_from, state, sizeof(state));
        // The model leaves the GPU when it has to wait, when your server is used instead, or
        // when another model is chosen.
        if (ctx && (!want || yours || strcmp(ctx_model, model) != 0)) {
            const char* why = !want ? (strcmp(state, GAME_STATE) == 0 ? "waiting for the game to close" : state)
                              : yours ? "your own server is set" : "another model was chosen";
            LOG("freeing the Whisper model (%s)", why);
            unload = true;
        }
        if (want && !yours && (!ctx || unload) && now >= next_load) load = true;
        bool can = want && (yours || (ctx && !unload));
        if (can && seen.queued > 0 && now >= retry_at) {
            work = true;
            if (yours) {
                Str url = str_trim(str(c->worker.endpoint));
                while (url.len && url.ptr[url.len - 1] == '/') url.len--;
                SET(t.url, url);
                SET(t.key, str_trim(str(c->worker.api_key)));
                SET(t.model, str(model));
                SET(t.language, str(language));
            }
        }
        // A countdown or a retry shows its seconds, so meanwhile it looks again each second.
        bool ticking = false;
        if (want) {
            if (now < retry_at) {
                snprintf(state, sizeof(state), "%s: trying again in %.0f s", trouble, retry_at - now);
                ticking = true;
            } else if (now < next_load) {
                snprintf(state, sizeof(state), "%s: trying again in %.0f s", trouble, next_load - now);
                ticking = true;
            } else if (can) {
                snprintf(state, sizeof(state), seen.queued ? "transcribing" : "up to date");
            } else {
                snprintf(state, sizeof(state), "starting");
            }
        } else {
            if (!ready) snprintf(state, sizeof(state), "not set up yet");
            ticking = quiet_from > 0 && now - quiet_from < (f64)c->processing.resume_after_secs + 1;
        }
        SET(seen.state, str(state));
        seen.game = strcmp(state, GAME_STATE) == 0;
        seen.waiting = !want;
        seen.running = can;
        worker_post_seen(app, &seen, &last);

        if (unload) {
            p_whisper_free(ctx);
            ctx = 0;
            ctx_model[0] = 0;
        }
        if (load) {
            char error[512];
            ctx = whisper_open(settings(app), model, error, sizeof(error));
            if (ctx) {
                SET(ctx_model, str(model));
            } else {
                WARN("%s; trying again in a minute", error);
                SET(trouble, str(error));
                next_load = now_ts() + 60;
            }
            continue;
        }
        if (work) {
            wchar_t name[MAX_PATH];
            scan_spool(spool, 0, 0, name);
            if (!name[0]) {
                count = true;  // the count was out of date
                continue;
            }
            if (!db) db = db_open(app);
            if (!db) {
                SET(trouble, str("the database could not be opened"));
                retry_at = now_ts() + 30;
                continue;
            }
            f64 began = now_ts();
            i32 lines = 0;
            Sent sent = transcribe_file(app, db, yours ? 0 : ctx, language, &t, spool, name, &lines);
            if (sent == SENT_ABORTED) continue;  // the next round sees why, and frees the model
            if (sent == SENT_RETRY) {
                SET(trouble, str(yours ? "your server could not be reached" : "the database was busy"));
                retry_at = now_ts() + 30;
                continue;
            }
            scan_spool(spool, &seen.queued, &seen.queued_secs, 0);
            LOG("%ls: %d lines in %.1fs (%d queued)", name, lines, now_ts() - began, seen.queued);
            if (lines > 0) event_send(app, worker_lines_apply);
            continue;
        }
        // Woken by worker_wake; the minute is only a safety net.
        HANDLE waits[2] = {app->stop, app->worker.wake};
        DWORD got = WaitForMultipleObjects(2, waits, FALSE, ticking ? 1000 : 60000);
        if (got == WAIT_OBJECT_0 + 1 || got == WAIT_TIMEOUT) count = true;
    }
    // A clip being transcribed was stopped by the abort check; it stays queued for next time.
    if (ctx) p_whisper_free(ctx);
    if (db) sqlite3_close(db);
    SetEvent(app->worker.done);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// worker.h

void worker_start(App* app) {
    Config* c = &app->config;
    if (c->processing.gpu_busy_percent < 1) c->processing.gpu_busy_percent = 1;
    if (c->processing.gpu_busy_percent > 100) c->processing.gpu_busy_percent = 100;
    SET(app->worker.state, str("starting"));
    app->worker.done = CreateEventW(0, TRUE, FALSE, 0);
    // Made in run_app, before capture starts: the capture threads wake the supervisor.
    CHECK(app->worker.wake, "the worker's wake event made before worker_start");
    start_thread(app, "worker-supervisor", supervisor_thread);
    start_thread(app, "gpu-monitor", gpu_monitor_thread);
}

void worker_set_mode(App* app, Mode mode) {
    if (app->config.processing.mode == mode) return;
    app->config.processing.mode = mode;
    LOG("transcription mode: %s", mode_name(mode));
    config_save_string("processing", "mode", mode_name(mode));
}

void worker_set_gpu_limit(App* app, u32 percent) {
    percent = percent < 1 ? 1 : percent > 100 ? 100 : percent;
    // The slider calls this every frame it is dragged; only a change is saved.
    if (app->config.processing.gpu_busy_percent == percent) return;
    app->config.processing.gpu_busy_percent = percent;
    LOG("a game is running when 3D load is above %u%%", percent);
    config_save_u32("processing", "gpu_busy_percent", percent);
}

void worker_set_pause_for_games(App* app, bool on) {
    if (app->config.processing.pause_for_games == on) return;
    app->config.processing.pause_for_games = on;
    LOG("auto mode %s while a game runs", on ? "waits" : "doesn't wait");
    config_save_bool("processing", "pause_for_games", on);
}

void worker_set_endpoint(App* app, Str url, Str model) {
    url = str_trim(url);
    model = str_trim(model);
    SET(app->config.worker.endpoint, url);
    config_save_string("worker", "endpoint", app->config.worker.endpoint);
    if (model.len) {
        SET(app->config.worker.model, model);
        config_save_string("worker", "model", app->config.worker.model);
    }
    LOG("transcription: %s", url.len ? app->config.worker.endpoint : "lifelog's own whisper.cpp");
}

void worker_wake(App* app) {
    if (app->worker.wake) SetEvent(app->worker.wake);
}

void worker_stop(App* app) {
    // The supervisor sees `stop`, cuts the clip it is on short and frees the model; a GPU in use
    // while the process ends can hang it.
    if (app->worker.done && WaitForSingleObject(app->worker.done, 15000) != WAIT_OBJECT_0) WARN("transcription did not stop in 15 s");
}
