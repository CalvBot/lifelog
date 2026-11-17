// worker.cpp: see worker.h. Two threads, neither touches App:
//   - the supervisor decides whether transcription may run, loads and frees the model, counts
//     the queue and transcribes the files, oldest first; it sleeps until woken;
//   - the GPU monitor reads the load and memory of the GPU Whisper runs on every 2 s.

#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")

#include "vendor/whisper/whisper.h"

// ---------------------------------------------------------------------------------------------
// The GPU, from the counters Task Manager uses. Only the adapter Whisper runs on is watched (until
// that is known, the busiest of them all), and lifelog's own use is left out. Three readings, kept
// apart because they mean different things:
//   - 3D load: games and other rendering. Over the limit for a while, transcription steps aside so
//     it does not cost frames.
//   - compute load: other programs' CUDA and compute work. Shown, but it does not pause
//     transcription: sharing compute slows both jobs down, it does not cost anyone frames.
//   - memory: how much of the adapter's own memory other programs hold. The model is not loaded
//     while it would not fit beside them, and is freed when they grow into it.

struct GpuWatch {
    PDH_HQUERY query;
    PDH_HCOUNTER engines;      // GPU Engine: utilisation per process and engine
    PDH_HCOUNTER process_mem;  // GPU Process Memory: dedicated memory per process and adapter
    PDH_HCOUNTER adapter_mem;  // GPU Adapter Memory: dedicated memory in use per adapter
};

struct GpuReading {
    f64 load_3d, load_compute;  // percent: the busiest engine of each kind, other programs only
    f64 others_bytes;           // the adapter's dedicated memory held by other programs
    f64 own_bytes;              // and by lifelog
};

static bool gpu_watch_open(GpuWatch* g) {
    *g = {};
    if (PdhOpenQueryW(nullptr, 0, &g->query) != ERROR_SUCCESS) return false;
    bool ok = PdhAddEnglishCounterW(g->query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g->engines) == ERROR_SUCCESS;
    // Memory is optional: without it only the loads are watched.
    if (PdhAddEnglishCounterW(g->query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &g->process_mem) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(g->query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g->adapter_mem) != ERROR_SUCCESS)
        g->process_mem = g->adapter_mem = 0;
    if (!ok) {
        PdhCloseQuery(g->query);
        *g = {};
        return false;
    }
    PdhCollectQueryData(g->query);  // rates need two samples; this is the first
    return true;
}

// "pid_12_luid_0x00000000_0x0000D1A5_phys_0…" → the adapter's LUID as one number, high part first
// as it is named (0 if the name has none).
static u64 gpu_name_luid(const wchar_t* name) {
    const wchar_t* at = name ? wcsstr(name, L"luid_0x") : 0;
    if (!at) return 0;
    wchar_t* end;
    u64 high = wcstoull(at + 7, &end, 16);
    if (wcsncmp(end, L"_0x", 3) != 0) return 0;
    u64 low = wcstoull(end + 3, 0, 16);
    return high << 32 | (low & 0xffffffffull);
}

// A counter's values for every instance, in scratch; null if there is no reading.
static PDH_FMT_COUNTERVALUE_ITEM_W* gpu_items(PDH_HCOUNTER c, DWORD* count) {
    DWORD size = 0;
    *count = 0;
    if (!c) return 0;
    if ((u32)PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, count, nullptr) != (u32)PDH_MORE_DATA || !size) return 0;
    // The items come first, the instance names after them in the same buffer.
    PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)arena_push(scratch(), size, 8);
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, count, items) != ERROR_SUCCESS) return 0;
    return items;
}

// One engine of one adapter, its load summed over the programs using it.
struct GpuEngine {
    const wchar_t* key;  // its instance name from "luid_" on (the same for every program)
    f64 load;
    bool is_3d;
};

// Reads the adapter `luid` (0: every adapter, the busiest engine of all; no memory). Call at least
// a second apart. False if there is no reading.
static bool gpu_read(GpuWatch* g, u64 luid, GpuReading* r) {
    *r = {};
    if (PdhCollectQueryData(g->query) != ERROR_SUCCESS) return false;
    Mark m = mark(scratch());
    // Our own are skipped: Whisper's kernels and the window show up as GPU work, and counting them
    // made transcription pause itself.
    wchar_t own[32];
    i32 own_len = swprintf(own, 32, L"pid_%lu_", GetCurrentProcessId());
    DWORD n = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = gpu_items(g->engines, &n);
    if (!items) {
        rewind(m);
        return false;
    }
    GpuEngine* engines = PUSH_ARRAY(scratch(), GpuEngine, 512);
    i32 count = 0;
    for (DWORD i = 0; i < n; i++) {
        const wchar_t* name = items[i].szName;
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA || !name) continue;
        if (wcsncmp(name, own, own_len) == 0) continue;
        if (luid && gpu_name_luid(name) != luid) continue;
        const wchar_t* key = wcsstr(name, L"luid_");
        const wchar_t* type = wcsstr(name, L"engtype_");
        if (!key || !type) continue;
        type += 8;
        bool is_3d = wcscmp(type, L"3D") == 0 || wcsncmp(type, L"Graphics", 8) == 0;
        bool is_compute = wcsncmp(type, L"Compute", 7) == 0 || wcscmp(type, L"Cuda") == 0;
        if (!is_3d && !is_compute) continue;  // copy, video, overlay: neither
        i32 k = 0;
        while (k < count && wcscmp(engines[k].key, key) != 0) k++;
        if (k == count) {
            if (count == 512) continue;
            engines[count++] = {key, 0, is_3d};
        }
        engines[k].load += items[i].FmtValue.doubleValue;
    }
    for (i32 k = 0; k < count; k++) {
        f64 load = engines[k].load > 100 ? 100 : engines[k].load;
        f64* to = engines[k].is_3d ? &r->load_3d : &r->load_compute;
        if (load > *to) *to = load;
    }
    if (luid) {
        f64 used = 0;
        items = gpu_items(g->adapter_mem, &n);
        for (DWORD i = 0; items && i < n; i++)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && gpu_name_luid(items[i].szName) == luid)
                used += items[i].FmtValue.doubleValue;
        items = gpu_items(g->process_mem, &n);
        for (DWORD i = 0; items && i < n; i++)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && gpu_name_luid(items[i].szName) == luid &&
                wcsncmp(items[i].szName, own, own_len) == 0)
                r->own_bytes += items[i].FmtValue.doubleValue;
        r->others_bytes = used > r->own_bytes ? used - r->own_bytes : 0;
    }
    rewind(m);
    return true;
}

