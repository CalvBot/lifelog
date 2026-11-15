// worker.cpp: see worker.h. Three threads:
//   - the supervisor decides whether the worker may run, starts and stops it, and counts the queue;
//   - the GPU monitor samples the 3D engine's load every 2 s;
//   - one reader per worker process copies its output into the log and spots finished files.

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
// The worker's output: each line goes to our log, and "x.wav: 3 lines in 1.2s (4 queued)" tells
// the timeline to reload.

struct Reader {
    App* app;
    HANDLE pipe;
};

static void reader_line(App* app, char* line, i64 len) {
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;
    line[len] = 0;
    Str s = str_trim({line, len});
    if (!s.len) return;
    // Python's own "HH:MM:SS [worker] " goes; our log has its own time and the tag is added back.
    if (s.len > 9 && s.ptr[2] == ':' && s.ptr[5] == ':' && s.ptr[8] == ' ') s = str_sub(s, 9);
    if (str_starts(s, str("[worker] "))) s = str_sub(s, 9);
    LOG("[worker] %.*s", (int)s.len, s.ptr);

    i64 at = str_find(s, str(" lines in "));
    if (at < 0) return;
    i64 colon = at;
    while (colon > 0 && s.ptr[colon - 1] != ':') colon--;
    i32 lines = 0, queued = 0;
    f64 secs = 0;
    if (colon == 0 || sscanf_s(s.ptr + colon, " %d", &lines) != 1) return;
    if (sscanf_s(s.ptr + at, " lines in %lfs (%d queued)", &secs, &queued) != 2) return;
    app_lock(app);
    app->worker.queued = queued;
    if (lines > 0) app->worker.lines_version++;
    app_unlock(app);
    if (lines > 0) wake_window(app);}

static DWORD WINAPI reader_thread(void* p) {
    Reader r = *(Reader*)p;
    free(p);
    SetThreadDescription(GetCurrentThread(), L"worker-output");
    char buf[4096];
    i64 used = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(r.pipe, buf + used, (DWORD)(sizeof(buf) - 1 - used), &got, nullptr) || got == 0) break;
        used += got;
        i64 start = 0;
        for (i64 i = 0; i < used; i++) {
            if (buf[i] != '\n') continue;
            reader_line(r.app, buf + start, i - start);
            start = i + 1;
        }
        memmove(buf, buf + start, (size_t)(used - start));
        used -= start;
        if (used == (i64)sizeof(buf) - 1) {  // a line too long for the buffer: log it in pieces
            reader_line(r.app, buf, used);
            used = 0;
        }
    }
    if (used) reader_line(r.app, buf, used);
    CloseHandle(r.pipe);  // the process ended (EOF) or the pipe broke
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Starting and stopping the process

struct Child {
    HANDLE process;
    HANDLE stdin_write;
    HANDLE job;
};

// The worker to run: from the config, else worker\.venv in the repo the exe was built from
// (<repo>\app\build\lifelog.exe) or next to it, else the environment Settings sets up (setup.h).
static bool find_worker(Config* cfg, wchar_t python[MAX_PATH], wchar_t dir[MAX_PATH]) {
    Mark m = mark(scratch());
    bool found = false;
    if (cfg->worker.python[0]) {
        wcsncpy_s(python, MAX_PATH, wide(scratch(), str(cfg->worker.python)), _TRUNCATE);
        wcsncpy_s(dir, MAX_PATH, wide(scratch(), str(cfg->worker.dir)), _TRUNCATE);
        found = true;
    }
    const wchar_t* places[] = {L"%ls\\worker", L"%ls\\..\\..\\worker"};
    for (i32 i = 0; !found && i < COUNT(places); i++) {
        wchar_t rel[MAX_PATH], py[MAX_PATH];
        swprintf(rel, MAX_PATH, places[i], exe_dir());
        if (!GetFullPathNameW(rel, MAX_PATH, dir, nullptr)) continue;
        swprintf(py, MAX_PATH, L"%ls\\.venv\\Scripts\\python.exe", dir);
        if (GetFileAttributesW(py) == INVALID_FILE_ATTRIBUTES) continue;
        wcscpy_s(python, MAX_PATH, py);
        found = true;
    }
    if (!found) {
        transcriber_python(python);
        found = GetFileAttributesW(python) != INVALID_FILE_ATTRIBUTES && worker_source(dir);
    }
    rewind(m);
    return found;
}

