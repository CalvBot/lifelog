// worker.cpp: see worker.h. Two threads:
//   - the supervisor decides whether transcription may run, starts and stops our whisper-server,
//     counts the queue and sends the files, oldest first;
//   - the GPU monitor samples the 3D engine's load every 2 s.

#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")

// ---------------------------------------------------------------------------------------------
// GPU load, from the counters Task Manager uses. Games load the 3D engine; Whisper runs on the
// CUDA engine, so our own work is not counted.

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

// Percent of the 3D engine in use since the last call, summed over every program (so it can
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
    f64 sum = 0;
    for (DWORD i = 0; ok && i < count; i++)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) sum += items[i].FmtValue.doubleValue;
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
        if (!gpu3d_sample(&gpu, &load)) continue;  // sampled unlocked: it can take a while
        app_lock(app);
        Worker* w = &app->worker;
        bool was_busy = w->gpu_busy;
        u32 percent = (u32)(load + 0.5);
        bool changed = percent != w->gpu_3d_percent;
        w->gpu_3d_percent = percent;
        if (w->summary_needs_gpu) {
            over = 0;  // the summary model's own load shows up as 3D work too; it is not a game
        } else if (load > (f64)app->config.processing.gpu_busy_percent) {
            over++;
            last_over = now_ts();
            if (over >= 3 && !w->gpu_busy) {
                w->gpu_busy = true;
                LOG("GPU busy (%.0f%% 3D): transcription waits, audio keeps queueing", load);
            }
        } else {
            over = 0;
            if (w->gpu_busy && now_ts() - last_over >= (f64)app->config.processing.resume_after_secs) {
                w->gpu_busy = false;
                LOG("GPU free again: transcription resumes");
            }
        }
        changed |= was_busy != w->gpu_busy;
        app_unlock(app);
        if (changed) wake_window(app);
    }
    PdhCloseQuery(gpu.query);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Our whisper.cpp server

// Only lifelog talks to it. A test copy (LIFELOG_CONFIG set) uses another port, so it never meets
// the installed lifelog's server.
static u16 whisper_port() { return GetEnvironmentVariableW(L"LIFELOG_CONFIG", 0, 0) ? 47812 : 47802; }

struct Server {
    HANDLE process;
    HANDLE job;
};

// Takes the server out of App.worker (the caller holds the lock); whoever does, stops it.
static Server server_take(Worker* w) {
    Server s = {w->process, w->job};
    w->process = w->job = 0;
    return s;
}

static void server_stop(Server* s) {
    if (s->process) {
        TerminateProcess(s->process, 0);  // it holds nothing to save: a file it had stays queued
        WaitForSingleObject(s->process, 10000);
        CloseHandle(s->process);
    }
    if (s->job) CloseHandle(s->job);
    *s = {};
}