// The adapter's own (dedicated) memory, from DXGI; 0 if it is not found.
static f64 gpu_total_bytes(u64 luid) {
    IDXGIFactory1* factory = 0;
    if (!luid || FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return 0;
    f64 total = 0;
    IDXGIAdapter1* a = 0;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && ((u64)(u32)d.AdapterLuid.HighPart << 32 | d.AdapterLuid.LowPart) == luid)
            total = (f64)d.DedicatedVideoMemory;
        a->Release();
    }
    factory->Release();
    return total;
}

// What the model takes on the GPU before it has been loaded once: its file and some room to work.
static f64 gpu_model_guess(const WorkerOptions* o) {
    wchar_t bin[MAX_PATH];
    model_path(o->ai_dir, SETUP_TRANSCRIBER, o->model, bin);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(bin, GetFileExInfoStandard, &info)) return 0;
    f64 size = (f64)(((u64)info.nFileSizeHigh << 32) | info.nFileSizeLow);
    return size * 1.25 + 512.0 * MB(1);
}

// Copies the options into `out` if the main thread has handed over new ones since `*version`.
static void worker_options_take(App* app, WorkerOptions* out, LONG* version) {
    Worker* w = &app->worker;
    LONG now = atomic_get(&w->options_version);
    if (now == *version) return;
    AcquireSRWLockShared(&w->options_lock);
    *out = w->options;
    *version = atomic_get(&w->options_version);
    ReleaseSRWLockShared(&w->options_lock);
}

// A condition that has to hold for three readings (~6 s) to turn on, and to have been off for
// `resume_secs` to turn off again (so a loading screen or a menu does not start a run mid-game).
struct GpuGate {
    u32 over;
    f64 last_over;
    bool on;
};

static void gpu_gate_step(GpuGate* g, bool over, f64 resume_secs) {
    f64 now = now_ts();
    if (over) {
        g->over++;
        g->last_over = now;
        if (g->over >= 3) g->on = true;
    } else {
        g->over = 0;
        if (g->on && now - g->last_over >= resume_secs) g->on = false;
    }
}