// Our environment plus UTF-8 output from Python, so the log is not in the console's code page.
static wchar_t* child_environment(Arena* a) {
    const wchar_t* add[] = {L"PYTHONIOENCODING=utf-8", L"PYTHONUNBUFFERED=1"};
    wchar_t* env = GetEnvironmentStringsW();
    i64 len = 0;
    while (env[len]) len += (i64)wcslen(env + len) + 1;
    i64 cap = len + 128;
    wchar_t* out = PUSH_ARRAY(a, wchar_t, cap);
    i64 at = 0;
    for (const wchar_t* e = env; *e; e += wcslen(e) + 1) {
        if (_wcsnicmp(e, L"PYTHONIOENCODING=", 17) == 0 || _wcsnicmp(e, L"PYTHONUNBUFFERED=", 17) == 0) continue;
        i64 n = (i64)wcslen(e) + 1;
        memcpy(out + at, e, (size_t)n * sizeof(wchar_t));
        at += n;
    }
    for (i32 i = 0; i < COUNT(add); i++) {
        i64 n = (i64)wcslen(add[i]) + 1;
        memcpy(out + at, add[i], (size_t)n * sizeof(wchar_t));
        at += n;
    }
    out[at] = 0;  // the block ends with an empty string
    FreeEnvironmentStringsW(env);
    return out;
}

static bool child_start(App* app, const wchar_t* python, const wchar_t* dir, Child* out) {
    *out = {};
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE in_read = 0, in_write = 0, err_read = 0, err_write = 0;
    // A big output pipe, so the worker never blocks on a log line while we wait for the lock.
    if (!CreatePipe(&in_read, &in_write, &inherit, 0)) return false;
    if (!CreatePipe(&err_read, &err_write, &inherit, 64 * 1024)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        return false;
    }
    // Our ends stay ours: if a child kept the stdin write end, the worker would never see it close.
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    Arena* a = scratch();
    Mark m = mark(a);
    wchar_t* cmd = PUSH_ARRAY(a, wchar_t, 3 * MAX_PATH);
    swprintf(cmd, 3 * MAX_PATH, L"\"%ls\" -m lifelog_worker --config \"%ls\"", python, config_path());

    // Only the two pipe ends go to the child, not whatever else is inheritable in our process.
    HANDLE pass[] = {in_read, err_write};
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)arena_push(a, attr_size, 16);
    bool ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, pass, sizeof(pass), nullptr, nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = err_write;
    si.StartupInfo.hStdError = err_write;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi = {};
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    DWORD error = 0;
    if (ok) {
        ok = CreateProcessW(python, cmd, nullptr, nullptr, TRUE, flags, child_environment(a), dir[0] ? dir : nullptr,
                            &si.StartupInfo, &pi);
        error = GetLastError();
    }
    DeleteProcThreadAttributeList(attrs);
    rewind(m);
    CloseHandle(in_read);  // the child has its copies now
    CloseHandle(err_write);
    if (!ok) {
        ERR("could not start the worker %ls (error %lu)", python, error);
        CloseHandle(in_write);
        CloseHandle(err_read);
        return false;
    }

    // A job holds the whole tree: the venv's python.exe is a launcher that starts the real one,
    // and killing only the launcher would leave the real one running. Closing the job kills it,
    // so the worker also ends if lifelog dies.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    if (!job || !AssignProcessToJobObject(job, pi.hProcess))
        WARN("worker not in a job (error %lu): it may outlive a crash", GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    Reader* r = (Reader*)calloc(1, sizeof(Reader));
    CHECK(r, "memory for the worker's output reader");
    r->app = app;
    r->pipe = err_read;
    HANDLE t = CreateThread(nullptr, 0, reader_thread, r, 0, nullptr);
    CHECK(t, "the worker's output reader to start (error %lu)", GetLastError());
    CloseHandle(t);

    out->process = pi.hProcess;
    out->stdin_write = in_write;
    out->job = job;
    LOG("worker started (pid %lu)", pi.dwProcessId);
    return true;
}

// Asks the worker to finish its current file and exit (it exits when its stdin closes), then
// makes sure. Slow: call it without the lock where you can.
static void child_stop(Child* c) {
    if (c->stdin_write) CloseHandle(c->stdin_write);
    if (c->process && WaitForSingleObject(c->process, 20000) != WAIT_OBJECT_0) {
        WARN("worker did not exit within 20 s; ending it");
        if (c->job) TerminateJobObject(c->job, 1);
        else TerminateProcess(c->process, 1);
        WaitForSingleObject(c->process, 5000);
    }
    if (c->process) CloseHandle(c->process);
    if (c->job) CloseHandle(c->job);  // kills anything left in it
    *c = {};
}