// Starts whisper-server with the model `model` (ggml-<model>.bin) and waits until it answers.
// What it prints goes to whisper.log next to lifelog.log. Unlocked.
static bool server_start(App* app, const char* model, const char* language, Server* out, char* error, i32 cap) {
    *out = {};
    wchar_t dir[MAX_PATH], exe[MAX_PATH], bin[MAX_PATH], vad[MAX_PATH], log[MAX_PATH];
    whisper_dir(dir);
    swprintf(exe, MAX_PATH, L"%ls\\Release\\whisper-server.exe", dir);
    swprintf(bin, MAX_PATH, L"%ls\\ggml-%hs.bin", dir, model);
    swprintf(vad, MAX_PATH, L"%ls\\%hs", dir, WHISPER_VAD_MODEL);
    swprintf(log, MAX_PATH, L"%ls\\whisper.log", local_dir());
    u16 port = whisper_port();
    // Beam search; no text carried over from the last segment (one bad line does not repeat into
    // the next); Silero voice detection skips music, game noise and silence.
    wchar_t cmd[4 * MAX_PATH];
    swprintf(cmd, COUNT(cmd),
             L"\"%ls\" -m \"%ls\" --host 127.0.0.1 --port %u -l %hs -bs 5 -mc 0 --vad -vm \"%ls\" -vsd 500 "
             L"--inference-path /v1/audio/transcriptions",
             exe, bin, (unsigned)port, language[0] ? language : "auto", vad);

    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), 0, TRUE};
    HANDLE out_log = CreateFileW(log, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, 0, 0);
    // Only the log goes to the child, not whatever else is inheritable in our process.
    Mark m = mark(scratch());
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(0, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)arena_push(scratch(), attr_size, 16);
    bool have_log = out_log != INVALID_HANDLE_VALUE;
    bool ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
              (!have_log || UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out_log, sizeof(out_log), 0, 0));
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    if (have_log) {
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = out_log;
        si.StartupInfo.hStdError = out_log;
    }
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi = {};
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    if (ok) ok = CreateProcessW(exe, cmd, 0, 0, have_log, flags, 0, dir, &si.StartupInfo, &pi) != 0;
    DWORD started_error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    rewind(m);
    if (have_log) CloseHandle(out_log);
    if (!ok) {
        snprintf(error, cap, "could not start whisper-server (error %lu)", started_error);
        return false;
    }
    // A job that Windows closes, ending the server, when lifelog's handle goes.
    HANDLE job = CreateJobObjectW(0, 0);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, pi.hProcess))
        WARN("could not tie whisper-server to lifelog (error %lu)", GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    out->process = pi.hProcess;
    out->job = job;
    LOG("whisper-server started on port %u with %s", (unsigned)port, model);

    char health[96];
    snprintf(health, sizeof(health), "http://127.0.0.1:%u/health", (unsigned)port);
    f64 began = now_ts();
    for (;;) {
        DWORD code = 0;
        if (GetExitCodeProcess(out->process, &code) && code != STILL_ACTIVE) {
            snprintf(error, cap, "whisper-server stopped while loading (exit code %lu): see whisper.log in %ls", code, local_dir());
            server_stop(out);
            return false;
        }
        Mark hm = mark(scratch());
        HttpReply reply = {};
        char ignored[128];
        bool up = http_call(scratch(), "GET", str(health), {}, {}, 2000, &reply, ignored, sizeof(ignored)) && reply.status == 200;
        rewind(hm);
        if (up) {
            LOG("whisper-server ready in %.1f s", now_ts() - began);
            return true;
        }
        if (now_ts() - began > 120) {
            snprintf(error, cap, "whisper-server did not load the model in 2 minutes");
            server_stop(out);
            return false;
        }
        if (!sleep_unless_stopping(app, 300)) {
            snprintf(error, cap, "lifelog is closing");
            server_stop(out);
            return false;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Transcribing one file

// Where a file goes: our server or yours. Copied under the lock, used unlocked.
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

enum Sent : u8 {
    SENT_OK,
    SENT_RETRY,  // the server could not be reached or is busy: keep the file and try again later
    SENT_BAD,    // the server refused this file: set it aside
};

// Sends `wav` (a whole .wav file) to POST <url>/audio/transcriptions, the OpenAI API both our
// server and others speak, and reads the lines out of the verbose_json answer. In `a`.
static Sent transcribe(Arena* a, Target* t, Str wav, Slice<Heard>* out, char* error, i32 cap) {
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
    // Long clips take a while on a slow GPU, or on the CPU.
    if (!http_call(a, "POST", url, headers, {body, len}, 10 * 60 * 1000, &reply, why, sizeof(why))) {
        snprintf(error, cap, "could not reach %s: %s", t->url, why);
        return SENT_RETRY;
    }
    if (reply.status == 429 || reply.status >= 500) {
        snprintf(error, cap, "%s answered %u: %.200s", t->url, reply.status, reply.body.ptr);
        return SENT_RETRY;
    }
    if (reply.status != 200) {
        snprintf(error, cap, "%s answered %u: %.200s", t->url, reply.status, reply.body.ptr);
        return SENT_BAD;
    }
    Json j = {};
    char parse_error[128];
    if (!json_parse(a, reply.body, &j, parse_error, sizeof(parse_error))) {
        snprintf(error, cap, "the answer is not JSON (%s): %.200s", parse_error, reply.body.ptr);
        return SENT_BAD;
    }
    const Json* segments = json_get(&j, "segments");
    i32 n = json_len(segments);
    Heard* lines = PUSH_ARRAY(a, Heard, n + 1);
    i32 count = 0;
    for (i32 i = 0; i < n; i++) {
        const Json* s = json_at(segments, i);
        Str raw = json_text(json_get(s, "text"));
        Str text = str_trim(raw);
        f64 from = json_get(s, "start")->num, to = json_get(s, "end")->num;
        if (!text.len) continue;
        // A segment that starts inside a word ("don" + "'t want it") goes on the end of the last.
        if (count && raw.ptr[0] != ' ' && from - lines[count - 1].end < 1.0) {
            Heard* last = &lines[count - 1];
            last->text = str_fmt(a, "%.*s%.*s", (int)last->text.len, last->text.ptr, (int)text.len, text.ptr);
            last->end = to;
            continue;
        }
        if (hallucination(text)) continue;
        if (json_get(s, "no_speech_prob")->num > 0.6 && json_get(s, "avg_logprob")->num < -1.0) continue;
        lines[count++] = {from, to, text};
    }
    // A server that gives only the text: one line for the whole file.
    if (!n) {
        Str text = str_trim(json_text(json_get(&j, "text")));
        if (text.len && !hallucination(text)) lines[count++] = {0, json_get(&j, "duration")->num, text};
    }
    *out = {lines, count};
    return SENT_OK;
}

// The oldest WAV in the spool folder (names start with the time in ms). False if there is none.
static bool oldest_file(const wchar_t* spool, wchar_t name[MAX_PATH]) {
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.wav", spool);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &f, FindExSearchNameMatch, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    name[0] = 0;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!name[0] || wcscmp(f.cFileName, name) < 0) wcscpy_s(name, MAX_PATH, f.cFileName);
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return name[0] != 0;
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

static void bind_text(sqlite3_stmt* st, int i, Str s) {
    if (s.len) sqlite3_bind_text(st, i, s.ptr, (int)s.len, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, i);
}

// Transcribes spool\<name> and saves its lines. False when it should be tried again later (the
// server could not be reached, or the database was busy); the file then stays where it is.
// Unlocked.
static bool transcribe_file(App* app, sqlite3* db, Target* t, const wchar_t* spool, const wchar_t* name, i32* lines_out) {
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
        return true;
    }
    Slice<Heard> heard = {};
    Sent sent = transcribe(a, t, wav, &heard, error, sizeof(error));
    if (sent == SENT_RETRY) {
        if (!stopping(app)) WARN("%s: %s; trying again later", label.ptr, error);
        rewind(m);
        return false;
    }
    if (sent == SENT_BAD) {
        WARN("%s: %s; set aside in spool\\failed", label.ptr, error);
        set_aside(spool, wav_path, meta_path);
        rewind(m);
        return true;
    }

    f64 start_ts = json_get(&meta, "start_ts")->num;
    Str source = json_text(json_get(&meta, "source"));
    Str exe = json_text(json_get(&meta, "exe"));
    Str named = json_text(json_get(&meta, "speaker"));  // a person in a call, if the clip says
    Str speaker = str_eq(source, str("mic")) ? str("me") : named.len ? named : exe.len ? exe : str("unknown");
    // Kept under the calendar day it was recorded on, for playback.
    LocalTime lt = local_time(start_ts);
    Str kept = heard.count ? str_fmt(a, "%04d-%02d-%02d/%s", lt.year, lt.month, lt.day, label.ptr) : Str{};

    bool saved = true;
    if (heard.count) {
        sqlite3_stmt* st = 0;
        saved = sqlite3_exec(db, "BEGIN IMMEDIATE", 0, 0, 0) == SQLITE_OK &&
                sqlite3_prepare_v2(db,
                                   "INSERT INTO segments (start_ts, end_ts, source, speaker, exe, pid, title, device, text, audio, audio_offset)"
                                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                   -1, &st, 0) == SQLITE_OK;
        for (i32 i = 0; saved && i < heard.count; i++) {
            Heard* h = &heard.items[i];
            const Json* pid = json_get(&meta, "pid");
            sqlite3_bind_double(st, 1, start_ts + h->start);
            sqlite3_bind_double(st, 2, start_ts + h->end);
            bind_text(st, 3, source);
            bind_text(st, 4, speaker);
            bind_text(st, 5, exe);
            if (pid->type == JSON_NUMBER) sqlite3_bind_int64(st, 6, (i64)pid->num);
            else sqlite3_bind_null(st, 6);
            bind_text(st, 7, json_text(json_get(&meta, "title")));
            bind_text(st, 8, json_text(json_get(&meta, "device")));
            bind_text(st, 9, h->text);
            bind_text(st, 10, kept);
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
            return false;
        }
    }
    if (heard.count) {
        wchar_t audio[MAX_PATH], day_dir[MAX_PATH], to[MAX_PATH];
        audio_dir(&app->config, audio);  // data_dir does not change while the program runs
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
    return true;
}

// ---------------------------------------------------------------------------------------------
// The supervisor

// WAV files in the spool folder and how long they are (16 kHz mono 16-bit, after a 44-byte header).
static void count_queue(const wchar_t* spool, i32* files, f64* secs) {
    *files = 0;
    *secs = 0;
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.wav", spool);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        u64 size = ((u64)f.nFileSizeHigh << 32) | f.nFileSizeLow;
        (*files)++;
        if (size > 44) *secs += (f64)(size - 44) / 32000.0;
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

// Whether transcription may run now, and the few words saying why (not). Caller holds the lock.
static bool worker_allowed(App* app, f64 quiet_from, char* state, i32 cap) {
    Worker* w = &app->worker;
    Mode mode = app->config.processing.mode;
    f64 wait = (f64)app->config.processing.resume_after_secs - (now_ts() - quiet_from);
    if (w->summary_needs_gpu) {
        snprintf(state, cap, "waiting for the daily summary");
    } else if (mode == MODE_PAUSED) {
        snprintf(state, cap, "paused");
    } else if (mode == MODE_AUTO && app->apps.game_running[0]) {
        Str game = str(app->apps.game_running);
        if (game.len > 4 && _stricmp(game.ptr + game.len - 4, ".exe") == 0) game.len -= 4;
        snprintf(state, cap, "waiting for %.*s to close", (int)game.len, game.ptr);
    } else if (mode == MODE_AUTO && w->gpu_busy) {
        snprintf(state, cap, "waiting for the graphics card");
    } else if (mode == MODE_AUTO && quiet_from > 0 && wait > 0) {
        snprintf(state, cap, "the game has closed: starting in %.0f s", wait);
    } else {
        return true;
    }
    return false;
}

static DWORD WINAPI supervisor_thread(void* p) {
    App* app = (App*)p;
    wchar_t spool[MAX_PATH];
    app_lock(app);
    spool_dir(&app->config, spool);
    app_unlock(app);
    sqlite3* db = 0;

    f64 next_start = 0;  // after a crash or a failed start, wait before trying again
    f64 next_count = 0;
    f64 retry_at = 0;    // after the server could not be reached
    f64 quiet_from = 0;  // when the last game closed, in auto mode; 0 if none has
    bool had_game = false, first = true;
    char trouble[256] = "";  // why the last try failed, while waiting to try again
    while (!stopping(app)) {
        f64 now = now_ts();
        i32 files = -1;
        f64 secs = 0;
        if (now >= next_count) {  // the folder is read unlocked; the data drive may be slow
            count_queue(spool, &files, &secs);
            next_count = now + 2;
        }

        Server stop = {};
        bool start = false, work = false, wake = false;
        char state[256], model[128], language[16];
        Target t = {};
        app_lock(app);
        Worker* w = &app->worker;
        Config* c = &app->config;
        bool yours = c->worker.endpoint[0] != 0;
        bool ready = yours || whisper_ready(c->worker.model);
        if (ready == w->missing || first) {
            w->missing = !ready;
            wake = true;
            if (!ready) WARN("transcription is not set up (Settings, Transcription): audio will queue up untranscribed");
            else if (!first) LOG("transcription is set up");
        }
        first = false;
        bool game = app->apps.game_running[0] != 0;
        if (had_game && !game) quiet_from = now;
        had_game = game;
        if (files >= 0 && (files != w->queued || fabs(secs - w->queued_secs) > 0.5)) {
            w->queued = files;
            w->queued_secs = secs;
            wake = true;
        }
        bool want = ready && worker_allowed(app, quiet_from, state, sizeof(state));
        if (w->process && (!want || yours)) {
            LOG("stopping whisper-server (%s)", want ? "your own server is set" : state);
            stop = server_take(w);
        } else if (w->process && WaitForSingleObject(w->process, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(w->process, &code);
            WARN("whisper-server exited (code %lu): see whisper.log; starting it again in 15 s", code);
            stop = server_take(w);
            next_start = now + 15;
        }
        if (want && !yours && !w->process && now >= next_start) {
            start = true;
            SET(model, str(c->worker.model));
            SET(language, str(c->worker.language));
        }
        bool can = want && (yours || w->process);
        if (can && w->queued > 0 && now >= retry_at) {
            work = true;
            if (yours) {
                Str url = str_trim(str(c->worker.endpoint));
                while (url.len && url.ptr[url.len - 1] == '/') url.len--;
                SET(t.url, url);
                SET(t.key, str_trim(str(c->worker.api_key)));
            } else {
                snprintf(t.url, sizeof(t.url), "http://127.0.0.1:%u/v1", (unsigned)whisper_port());
            }
            SET(t.model, str(c->worker.model));
            SET(t.language, str(c->worker.language));
        }
        if (want) {
            if (now < retry_at) snprintf(state, sizeof(state), "%s: trying again in %.0f s", trouble, retry_at - now);
            else if (can) snprintf(state, sizeof(state), w->queued ? "transcribing" : "up to date");
            else snprintf(state, sizeof(state), "starting");
            w->waiting_since = 0;
        } else {
            if (!ready) snprintf(state, sizeof(state), "not set up yet");
            if (!w->waiting_since) w->waiting_since = now;
        }
        if (strcmp(state, w->state) != 0) {
            SET(w->state, str(state));
            wake = true;
        }
        wake |= can != w->running;
        w->running = can;
        app_unlock(app);
        if (wake) wake_window(app);

        if (stop.process) server_stop(&stop);
        if (start) {
            Server s = {};
            char error[512];
            bool ok = server_start(app, model, language, &s, error, sizeof(error));
            if (!ok && !stopping(app)) WARN("%s; trying again in a minute", error);
            app_lock(app);
            if (ok && !stopping(app)) {
                app->worker.process = s.process;
                app->worker.job = s.job;
            } else if (!ok) {
                next_start = now_ts() + 60;
            }
            app_unlock(app);
            if (ok && stopping(app)) server_stop(&s);  // the program ended while it started
            continue;
        }
        if (work) {
            wchar_t name[MAX_PATH];
            if (!oldest_file(spool, name)) {
                next_count = 0;  // the count was out of date
                continue;
            }
            if (!db) db = db_open(app);
            if (!db) {
                snprintf(trouble, sizeof(trouble), "the database could not be opened");
                retry_at = now_ts() + 30;
                continue;
            }
            f64 began = now_ts();
            i32 lines = 0;
            if (!transcribe_file(app, db, &t, spool, name, &lines)) {
                // Our server going away mid-file is handled above; anything else waits a while.
                snprintf(trouble, sizeof(trouble), "could not transcribe");
                retry_at = now_ts() + 30;
                continue;
            }
            i32 left = 0;
            f64 left_secs = 0;
            count_queue(spool, &left, &left_secs);
            next_count = now_ts() + 2;
            LOG("%ls: %d lines in %.1fs (%d queued)", name, lines, now_ts() - began, left);
            app_lock(app);
            app->worker.queued = left;
            app->worker.queued_secs = left_secs;
            if (lines > 0) app->worker.lines_version++;
            app_unlock(app);
            wake_window(app);
            continue;
        }
        sleep_unless_stopping(app, 500);
    }
    if (db) sqlite3_close(db);
    // A running server is left for worker_stop, which the main thread calls before the program exits.
    return 0;
}

// ---------------------------------------------------------------------------------------------
// worker.h

void worker_start(App* app) {
    Config* c = &app->config;
    if (c->processing.gpu_busy_percent < 1) c->processing.gpu_busy_percent = 1;
    if (c->processing.gpu_busy_percent > 100) c->processing.gpu_busy_percent = 100;
    SET(app->worker.state, str("starting"));
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

void worker_stop(App* app) {
    Server s = server_take(&app->worker);
    app->worker.running = false;
    if (s.process) {
        server_stop(&s);
        LOG("whisper-server stopped");
    }
}