static DWORD WINAPI gpu_monitor_thread(void* p) {
    App* app = (App*)p;
    Worker* w = &app->worker;
    GpuWatch gpu;
    if (!gpu_watch_open(&gpu)) {
        WARN("GPU load counters unavailable: transcription will not pause for the GPU load");
        return 0;
    }
    GpuGate busy = {}, short_of = {};
    u64 luid = 0;
    f64 total = 0;
    f64 peak = 0;        // the most the loaded model has held, measured
    char peak_model[128] = "";
    WorkerOptions o = {};
    LONG version = 0;
    while (sleep_unless_stopping(app, 2000)) {
        u64 now_luid = (u64)atomic_get64(&w->gpu_luid);
        if (now_luid != luid) {
            luid = now_luid;
            total = gpu_total_bytes(luid);
            busy = short_of = {};
        }
        GpuReading r;
        if (!gpu_read(&gpu, luid, &r)) continue;
        worker_options_take(app, &o, &version);
        if (strcmp(peak_model, o.model) != 0) {
            peak = 0;
            SET(peak_model, str(o.model));
        }
        bool loaded = WaitForSingleObject(w->gpu_free, 0) != WAIT_OBJECT_0;
        if (loaded && r.own_bytes > peak) peak = r.own_bytes;
        f64 need = peak > 0 ? peak : gpu_model_guess(&o);
        f64 room = total - r.others_bytes;  // what other programs leave of it
        bool was_busy = busy.on, was_short = short_of.on;
        if (atomic_get(&w->summary_needs_gpu)) {
            // The summary model's own load and memory show up as other programs'; it is not a game.
            busy.over = short_of.over = 0;
        } else {
            gpu_gate_step(&busy, r.load_3d > (f64)o.gpu_busy_percent, (f64)o.resume_after_secs);
            // Short when the model would not fit beside the others (or no longer does), with a
            // quarter GB to spare; only once the adapter and its size are known.
            bool tight = total > 0 && need > 0 && room < need + 256.0 * MB(1);
            gpu_gate_step(&short_of, tight, (f64)o.resume_after_secs);
        }
        if (busy.on && !was_busy) LOG("GPU busy (%.0f%% 3D): transcription waits, audio keeps queueing", r.load_3d);
        if (!busy.on && was_busy) LOG("GPU free again: transcription resumes");
        if (short_of.on && !was_short)
            LOG("GPU memory short (%.1f of %.1f GB free beside other programs, the model needs %.1f): transcription waits",
                room / GB(1), total / GB(1), need / GB(1));
        if (!short_of.on && was_short) LOG("GPU memory free again: transcription resumes");
        LONG pct_3d = (LONG)(r.load_3d + 0.5), pct_compute = (LONG)(r.load_compute + 0.5);
        LONG free_mb = total > 0 ? (LONG)(room / MB(1)) : -1, need_mb = (LONG)(need / MB(1));
        bool changed = pct_3d != atomic_get(&w->gpu_3d_percent) || pct_compute != atomic_get(&w->gpu_compute_percent) ||
                       free_mb / 64 != atomic_get(&w->gpu_free_mb) / 64 || need_mb != atomic_get(&w->gpu_need_mb);
        atomic_set(&w->gpu_3d_percent, pct_3d);
        atomic_set(&w->gpu_compute_percent, pct_compute);
        atomic_set(&w->gpu_free_mb, free_mb);
        atomic_set(&w->gpu_need_mb, need_mb);
        atomic_set(&w->gpu_busy, busy.on);
        atomic_set(&w->gpu_short, short_of.on);
        bool gates = was_busy != busy.on || was_short != short_of.on;
        if (gates) worker_wake(app);
        if (changed || gates) wake_window(app);
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

// Whether transcription may run now, and the few words saying why (not). It reads atomics and the
// supervisor's options. Waiting for a game, the state is GAME_STATE and the main thread puts the
// game's name in.
static const char* GAME_STATE = "waiting for a game to close";

static bool worker_allowed(App* app, const WorkerOptions* c, f64 quiet_from, char* state, i32 cap) {
    Worker* w = &app->worker;
    Mode mode = c->mode;
    f64 wait = (f64)c->resume_after_secs - (now_ts() - quiet_from);
    if (atomic_get(&w->summary_needs_gpu)) {
        snprintf(state, cap, "waiting for the daily summary");
    } else if (mode == MODE_PAUSED) {
        snprintf(state, cap, "paused");
    } else if (mode == MODE_AUTO && c->pause_for_games && atomic_get(&app->apps.game_on)) {
        snprintf(state, cap, "%s", GAME_STATE);
    } else if (mode == MODE_AUTO && atomic_get(&w->gpu_busy)) {
        snprintf(state, cap, "waiting for the graphics card");
    } else if (mode == MODE_AUTO && !c->endpoint[0] && atomic_get(&w->gpu_short)) {
        // Only our own model needs room on this GPU; your server has its own.
        snprintf(state, cap, "waiting for graphics memory (%.1f GB free, the model needs %.1f)",
                 atomic_get(&w->gpu_free_mb) / 1024.0, atomic_get(&w->gpu_need_mb) / 1024.0);
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
WHISPER_FN(whisper_token_to_str)
WHISPER_FN(whisper_vad_default_params)
WHISPER_FN(whisper_vad_default_context_params)
WHISPER_FN(whisper_vad_init_from_file_with_params)
WHISPER_FN(whisper_vad_segments_from_samples)
WHISPER_FN(whisper_vad_segments_n_segments)
WHISPER_FN(whisper_vad_segments_get_segment_t0)
WHISPER_FN(whisper_vad_segments_get_segment_t1)
WHISPER_FN(whisper_vad_free_segments)
WHISPER_FN(whisper_log_set)
WHISPER_FN(ggml_backend_load_all_from_path)
// Which device ggml has: optional, for telling which GPU to watch.
WHISPER_FN(ggml_backend_dev_count)
WHISPER_FN(ggml_backend_dev_get)
WHISPER_FN(ggml_backend_dev_type)
WHISPER_FN(ggml_backend_dev_description)
WHISPER_FN(ggml_backend_dev_get_props)
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
    WHISPER_FN(whisper, whisper_token_to_str)
    WHISPER_FN(whisper, whisper_vad_default_params)
    WHISPER_FN(whisper, whisper_vad_default_context_params)
    WHISPER_FN(whisper, whisper_vad_init_from_file_with_params)
    WHISPER_FN(whisper, whisper_vad_segments_from_samples)
    WHISPER_FN(whisper, whisper_vad_segments_n_segments)
    WHISPER_FN(whisper, whisper_vad_segments_get_segment_t0)
    WHISPER_FN(whisper, whisper_vad_segments_get_segment_t1)
    WHISPER_FN(whisper, whisper_vad_free_segments)
    WHISPER_FN(whisper, whisper_log_set)
    WHISPER_FN(ggml, ggml_backend_load_all_from_path)
#undef WHISPER_FN
    if (!ok) {
        snprintf(error, cap, "whisper.dll is not the version lifelog expects: set it up again in Settings");
        return false;
    }
    swprintf(path, MAX_PATH, L"%ls\\ggml-base.dll", dir);
    HMODULE base = GetModuleHandleW(path);
    if (!base) base = GetModuleHandleW(L"ggml-base.dll");  // ggml.dll's import, by its plain name
    p_ggml_backend_dev_count = (decltype(p_ggml_backend_dev_count))GetProcAddress(ggml, "ggml_backend_dev_count");
    p_ggml_backend_dev_get = (decltype(p_ggml_backend_dev_get))GetProcAddress(ggml, "ggml_backend_dev_get");
    if (base) {
        p_ggml_backend_dev_type = (decltype(p_ggml_backend_dev_type))GetProcAddress(base, "ggml_backend_dev_type");
        p_ggml_backend_dev_description = (decltype(p_ggml_backend_dev_description))GetProcAddress(base, "ggml_backend_dev_description");
        p_ggml_backend_dev_get_props = (decltype(p_ggml_backend_dev_get_props))GetProcAddress(base, "ggml_backend_dev_get_props");
    }
    p_whisper_log_set(whisper_log, 0);  // ggml's log too
    Mark m = mark(scratch());
    p_ggml_backend_load_all_from_path(narrow(scratch(), dir).ptr);
    rewind(m);
    g_whisper_loaded = true;
    whisper_loaded_from(dir);
    return true;
}

// The attention heads that line text up with audio, for a model by its name. Other models use
// the top layers' heads, which align less well.
static whisper_alignment_heads_preset whisper_heads(const char* model) {
    static const struct { const char* name; whisper_alignment_heads_preset heads; } known[] = {
        {"large-v3-turbo", WHISPER_AHEADS_LARGE_V3_TURBO}, {"large-v3", WHISPER_AHEADS_LARGE_V3},
        {"large-v2", WHISPER_AHEADS_LARGE_V2}, {"large-v1", WHISPER_AHEADS_LARGE_V1},
        {"medium.en", WHISPER_AHEADS_MEDIUM_EN}, {"medium", WHISPER_AHEADS_MEDIUM},
        {"small.en", WHISPER_AHEADS_SMALL_EN}, {"small", WHISPER_AHEADS_SMALL},
        {"base.en", WHISPER_AHEADS_BASE_EN}, {"base", WHISPER_AHEADS_BASE},
        {"tiny.en", WHISPER_AHEADS_TINY_EN}, {"tiny", WHISPER_AHEADS_TINY},
    };
    // Longest names first, so "large-v3-turbo-q5_0" is turbo and "small.en" is not small.
    for (i32 i = 0; i < COUNT(known); i++)
        if (str_starts(str(model), str(known[i].name))) return known[i].heads;
    return WHISPER_AHEADS_N_TOP_MOST;
}

// Loads ggml-<model>.bin onto the GPU. Null (with `error`) if it could not.
static whisper_context* whisper_open(const char* ai_dir, const char* model, char* error, i32 cap) {
    wchar_t bins[MAX_PATH], bin[MAX_PATH];
    whisper_bin_dir(ai_dir, bins);
    if (!whisper_load(bins, error, cap)) return 0;
    model_path(ai_dir, SETUP_TRANSCRIBER, model, bin);
    whisper_context_params cp = p_whisper_context_default_params();
    cp.use_gpu = true;
    // Word times come from aligning each token with the audio (DTW over the attention heads that
    // follow it), which needs the model's own list of those heads and does not run with flash
    // attention. About a fifth slower.
    cp.flash_attn = false;
    cp.dtw_token_timestamps = true;
    cp.dtw_aheads_preset = whisper_heads(model);
    f64 began = now_ts();
    Mark m = mark(scratch());
    whisper_context* ctx = p_whisper_init_from_file_with_params(narrow(scratch(), bin).ptr, cp);
    rewind(m);
    if (!ctx) snprintf(error, cap, "could not load the model ggml-%s.bin", model);
    else LOG("Whisper %s loaded in %.1f s", model, now_ts() - began);
    return ctx;
}

// An adapter's PCI address, from the kernel graphics thunks in gdi32 (their structs, from
// d3dkmthk.h, which needs the driver kit's headers).
static bool adapter_pci(LUID luid, u32* bus, u32* device, u32* function) {
    struct OpenFromLuid { LUID luid; u32 adapter; };
    struct Query { u32 adapter; i32 type; void* data; u32 size; };
    struct Close { u32 adapter; };
    struct Address { u32 bus, device, function; };
    typedef LONG(APIENTRY * OpenFn)(OpenFromLuid*);
    typedef LONG(APIENTRY * QueryFn)(const Query*);
    typedef LONG(APIENTRY * CloseFn)(const Close*);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    OpenFn open = gdi ? (OpenFn)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid") : 0;
    QueryFn query = gdi ? (QueryFn)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo") : 0;
    CloseFn close = gdi ? (CloseFn)GetProcAddress(gdi, "D3DKMTCloseAdapter") : 0;
    if (!open || !query || !close) return false;
    OpenFromLuid o = {luid, 0};
    if (open(&o) != 0) return false;
    Address a = {};
    Query q = {o.adapter, 6 /* KMTQAITYPE_ADAPTERADDRESS */, &a, sizeof(a)};
    bool ok = query(&q) == 0;
    Close c = {o.adapter};
    close(&c);
    *bus = a.bus;
    *device = a.device;
    *function = a.function;
    return ok;
}

// The adapter whisper.cpp runs on, as a DXGI LUID (high part first): ggml's first GPU, the one
// whisper takes for gpu_device 0. Matched by PCI address once the model is `loaded` (ggml's
// device properties ask CUDA for free memory, which makes a context on the GPU: only once there
// is one anyway), else by name when only one adapter has it. 0 if it cannot be told.
static u64 whisper_adapter(bool loaded, char* name, i32 cap) {
    name[0] = 0;
    if (!p_ggml_backend_dev_count || !p_ggml_backend_dev_get || !p_ggml_backend_dev_type || !p_ggml_backend_dev_description)
        return 0;
    ggml_backend_dev_t dev = 0;
    for (size_t i = 0, n = p_ggml_backend_dev_count(); i < n && !dev; i++) {
        ggml_backend_dev_t d = p_ggml_backend_dev_get(i);
        if (p_ggml_backend_dev_type(d) == GGML_BACKEND_DEVICE_TYPE_GPU) dev = d;
    }
    if (!dev) return 0;  // no GPU: Whisper runs on the processor
    const char* described = p_ggml_backend_dev_description(dev);
    snprintf(name, cap, "%s", described ? described : "");
    bool pci = false;
    u32 bus = 0, device = 0, function = 0;
    if (loaded && p_ggml_backend_dev_get_props) {
        // Room to spare, in case this ggml's props have grown since the header lifelog has.
        struct { ggml_backend_dev_props p; u8 spare[256]; } props = {};
        p_ggml_backend_dev_get_props(dev, &props.p);
        unsigned domain, b, d, f;
        if (props.p.device_id && sscanf_s(props.p.device_id, "%x:%x:%x.%x", &domain, &b, &d, &f) == 4) {
            pci = true;
            bus = b, device = d, function = f;
        }
    }
    IDXGIFactory1* factory = 0;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return 0;
    u64 by_pci = 0, by_name = 0;
    i32 named = 0;
    IDXGIAdapter1* a = 0;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        bool got = SUCCEEDED(a->GetDesc1(&desc));
        a->Release();
        if (!got || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        u64 luid = (u64)(u32)desc.AdapterLuid.HighPart << 32 | desc.AdapterLuid.LowPart;
        u32 b, d, f;
        bool located = adapter_pci(desc.AdapterLuid, &b, &d, &f);
        // A virtual adapter (seen with Parsec's display) can carry the real card's name but has
        // no PCI address; its counters stay empty, so it is never the one to watch.
        if (located && b == 0xFFFFFFFF) continue;
        if (pci && located && b == bus && d == device && f == function) by_pci = luid;
        Mark m = mark(scratch());
        if (name[0] && str_eq_nocase(str_trim(narrow(scratch(), desc.Description)), str_trim(str(name)))) {
            named++;
            by_name = luid;
        }
        rewind(m);
    }
    factory->Release();
    return by_pci ? by_pci : named == 1 ? by_name : 0;
}

// Tells the GPU monitor which adapter to watch, when it has changed.
static void whisper_adapter_publish(App* app, bool loaded) {
    char name[256];
    u64 luid = whisper_adapter(loaded, name, sizeof(name));
    if (!luid) {
        static bool said;
        if (!said) {
            said = true;
            WARN("could not tell which GPU Whisper runs on (%s): the load on all of them counts",
                 !p_ggml_backend_dev_type ? "ggml-base.dll not found" : name[0] ? name : "no GPU device");
        }
        return;
    }
    if ((u64)atomic_get64(&app->worker.gpu_luid) == luid) return;
    InterlockedExchange64(&app->worker.gpu_luid, (LONG64)luid);
    LOG("Whisper runs on %s: only its load and memory decide when transcription waits", name);
}

// Asked by whisper.cpp between steps: stop this clip? Yes when lifelog is closing, or when
// transcription has to step aside (a game started, the summary wants the GPU). Looks at most
// every quarter second.
struct AbortCheck {
    App* app;
    WorkerOptions options;  // its own copy, so a change (say, to paused) counts mid-clip
    LONG version;
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
    worker_options_take(c->app, &c->options, &c->version);
    c->aborted = !worker_allowed(c->app, &c->options, 0, why, sizeof(why));
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

// A word as it was heard: its text (with the space before it, if any) and when it was said.
struct HeardWord {
    Str text;
    f64 start;  // seconds into the file
    f64 end;
};

struct Heard {
    f64 start;  // seconds into the file
    f64 end;
    Str text;
    i32 first_word;  // its words: [first_word, first_word + words) of the transcription's words
    i32 words;
};

// What a transcription heard: lines, and every kept line's words in order.
struct Transcript {
    Slice<Heard> lines;
    Slice<HeardWord> words;
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
// end of the line before. The segment's words are the last ones in `words`, from `first_word`
// on; they are dropped with it.
static void hear(Arena* a, Heard* lines, i32* count, i32* words, i32 first_word, Str raw, f64 from, f64 to, f64 no_speech, f64 logprob) {
    Str text = str_trim(raw);
    if (text.len && *count && raw.ptr[0] != ' ' && from - lines[*count - 1].end < 1.0) {
        Heard* last = &lines[*count - 1];
        last->text = str_fmt(a, "%.*s%.*s", (int)last->text.len, last->text.ptr, (int)text.len, text.ptr);
        last->end = to;
        last->words += *words - first_word;
        return;
    }
    if (!text.len || hallucination(text) || (no_speech > 0.6 && logprob < -1.0)) {
        *words = first_word;
        return;
    }
    lines[(*count)++] = {from, to, str_copy(a, text), first_word, *words - first_word};
}

enum Sent : u8 {
    SENT_OK,
    SENT_RETRY,    // could not be done now (your server could not be reached, or is busy): try later
    SENT_BAD,      // this file cannot be transcribed: set it aside
    SENT_ABORTED,  // stopped part-way because transcription has to wait: the file stays queued
};

// Silero voice detection, run before Whisper so that only speech is transcribed: music, game
// noise and silence are skipped (and do not get made-up lines). It runs on the CPU, and is loaded
// with the first clip and kept.
static whisper_vad_context* g_vad;

// A stretch of the file in what Whisper hears: samples [from, from + len) of the file are
// [at, at + len) of its input.
struct Speech {
    i64 from;
    i64 at;
    i64 len;
};

// A time in Whisper's input, in hundredths of a second, as seconds into the file. One in the
// short silence between two pieces stands for the whole pause they were cut from: it goes to the
// start of the speech after it when it is a start, to the end of the speech before when an end.
static f64 file_time(Slice<Speech> pieces, i64 cs, bool start) {
    i64 s = cs * (WHISPER_SAMPLE_RATE / 100);
    i32 i = 0;
    while (i + 1 < pieces.count && pieces.items[i + 1].at <= s) i++;
    Speech p = pieces.items[i];
    i64 at = p.from + (s < p.at ? 0 : s - p.at);
    if (s > p.at + p.len) at = start && i + 1 < pieces.count ? pieces.items[i + 1].from : p.from + p.len;
    return (f64)at / WHISPER_SAMPLE_RATE;
}

// Where the piece of speech holding a time in Whisper's input starts, both in hundredths of a second.
static i64 speech_start(Slice<Speech> pieces, i64 cs) {
    i64 s = cs * (WHISPER_SAMPLE_RATE / 100);
    i32 i = 0;
    while (i + 1 < pieces.count && pieces.items[i + 1].at <= s) i++;
    return pieces.items[i].at / (WHISPER_SAMPLE_RATE / 100);
}

// The speech in `samples`, one piece after another with a tenth of a second of silence between,
// the way whisper.cpp's own voice detection does it (each piece but the last runs a tenth of a
// second on, so its last word is not cut). False if the detector could not be run.
static bool speech_only(Arena* a, const char* vad_path, bool mic, Slice<f32> samples, Slice<f32>* heard, Slice<Speech>* pieces) {
    *heard = {};
    *pieces = {};
    if (!g_vad) g_vad = p_whisper_vad_init_from_file_with_params(vad_path, p_whisper_vad_default_context_params());
    if (!g_vad) return false;
    whisper_vad_params vp = p_whisper_vad_default_params();
    vp.min_silence_duration_ms = 500;
    // The mic is only ever your own voice, close up: a lower bar than for apps (0.5), whose
    // music and game sound would get through it.
    if (mic) vp.threshold = 0.35f;
    whisper_vad_segments* found = p_whisper_vad_segments_from_samples(g_vad, vp, samples.items, samples.count);
    if (!found) return false;
    i32 n = p_whisper_vad_segments_n_segments(found);
    const i64 overlap = WHISPER_SAMPLE_RATE / 10, silence = WHISPER_SAMPLE_RATE / 10;
    Speech* p = PUSH_ARRAY(a, Speech, n + 1);
    i32 count = 0;
    i64 at = 0;
    for (i32 i = 0; i < n; i++) {
        // In hundredths of a second.
        i64 from = (i64)(p_whisper_vad_segments_get_segment_t0(found, i) / 100.0 * WHISPER_SAMPLE_RATE);
        i64 to = (i64)(p_whisper_vad_segments_get_segment_t1(found, i) / 100.0 * WHISPER_SAMPLE_RATE);
        if (i + 1 < n) to += overlap;
        i64 last = samples.count - 1;
        from = from < last ? from : last;
        to = to < last ? to : last;
        if (to <= from) continue;
        if (count) at += silence;
        p[count++] = {from, at, to - from};
        at += to - from;
    }
    p_whisper_vad_free_segments(found);
    f32* out = PUSH_ARRAY(a, f32, at + 1);  // zeroed
    for (i32 i = 0; i < count; i++) memcpy(out + p[i].at, samples.items + p[i].from, sizeof(f32) * p[i].len);
    *heard = {out, (i32)at};
    *pieces = {p, count};
    return true;
}

// With our own whisper.cpp. In `a`.
//
// Each word's times come from the alignment (DTW) of its tokens with the audio. A token's aligned
// time falls near its end, so a word starts at the time of the token before it and ends at its
// own last token's. Tried against speech with known word times, this was within 35-80 ms on most
// words (and Whisper's own token timestamps were off by seconds).
static Sent transcribe_here(App* app, Arena* a, whisper_context* ctx, const WorkerOptions* o, bool mic, Str wav, Transcript* out,
                            char* error, i32 cap) {
    *out = {};
    Slice<f32> samples = {};
    if (!wav_samples(a, wav, &samples)) {
        snprintf(error, cap, "not a 16 kHz mono 16-bit WAV");
        return SENT_BAD;
    }
    wchar_t dir[MAX_PATH], vad[MAX_PATH];
    whisper_dir(o->ai_dir, dir);
    swprintf(vad, MAX_PATH, L"%ls\\%hs", dir, WHISPER_VAD_MODEL);
    Slice<f32> heard = {};
    Slice<Speech> pieces = {};
    if (!speech_only(a, narrow(a, vad).ptr, mic, samples, &heard, &pieces)) {
        snprintf(error, cap, "the voice detection model could not be run: set the transcriber up again in Settings");
        return SENT_RETRY;
    }
    if (!pieces.count) return SENT_OK;  // no speech

    whisper_full_params wp = p_whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    wp.n_threads = 4;
    wp.beam_search.beam_size = 5;
    wp.language = o->language[0] ? o->language : "auto";
    // No text carried over from the last window, so one bad line does not repeat into the next.
    wp.no_context = true;
    wp.n_max_text_ctx = 0;
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    AbortCheck* check = PUSH(a, AbortCheck);
    check->app = app;
    wp.abort_callback = whisper_should_abort;
    wp.abort_callback_user_data = check;

    g_whisper_aborting = false;
    i32 result = p_whisper_full(ctx, wp, heard.items, heard.count);
    g_whisper_aborting = false;
    if (check->aborted) return SENT_ABORTED;
    if (result != 0) {
        snprintf(error, cap, "whisper.cpp failed on it (%d)", result);
        return SENT_BAD;
    }
    i32 n = p_whisper_full_n_segments(ctx);
    i32 all_tokens = 0;
    for (i32 i = 0; i < n; i++) all_tokens += p_whisper_full_n_tokens(ctx, i);
    Heard* lines = PUSH_ARRAY(a, Heard, n + 1);
    HeardWord* words = PUSH_ARRAY(a, HeardWord, all_tokens + 1);
    i32 count = 0, word_count = 0;
    i32 eot = p_whisper_token_eot(ctx);
    f64 last_start = 0;
    for (i32 i = 0; i < n; i++) {
        i64 t0 = p_whisper_full_get_segment_t0(ctx, i), t1 = p_whisper_full_get_segment_t1(ctx, i);
        i32 tokens = p_whisper_full_n_tokens(ctx, i);
        i32 first_word = word_count;
        // The average log probability of its text tokens, as OpenAI's Whisper reports it.
        f64 total = 0;
        i64 before = -1;  // the aligned time of the text token before this one, if it is one
        for (i32 j = 0; j < tokens; j++) {
            whisper_token_data t = p_whisper_full_get_token_data(ctx, i, j);
            if (t.id >= eot) {
                before = -1;
                continue;
            }
            total += t.plog;
            Str piece = str(p_whisper_token_to_str(ctx, t.id));
            i64 end = t.t_dtw >= 0 ? t.t_dtw : t0;
            if (word_count == first_word || (piece.len && piece.ptr[0] == ' ')) {
                // A segment's first word has no token before it: it starts with the segment,
                // which may begin back where the last one ended; after a pause, not before the
                // speech does.
                i64 start = before >= 0 ? before : t0;
                i64 speech = speech_start(pieces, end);
                start = start > speech ? start : speech;
                start = start < end ? start : end;
                f64 s = file_time(pieces, start, true);
                s = s > last_start ? s : last_start;
                last_start = s;
                words[word_count++] = {str_copy(a, piece), s, s};
            } else {
                HeardWord* w = &words[word_count - 1];
                w->text = str_fmt(a, "%.*s%.*s", (int)w->text.len, w->text.ptr, (int)piece.len, piece.ptr);
            }
            HeardWord* w = &words[word_count - 1];
            f64 e = file_time(pieces, end, false);
            w->end = e > w->start ? e : w->start;
            before = t.t_dtw;
        }
        f64 logprob = tokens ? total / tokens : 0;
        // The line starts with its first word: the segment's own start is often back where the
        // one before it ended.
        f64 from = word_count > first_word ? words[first_word].start : file_time(pieces, t0, true);
        hear(a, lines, &count, &word_count, first_word, str(p_whisper_full_get_segment_text(ctx, i)), from,
             file_time(pieces, t1, false), p_whisper_full_get_segment_no_speech_prob(ctx, i), logprob);
    }
    out->lines = {lines, count};
    out->words = {words, word_count};
    return SENT_OK;
}

// With your own server: POST <url>/audio/transcriptions (OpenAI's API, which most transcription
// servers speak), reading the lines and their words out of the verbose_json answer. In `a`.
static Sent transcribe_there(Arena* a, Target* t, Str wav, Transcript* out, char* error, i32 cap) {
    *out = {};
    const char* boundary = "lifelog-3f9d2c7a51e8";
    Buf head = {};
    const char* fields[][2] = {{"model", t->model}, {"response_format", "verbose_json"}, {"temperature", "0"}, {"language", t->language},
                                {"timestamp_granularities[]", "segment"}, {"timestamp_granularities[]", "word"}};
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
    const Json* said = json_get(&j, "words");  // for the whole file, in order
    i32 n = json_len(segments), all_words = json_len(said);
    Heard* lines = PUSH_ARRAY(a, Heard, n + 1);
    HeardWord* words = PUSH_ARRAY(a, HeardWord, all_words + 1);
    i32 count = 0, word_count = 0, next_word = 0;
    for (i32 i = 0; i < n; i++) {
        const Json* s = json_at(segments, i);
        // A segment's words are those that start before the next segment does.
        f64 until = i + 1 < n ? json_get(json_at(segments, i + 1), "start")->num : INFINITY;
        i32 first_word = word_count;
        for (; next_word < all_words && json_get(json_at(said, next_word), "start")->num < until; next_word++) {
            const Json* w = json_at(said, next_word);
            words[word_count++] = {json_text(json_get(w, "word")), json_get(w, "start")->num, json_get(w, "end")->num};
        }
        hear(a, lines, &count, &word_count, first_word, json_text(json_get(s, "text")), json_get(s, "start")->num,
             json_get(s, "end")->num, json_get(s, "no_speech_prob")->num, json_get(s, "avg_logprob")->num);
    }
    // A server that gives only the text: one line for the whole file.
    if (!n) hear(a, lines, &count, &word_count, 0, json_text(json_get(&j, "text")), 0, json_get(&j, "duration")->num, 0, 0);
    out->lines = {lines, count};
    out->words = {words, word_count};
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


// A line's words as saved with it (segments.words): for each, four little-endian i32s, its text
// as bytes [from, to) of the line's text and its sound as samples [start, end) from the line's
// first sample. A word is found in the text by its own; one that is not there is left out.
static Str line_words(Arena* a, Heard* h, HeardWord* words, i32 rate, i64 from, i64 to) {
    i32* out = PUSH_ARRAY(a, i32, h->words * 4 + 1);
    i32 count = 0;
    i64 at = 0;  // in the text: words are found in order
    for (i32 i = 0; i < h->words; i++) {
        HeardWord* w = &words[h->first_word + i];
        Str text = str_trim(w->text);
        i64 found = text.len ? str_find(str_sub(h->text, at), text) : -1;
        if (found < 0) continue;
        i64 start = (i64)floor(w->start * rate + 0.5) - from, end = (i64)floor(w->end * rate + 0.5) - from;
        start = start < 0 ? 0 : start > to - from ? to - from : start;
        end = end < start ? start : end > to - from ? to - from : end;
        i32* q = out + count++ * 4;
        q[0] = (i32)(at + found);
        q[1] = (i32)(at + found + text.len);
        q[2] = (i32)start;
        q[3] = (i32)end;
        at += found + text.len;
    }
    return {(const char*)out, (i64)count * 16};
}

// Transcribes spool\<name> (with `ctx`, or your server `t` when ctx is null) and saves its lines.
// SENT_OK also when the file was set aside; on SENT_RETRY and SENT_ABORTED the file stays where
// it is. Not on the main thread.
static Sent transcribe_file(App* app, sqlite3* db, whisper_context* ctx, const WorkerOptions* o, Target* t, const wchar_t* spool,
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
    Transcript heard = {};
    bool mic = str_eq(json_text(json_get(&meta, "source")), str("mic"));
    Sent sent = ctx ? transcribe_here(app, a, ctx, o, mic, wav, &heard, error, sizeof(error))
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

    // Where the clip sits: its first sample's position at its rate (see capture.cpp). Clips spooled
    // before positions were written have only start_ts.
    const Json* rate_j = json_get(&meta, "sample_rate");
    i32 rate = rate_j->type == JSON_NUMBER && rate_j->num > 0 ? (i32)rate_j->num : 16000;
    const Json* start_j = json_get(&meta, "start");
    f64 start_ts = json_get(&meta, "start_ts")->num;
    i64 start = start_j->type == JSON_NUMBER ? (i64)start_j->num : (i64)floor(start_ts * rate + 0.5);
    i64 length = wav.len > WAV_HEADER ? (wav.len - WAV_HEADER) / 2 : 0;
    start_ts = (f64)start / rate;
    Str source = json_text(json_get(&meta, "source"));
    Str exe = json_text(json_get(&meta, "exe"));
    Str named = json_text(json_get(&meta, "speaker"));  // a person in a call, if the clip says
    Str speaker = str_eq(source, str("mic")) ? str("me") : named.len ? named : exe.len ? exe : str("unknown");
    // Kept under the calendar day it was recorded on, for playback.
    LocalTime lt = local_time(start_ts);
    Str kept = heard.lines.count ? str_fmt(a, "%04d-%02d-%02d/%s", lt.year, lt.month, lt.day, label.ptr) : Str{};

    if (heard.lines.count) {
        sqlite3_stmt* rec = 0;
        sqlite3_stmt* st = 0;
        bool saved = sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0) == SQLITE_OK &&
                     sqlite3_prepare_v2(db, "INSERT INTO recordings (file, sample_rate, start, length, source, speaker) VALUES (?, ?, ?, ?, ?, ?)",
                                        -1, &rec, 0) == SQLITE_OK &&
                     sqlite3_prepare_v2(db,
                                        "INSERT INTO segments (start_ts, end_ts, source, speaker, exe, pid, title, device, text, recording,"
                                        " start_sample, end_sample, words) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                        -1, &st, 0) == SQLITE_OK;
        if (saved) {
            bind_str(rec, 1, kept);
            sqlite3_bind_int(rec, 2, rate);
            sqlite3_bind_int64(rec, 3, start);
            sqlite3_bind_int64(rec, 4, length);
            bind_str(rec, 5, source);
            bind_str(rec, 6, speaker);
            saved = sqlite3_step(rec) == SQLITE_DONE;
        }
        i64 recording = sqlite3_last_insert_rowid(db);
        const Json* pid = json_get(&meta, "pid");
        for (i32 i = 0; saved && i < heard.lines.count; i++) {
            Heard* h = &heard.lines.items[i];
            i64 from = (i64)floor(h->start * rate + 0.5), to = (i64)floor(h->end * rate + 0.5);
            from = from < 0 ? 0 : from > length ? length : from;
            to = to < from ? from : to > length ? length : to;
            sqlite3_bind_double(st, 1, (f64)(start + from) / rate);
            sqlite3_bind_double(st, 2, (f64)(start + to) / rate);
            bind_str(st, 3, source);
            bind_str(st, 4, speaker);
            bind_opt(st, 5, exe);
            if (pid->type == JSON_NUMBER) sqlite3_bind_int64(st, 6, (i64)pid->num);
            else sqlite3_bind_null(st, 6);
            bind_opt(st, 7, json_text(json_get(&meta, "title")));
            bind_opt(st, 8, json_text(json_get(&meta, "device")));
            bind_str(st, 9, h->text);
            sqlite3_bind_int64(st, 10, recording);
            sqlite3_bind_int64(st, 11, from);
            sqlite3_bind_int64(st, 12, to);
            Str words = line_words(a, h, heard.words.items, rate, from, to);
            if (words.len) sqlite3_bind_blob(st, 13, words.ptr, (int)words.len, SQLITE_TRANSIENT);
            else sqlite3_bind_null(st, 13);
            saved = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_reset(st);
        }
        if (!saved) WARN("%s: saving its lines failed (%s); trying again later", label.ptr, sqlite3_errmsg(db));
        sqlite3_finalize(rec);
        sqlite3_finalize(st);
        saved = saved && sqlite3_exec(db, "COMMIT", 0, 0, 0) == SQLITE_OK;
        if (!saved) {
            sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
            rewind(m);
            return SENT_RETRY;
        }
        wchar_t audio[MAX_PATH], day_dir[MAX_PATH], to[MAX_PATH];
        audio_dir(audio);
        swprintf(day_dir, MAX_PATH, L"%ls\\%04d-%02d-%02d", audio, lt.year, lt.month, lt.day);
        SHCreateDirectoryExW(0, day_dir, 0);
        swprintf(to, MAX_PATH, L"%ls\\%ls", day_dir, name);
        if (!MoveFileExW(wav_path, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) DeleteFileW(wav_path);
    } else {
        DeleteFileW(wav_path);
    }
    DeleteFileW(meta_path);
    *lines_out = heard.lines.count;
    rewind(m);
    return SENT_OK;
}

// ---------------------------------------------------------------------------------------------
// The supervisor
//
// It reads atomics and its own copy of the options, and posts what it is doing to the main
// thread when that changes. It sleeps until worker_wake (a new WAV, a game, the GPU, the summary,
// a setting) or a timer it needs (a retry, the countdown after a game) says to look again.

// What the window shows of it: the supervisor's status record, posted through Worker.seen when
// it changes, so only the newest waits for the main thread.
struct WorkerSeen {
    char state[256];
    i32 queued;
    f64 queued_secs;
    bool running;  // transcribing is possible now: the model is loaded, or an endpoint is set and allowed
    bool missing;  // nothing to transcribe with: no endpoint, and whisper.cpp is not set up
    bool game;
};

// The main thread.
static void worker_seen_apply(App* app, Event* e) {
    Worker* w = &app->worker;
    WorkerSeen seen = latest_take<WorkerSeen>(e);
    w->queued = seen.queued;
    w->queued_secs = seen.queued_secs;
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
    Worker* w = &app->worker;
    bool moved = atomic_get(&w->live_queued) != now->queued || (atomic_get(&w->live_running) != 0) != now->running;
    atomic_set(&w->live_queued, now->queued);
    atomic_set(&w->live_running, now->running);
    if (moved) SetEvent(w->live_changed);
    if (memcmp(now, last, sizeof(WorkerSeen)) == 0) return;
    *last = *now;
    latest_send(app, &app->worker.seen, worker_seen_apply, *now);
}

static DWORD WINAPI supervisor_thread(void* p) {
    App* app = (App*)p;
    wchar_t spool[MAX_PATH];
    spool_dir(spool);
    sqlite3* db = 0;
    whisper_context* ctx = 0;  // our model, while it may use the GPU
    char ctx_model[128] = "";

    f64 next_load = 0;   // after a failed load, wait before trying again
    f64 retry_at = 0;    // after your server could not be reached
    f64 quiet_from = 0;  // when the last game closed, in auto mode; 0 if none has
    bool had_game = false, first = true, count = true;
    bool looked = false;  // for the GPU Whisper runs on, before its model first loads
    char trouble[256] = "";  // why the last try failed, while waiting to try again
    WorkerOptions o = {};
    LONG version = 0;
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
        char state[256];
        Target t = {};
        worker_options_take(app, &o, &version);
        const WorkerOptions* c = &o;
        const char* model = c->model;
        bool yours = c->endpoint[0] != 0;
        bool ready = yours || whisper_ready(c->ai_dir, c->model);
        if (ready == seen.missing || first) {
            if (!ready) WARN("transcription is not set up (Settings, Transcription): audio will queue up untranscribed");
            else if (!first) LOG("transcription is set up");
        }
        seen.missing = !ready;
        first = false;
        if (ready && !yours && !looked) {
            // Which GPU to watch, so load on another one does not hold transcription up. By name
            // only: nothing is put on the GPU yet.
            looked = true;
            wchar_t bins[MAX_PATH];
            char error[512];
            whisper_bin_dir(c->ai_dir, bins);
            if (whisper_load(bins, error, sizeof(error))) whisper_adapter_publish(app, false);
        }
        bool game = c->pause_for_games && atomic_get(&app->apps.game_on);
        if (had_game && !game) quiet_from = now;
        had_game = game;
        bool want = ready && worker_allowed(app, c, quiet_from, state, sizeof(state));
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
                Str url = str_trim(str(c->endpoint));
                while (url.len && url.ptr[url.len - 1] == '/') url.len--;
                SET(t.url, url);
                SET(t.key, str_trim(str(c->api_key)));
                SET(t.model, str(model));
                SET(t.language, str(c->language));
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
            ticking = quiet_from > 0 && now - quiet_from < (f64)c->resume_after_secs + 1;
        }
        SET(seen.state, str(state));
        seen.game = strcmp(state, GAME_STATE) == 0;
        seen.running = can;
        worker_post_seen(app, &seen, &last);

        if (unload) {
            p_whisper_free(ctx);
            ctx = 0;
            ctx_model[0] = 0;
        }
        if (!ctx) SetEvent(app->worker.gpu_free);  // the summary may have the GPU
        if (load) {
            // The GPU is taken before the summary's flag is read a last time: a summary that asks
            // after this waits for the model to load and be freed again.
            ResetEvent(app->worker.gpu_free);
            if (atomic_get(&app->worker.summary_needs_gpu)) {
                SetEvent(app->worker.gpu_free);
                continue;
            }
            char error[512];
            ctx = whisper_open(c->ai_dir, model, error, sizeof(error));
            if (ctx) {
                SET(ctx_model, str(model));
                whisper_adapter_publish(app, true);
            } else {
                SetEvent(app->worker.gpu_free);
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
            Sent sent = transcribe_file(app, db, yours ? 0 : ctx, c, &t, spool, name, &lines);
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
    app->worker.gpu_free = CreateEventW(0, TRUE, TRUE, 0);  // no model yet
    app->worker.live_changed = CreateEventW(0, FALSE, FALSE, 0);
    CHECK(app->worker.done && app->worker.gpu_free && app->worker.live_changed, "the worker's events (error %lu)", GetLastError());
    // Made in run_app, before capture starts: the capture threads wake the supervisor.
    CHECK(app->worker.wake, "the worker's wake event made before worker_start");
    app->worker.options_lock = SRWLOCK_INIT;
    worker_options_publish(app);
    start_thread(app, "worker-supervisor", supervisor_thread);
    start_thread(app, "gpu-monitor", gpu_monitor_thread);
}

void worker_options_publish(App* app) {
    const Config* c = &app->config;
    Worker* w = &app->worker;
    AcquireSRWLockExclusive(&w->options_lock);
    WorkerOptions* o = &w->options;
    o->mode = c->processing.mode;
    o->gpu_busy_percent = c->processing.gpu_busy_percent;
    o->pause_for_games = c->processing.pause_for_games;
    o->resume_after_secs = c->processing.resume_after_secs;
    SET(o->model, str(c->worker.model));
    SET(o->language, str(c->worker.language));
    SET(o->endpoint, str(c->worker.endpoint));
    SET(o->api_key, str(c->worker.api_key));
    SET(o->ai_dir, str(c->ai.dir));
    InterlockedIncrement(&w->options_version);
    ReleaseSRWLockExclusive(&w->options_lock);
    worker_wake(app);
}

void worker_set_mode(App* app, Mode mode) {
    if (app->config.processing.mode == mode) return;
    app->config.processing.mode = mode;
    LOG("transcription mode: %s", mode_name(mode));
    config_save_string("processing", "mode", mode_name(mode));
    worker_options_publish(app);
}

void worker_set_gpu_limit(App* app, u32 percent) {
    percent = percent < 1 ? 1 : percent > 100 ? 100 : percent;
    // The slider calls this every frame it is dragged; only a change is saved.
    if (app->config.processing.gpu_busy_percent == percent) return;
    app->config.processing.gpu_busy_percent = percent;
    LOG("a game is running when 3D load is above %u%%", percent);
    config_save_u32("processing", "gpu_busy_percent", percent);
    worker_options_publish(app);
}

void worker_set_pause_for_games(App* app, bool on) {
    if (app->config.processing.pause_for_games == on) return;
    app->config.processing.pause_for_games = on;
    LOG("auto mode %s while a game runs", on ? "waits" : "doesn't wait");
    config_save_bool("processing", "pause_for_games", on);
    worker_options_publish(app);
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
    worker_options_publish(app);
}

void worker_wake(App* app) {
    if (app->worker.wake) SetEvent(app->worker.wake);
}

void worker_stop(App* app) {
    // The supervisor sees `stop`, cuts the clip it is on short and frees the model; a GPU in use
    // while the process ends can hang it.
    if (app->worker.done && WaitForSingleObject(app->worker.done, 15000) != WAIT_OBJECT_0) WARN("transcription did not stop in 15 s");
}