// Takes the process out of App.worker (the caller holds the lock); whoever does, stops it.
static Child child_take(Worker* w) {
    Child c = {w->process, w->stdin_write, w->job};
    w->process = w->stdin_write = w->job = 0;
    return c;
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

// Whether the worker may run now, and the few words saying why (not). Caller holds the lock.
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
    wchar_t python[MAX_PATH] = {}, dir[MAX_PATH] = {}, spool[MAX_PATH];
    app_lock(app);
    bool found = find_worker(&app->config, python, dir);
    app->worker.missing = !found;
    spool_dir(&app->config, spool);
    app_unlock(app);
    if (!found) WARN("the transcriber is not set up (Settings, Transcription): audio will queue up untranscribed");

    f64 next_start = 0;  // after a crash or a failed start, wait before trying again
    f64 next_count = 0;
    f64 quiet_from = 0;  // when the last game closed, in auto mode; 0 if none has
    bool had_game = false;
    while (!stopping(app)) {
        f64 now = now_ts();
        i32 files = -1;
        f64 secs = 0;
        if (now >= next_count) {  // the folder is read unlocked; the data drive may be slow
            count_queue(spool, &files, &secs);
            next_count = now + 2;
        }

        Child stop = {};
        bool start = false, wake = false;
        char state[160];
        app_lock(app);
        Worker* w = &app->worker;
        if (!w->process) {  // set up from Settings since, or another Python chosen there
            bool was = found;
            found = find_worker(&app->config, python, dir);
            if (found && !was) LOG("transcriber found: %ls", python);
            if (found == w->missing) {
                w->missing = !found;
                wake = true;
            }
        }
        bool game = app->apps.game_running[0] != 0;
        if (had_game && !game) quiet_from = now;
        had_game = game;
        if (files >= 0 && (files != w->queued || fabs(secs - w->queued_secs) > 0.5)) {
            w->queued = files;
            w->queued_secs = secs;
            wake = true;
        }
        bool want = found && worker_allowed(app, quiet_from, state, sizeof(state));
        if (w->process && !want) {
            LOG("stopping worker (%s)", state);
            stop = child_take(w);
        } else if (w->process && WaitForSingleObject(w->process, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(w->process, &code);
            WARN("worker exited (code %lu); restarting in 15 s", code);
            stop = child_take(w);
            next_start = now + 15;
        }
        // worker_stop waits for a process being stopped here, so the exit does not cut it off.
        if (stop.process)
            DuplicateHandle(GetCurrentProcess(), stop.process, GetCurrentProcess(), &w->leaving, SYNCHRONIZE, FALSE, 0); else if (!w->process && want && now >= next_start) {
            start = true;
        }
        if (want) {
            if (w->process) snprintf(state, sizeof(state), w->queued ? "transcribing" : "up to date");
            else snprintf(state, sizeof(state), "starting");
            w->waiting_since = 0;
        } else {
            if (!found) snprintf(state, sizeof(state), "not set up yet");
            if (!w->waiting_since) w->waiting_since = now;
        }
        if (strcmp(state, w->state) != 0) {
            SET(w->state, str(state));
            wake = true;
        }
        app_unlock(app);

        if (stop.process) {
            child_stop(&stop);
            app_lock(app);
            CloseHandle(app->worker.leaving);
            app->worker.leaving = 0;
            app_unlock(app);
        }
        if (start) {
            Child c;
            bool ok = child_start(app, python, dir, &c);
            app_lock(app);
            if (ok && !stopping(app)) {
                app->worker.process = c.process;
                app->worker.stdin_write = c.stdin_write;
                app->worker.job = c.job;
            } else if (!ok) {
                next_start = now_ts() + 60;
            }
            app_unlock(app);
            if (ok && stopping(app)) child_stop(&c);  // the program ended while it started
        }
        app_lock(app);
        bool running = app->worker.process != 0;
        wake |= running != app->worker.running;
        app->worker.running = running;
        app_unlock(app);
        if (wake) wake_window(app);
        sleep_unless_stopping(app, 500);
    }

    // A running worker is left for worker_stop, which the main thread calls before the program
    // exits; stopping it here could be cut off halfway by that exit.
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

void worker_stop(App* app) {
    Child c = child_take(&app->worker);
    app->worker.running = false;
    if (c.process) {
        LOG("stopping worker (lifelog is ending)");
        child_stop(&c);
        LOG("worker stopped");
    }
    // One the supervisor is already stopping (a game started): let it finish its file too.
    if (app->worker.leaving) WaitForSingleObject(app->worker.leaving, 25000);
}
