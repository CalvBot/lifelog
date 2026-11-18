// setup.cpp: see setup.h.

// The llama.cpp and whisper.cpp releases Settings downloads; bump to update.
static const wchar_t* LLAMA_BUILD = L"b11873";
static const wchar_t* WHISPER_BUILD = L"b5454";
static const wchar_t* WHISPER_ZIP = L"whisper-bin-win-cuda-12.4.0-x64.zip";
static const char* WHISPER_VAD_MODEL = "ggml-silero-v6.2.0.bin";
static const wchar_t* WHISPER_VAD_URL = L"https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin";
// The small English model voice commands are heard with (voice.h), on the processor.
// Voice commands' models run on the processor, beside a game, so they are the small English ones.
static const VoiceModel VOICE_MODELS[] = {
    {"base.en", "Base", "0.15 GB. Quick, and hears most commands. The default.", 0.15},
    {"tiny.en", "Tiny", "0.08 GB. The lightest on the processor, but mishears more.", 0.08},
    {"small.en", "Small", "0.49 GB. Hears best, but takes longer and uses more of the processor.", 0.49},
};

// The models offered in Settings, the default first. Each is a file published where its makers
// put it; a new one needs only a line here (and a .gguf llama.cpp b11873 can run).
#define WHISPER_HF L"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
static const AiModel WHISPER_MODELS[] = {
    {"large-v3-turbo", "Whisper large-v3-turbo", "Fast and accurate. The default.", WHISPER_HF L"ggml-large-v3-turbo.bin", 1.62},
    {"large-v3-turbo-q5_0", "Whisper large-v3-turbo, small", "Nearly as accurate, a third of the size.", WHISPER_HF L"ggml-large-v3-turbo-q5_0.bin", 0.57},
    {"large-v3", "Whisper large-v3", "The most accurate, about twice as slow.", WHISPER_HF L"ggml-large-v3.bin", 3.10},
    {"small.en", "Whisper small, English", "English only. For a weak graphics card.", WHISPER_HF L"ggml-small.en.bin", 0.49},
};
#undef WHISPER_HF
static const AiModel SUMMARY_MODELS[] = {
    {"qwen3-14b", "Qwen3 14B", "The default. Needs a 12 GB graphics card.", L"https://huggingface.co/Qwen/Qwen3-14B-GGUF/resolve/main/Qwen3-14B-Q4_K_M.gguf", 9.00},
    {"gemma-4-12b", "Gemma 4 12B", "Google's. Needs a 10 GB graphics card.",
     L"https://huggingface.co/google/gemma-4-12B-it-qat-q4_0-gguf/resolve/main/gemma-4-12b-it-qat-q4_0.gguf", 6.98},
    {"qwen3-8b", "Qwen3 8B", "Needs an 8 GB graphics card.", L"https://huggingface.co/Qwen/Qwen3-8B-GGUF/resolve/main/Qwen3-8B-Q4_K_M.gguf", 5.03},
    {"qwen3-4b", "Qwen3 4B", "Needs a 6 GB graphics card. Plainer.", L"https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf", 2.50},
    {"qwen3.8-27b", "Qwen3.8 27B", "The best. Needs a 24 GB graphics card.",
     L"https://huggingface.co/unsloth/Qwen3.8-27B-GGUF/resolve/main/Qwen3.8-27B-UD-Q4_K_M.gguf", 16.6},
};

i32 ai_models(SetupPart part, const AiModel** out) {
    *out = part == SETUP_TRANSCRIBER ? WHISPER_MODELS : SUMMARY_MODELS;
    return part == SETUP_TRANSCRIBER ? (i32)COUNT(WHISPER_MODELS) : (i32)COUNT(SUMMARY_MODELS);
}

const AiModel* ai_model(SetupPart part, const char* name) {
    const AiModel* models;
    i32 n = ai_models(part, &models);
    for (i32 i = 0; i < n; i++)
        if (strcmp(models[i].name, name) == 0) return &models[i];
    return 0;
}

// The setup threads take no lock: what they say, the model they set up and their end are posted
// to the main thread as these records. `part` is SETUP_PARTS for the move.
static SetupState* setup_state(App* app, i32 part) { return part == SETUP_PARTS ? &app->setup.move : &app->setup.part[part]; }

// Progress: only the newest matters, so each part has its own Latest.
struct SetupSaid {
    i32 part;
    char text[sizeof(SetupState::text)];
};

// The model a part now has downloaded, to use instead of your own server.
struct SetupModel {
    SetupPart part;
    char model[sizeof(SetupState::model)];
};

// The end of a download or the move.
struct SetupDone {
    i32 part;
    bool ok;
    char error[512];  // why not
};

static void setup_say_apply(App* app, Event* e) {
    SetupSaid said = latest_take<SetupSaid>(e);
    SET(setup_state(app, said.part)->text, str(said.text));
    wake_window(app);
}

static void setup_say(App* app, i32 part, const char* fmt, ...) {
    SetupSaid said;
    said.part = part;
    va_list args;
    va_start(args, fmt);
    vsnprintf(said.text, sizeof(said.text), fmt, args);
    va_end(args);
    latest_send(app, &app->setup.said[part], setup_say_apply, said);
}

static void setup_model_apply(App* app, Event* e) {
    SetupModel* m = event_record<SetupModel>(e);
    if (m->part == SETUP_TRANSCRIBER) worker_set_endpoint(app, str(""), str(m->model));
    else summary_set_endpoint(app, str(""), str(m->model));
}

static void setup_done_apply(App* app, Event* e) {
    SetupDone* d = event_record<SetupDone>(e);
    SetupState* s = setup_state(app, d->part);
    s->running = false;
    s->failed = !d->ok;
    SET(s->text, str(d->ok ? "" : d->error));
    s->model[0] = 0;
    app->setup.scanned_ts = 0;  // look again
    worker_wake(app);
    wake_window(app);
}

static void ai_sub(const char* ai_dir, const wchar_t* name, wchar_t out[MAX_PATH]) { path_under(ai_dir, name, out); }

void whisper_dir(const char* ai_dir, wchar_t out[MAX_PATH]) { ai_sub(ai_dir, L"whisper", out); }

void whisper_bin_dir(const char* ai_dir, wchar_t out[MAX_PATH]) {
    wchar_t dir[MAX_PATH];
    whisper_dir(ai_dir, dir);
    swprintf(out, MAX_PATH, L"%ls\\%ls", dir, WHISPER_BUILD);
}

void llm_dir(const char* ai_dir, wchar_t out[MAX_PATH]) { ai_sub(ai_dir, L"llm", out); }

static bool exists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

void model_path(const char* ai_dir, SetupPart part, const char* model, wchar_t out[MAX_PATH]) {
    wchar_t dir[MAX_PATH];
    Mark m = mark(scratch());
    wchar_t* name = wide(scratch(), str(model));
    if (part == SETUP_TRANSCRIBER) {
        whisper_dir(ai_dir, dir);
        swprintf(out, MAX_PATH, L"%ls\\ggml-%ls.bin", dir, name);
    } else {
        llm_dir(ai_dir, dir);
        swprintf(out, MAX_PATH, L"%ls\\%ls.gguf", dir, name);
    }
    rewind(m);
}

void llm_exe_path(const char* ai_dir, wchar_t out[MAX_PATH]) {
    wchar_t dir[MAX_PATH];
    llm_dir(ai_dir, dir);
    swprintf(out, MAX_PATH, L"%ls\\llama-server.exe", dir);
}

bool whisper_ready(const char* ai_dir, const char* model) {
    wchar_t dir[MAX_PATH], bins[MAX_PATH], dll[MAX_PATH], bin[MAX_PATH], vad[MAX_PATH];
    whisper_dir(ai_dir, dir);
    whisper_bin_dir(ai_dir, bins);
    swprintf(dll, MAX_PATH, L"%ls\\whisper.dll", bins);
    model_path(ai_dir, SETUP_TRANSCRIBER, model, bin);
    swprintf(vad, MAX_PATH, L"%ls\\%hs", dir, WHISPER_VAD_MODEL);
    return exists(dll) && exists(bin) && exists(vad);
}

bool summary_ai_ready(const char* ai_dir, const char* model) {
    wchar_t exe[MAX_PATH], gguf[MAX_PATH];
    llm_exe_path(ai_dir, exe);
    model_path(ai_dir, SETUP_SUMMARY_AI, model, gguf);
    return exists(exe) && exists(gguf);
}

// The whisper.cpp build the worker loaded: its libraries stay open until lifelog closes, so that
// folder cannot be deleted or moved. Set once, by the worker.
static wchar_t g_whisper_from[MAX_PATH];
void whisper_loaded_from(const wchar_t* dir) { wcsncpy_s(g_whisper_from, dir, _TRUNCATE); }

// ---------------------------------------------------------------------------------------------
// Downloading

// `url` to `path` (through path.part, so a cut-off download is never taken for a whole one),
// following redirects, with "<what>: x of y GB" as it goes.
static bool download(App* app, i32 part, const char* what, const wchar_t* url, const wchar_t* path, char* error, i32 cap) {
    URL_COMPONENTS parts = {sizeof(parts)};
    wchar_t host[256] = {}, upath[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = COUNT(host);
    parts.lpszUrlPath = upath;
    parts.dwUrlPathLength = COUNT(upath);
    if (!WinHttpCrackUrl(url, 0, 0, &parts)) {
        snprintf(error, cap, "not a URL: %ls", url);
        return false;
    }
    wchar_t part_file[MAX_PATH];
    swprintf(part_file, MAX_PATH, L"%ls.part", path);
    HINTERNET session = WinHttpOpen(L"lifelog", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, parts.nPort, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", upath, 0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : 0;
    bool ok = req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, 0);
    if (!ok) snprintf(error, cap, "could not reach %ls (WinHTTP error %lu)", host, GetLastError());
    DWORD status = 0, size = sizeof(status);
    if (ok) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            snprintf(error, cap, "%s: the server answered %lu", what, status);
            ok = false;
        }
    }
    wchar_t length_text[32] = L"";
    size = sizeof(length_text);
    f64 total = 0;
    if (ok && WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &size, WINHTTP_NO_HEADER_INDEX))
        total = _wtof(length_text);
    HANDLE f = ok ? CreateFileW(part_file, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0) : INVALID_HANDLE_VALUE;
    if (ok && f == INVALID_HANDLE_VALUE) {
        snprintf(error, cap, "could not write %ls (error %lu)", part_file, GetLastError());
        ok = false;
    }
    f64 got = 0, said = -1;
    static char chunk[1 << 16];
    while (ok) {
        if (stopping(app)) {
            snprintf(error, cap, "lifelog was closed");
            ok = false;
            break;
        }
        DWORD n = 0;
        if (!WinHttpReadData(req, chunk, sizeof(chunk), &n)) {
            snprintf(error, cap, "%s: the download broke off (WinHTTP error %lu)", what, GetLastError());
            ok = false;
            break;
        }
        if (n == 0) break;
        DWORD wrote = 0;
        if (!WriteFile(f, chunk, n, &wrote, 0) || wrote != n) {
            snprintf(error, cap, "could not write %ls (the disk may be full)", part_file);
            ok = false;
            break;
        }
        got += n;
        if (got - said >= 20e6) {  // every 20 MB
            said = got;
            if (total > 0) setup_say(app, part, "Downloading %s: %.1f of %.1f GB", what, got / 1e9, total / 1e9);
            else setup_say(app, part, "Downloading %s: %.1f GB", what, got / 1e9);
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    if (ok && total > 0 && got != total) {
        snprintf(error, cap, "%s: the download stopped short", what);
        ok = false;
    }
    if (ok && !MoveFileExW(part_file, path, MOVEFILE_REPLACE_EXISTING)) {
        snprintf(error, cap, "could not move %ls into place (error %lu)", part_file, GetLastError());
        ok = false;
    }
    if (!ok) DeleteFileW(part_file);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Running the tools

// Runs `cmd` with no window, its output appended to setup.log, plus `env` ("NAME=value" strings,
// may be null) on top of our environment. False unless it exits with 0.
static bool run(App* app, const wchar_t* cmd, const wchar_t** env, i32 env_count, const wchar_t* cwd, char* error, i32 cap) {
    wchar_t log[MAX_PATH];
    swprintf(log, MAX_PATH, L"%ls\\setup.log", local_dir());
    SECURITY_ATTRIBUTES sa = {sizeof(sa), 0, TRUE};
    HANDLE out = CreateFileW(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, 0, 0);
    Mark m = mark(scratch());
    // Our environment block, then the extra variables.
    wchar_t* base = GetEnvironmentStringsW();
    i64 len = 0;
    while (base[len]) len += (i64)wcslen(base + len) + 1;
    i64 extra = 0;
    for (i32 i = 0; i < env_count; i++) extra += (i64)wcslen(env[i]) + 1;
    wchar_t* block = PUSH_ARRAY(scratch(), wchar_t, len + extra + 1);
    memcpy(block, base, (size_t)len * sizeof(wchar_t));
    i64 at = len;
    for (i32 i = 0; i < env_count; i++) {
        i64 n = (i64)wcslen(env[i]) + 1;
        memcpy(block + at, env[i], (size_t)n * sizeof(wchar_t));
        at += n;
    }
    block[at] = 0;
    FreeEnvironmentStringsW(base);

    if (out != INVALID_HANDLE_VALUE) {
        Str line = str_fmt(scratch(), "\r\n> %s\r\n", narrow(scratch(), cmd).ptr);
        DWORD w = 0;
        WriteFile(out, line.ptr, (DWORD)line.len, &w, 0);
    }
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = 0;
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi = {};
    i64 cmd_len = (i64)wcslen(cmd) + 1;
    wchar_t* cmd_copy = PUSH_ARRAY(scratch(), wchar_t, cmd_len);
    memcpy(cmd_copy, cmd, (size_t)cmd_len * sizeof(wchar_t));
    bool ok = CreateProcessW(0, cmd_copy, 0, 0, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, block, cwd ? cwd : local_dir(), &si, &pi) != 0;
    if (!ok) snprintf(error, cap, "could not run %ls (error %lu)", cmd, GetLastError());
    rewind(m);
    if (ok) {
        HANDLE waits[2] = {pi.hProcess, app->stop};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (code != 0) {
            snprintf(error, cap, "a setup step failed (exit code %lu): see setup.log in %ls", code, local_dir());
            ok = false;
        }
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    return ok;
}

// Unpacks a .zip into `dir` with the tar that comes with Windows.
static bool unzip(App* app, const wchar_t* zip, const wchar_t* dir, char* error, i32 cap) {
    wchar_t sys[MAX_PATH], cmd[3 * MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(cmd, COUNT(cmd), L"\"%ls\\tar.exe\" -xf \"%ls\" -C \"%ls\"", sys, zip, dir);
    return run(app, cmd, 0, 0, 0, error, cap);
}

// Deletes a folder this setup made, with everything in it.
static void remove_tree(const wchar_t* dir) {
    wchar_t from[MAX_PATH + 2] = {};
    wcsncpy_s(from, MAX_PATH, dir, _TRUNCATE);  // the list ends with two zeros
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

// ---------------------------------------------------------------------------------------------
// The parts

// What a download thread needs, worked out on the main thread from its config. It is the data of
// a Job (events.h) the thread holds, and drops when it ends.
struct SetupJob {
    App* app;
    SetupPart part;
    char model[128];
    wchar_t url[512];          // the model's
    wchar_t dir[MAX_PATH];     // whisper or llm
    wchar_t bins[MAX_PATH];    // whisper's build folder
    wchar_t target[MAX_PATH];  // the model file
    bool voice_only;           // for voice commands: the transcriber's model stays as chosen
    char voice[32];            // voice commands' model: "base.en"
};

static bool setup_transcriber(SetupJob* job, char* error, i32 cap) {
    App* app = job->app;
    // Each build gets its own folder: a running lifelog has the one it loaded open, so a new
    // build goes beside it and is used from the next start.
    wchar_t* dir = job->dir;
    wchar_t* bins = job->bins;
    wchar_t dll[MAX_PATH];
    CreateDirectoryW(dir, 0);
    swprintf(dll, MAX_PATH, L"%ls\\whisper.dll", bins);
    if (!exists(dll)) {
        wchar_t url[512], zip[MAX_PATH], unpacked[MAX_PATH], release[MAX_PATH];
        swprintf(url, COUNT(url), L"https://github.com/ggml-org/whisper.cpp/releases/download/%ls/%ls", WHISPER_BUILD, WHISPER_ZIP);
        swprintf(zip, MAX_PATH, L"%ls\\%ls", dir, WHISPER_ZIP);
        swprintf(unpacked, MAX_PATH, L"%ls\\unpacking", dir);
        swprintf(release, MAX_PATH, L"%ls\\Release", unpacked);
        if (!download(app, SETUP_TRANSCRIBER, "whisper.cpp", url, zip, error, cap)) return false;
        remove_tree(unpacked);  // left by a try that failed
        CreateDirectoryW(unpacked, 0);
        bool ok = unzip(app, zip, unpacked, error, cap);
        DeleteFileW(zip);
        if (ok && !MoveFileExW(release, bins, 0)) {
            snprintf(error, cap, "whisper.cpp's download did not hold the Release folder (error %lu)", GetLastError());
            ok = false;
        }
        remove_tree(unpacked);
        if (!ok) return false;
        if (!exists(dll)) {
            snprintf(error, cap, "whisper.cpp's download did not hold whisper.dll");
            return false;
        }
    }
    wchar_t vad[MAX_PATH];
    swprintf(vad, MAX_PATH, L"%ls\\%hs", dir, WHISPER_VAD_MODEL);
    if (!job->voice_only && !exists(job->target) && !download(app, SETUP_TRANSCRIBER, "the model", job->url, job->target, error, cap)) return false;
    if (!exists(vad) && !download(app, SETUP_TRANSCRIBER, "the voice detection model", WHISPER_VAD_URL, vad, error, cap)) return false;
    wchar_t voice[MAX_PATH];
    swprintf(voice, MAX_PATH, L"%ls\\ggml-%hs.bin", dir, job->voice);
    wchar_t url[256];
    swprintf(url, COUNT(url), L"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-%hs.bin", job->voice);
    if (!exists(voice) && !download(app, SETUP_TRANSCRIBER, "the voice commands model", url, voice, error, cap)) return false;
    return true;
}

static bool setup_summary_ai(SetupJob* job, char* error, i32 cap) {
    App* app = job->app;
    wchar_t* dir = job->dir;
    wchar_t exe[MAX_PATH], stamp[MAX_PATH];
    CreateDirectoryW(dir, 0);
    swprintf(exe, MAX_PATH, L"%ls\\llama-server.exe", dir);
    swprintf(stamp, MAX_PATH, L"%ls\\build.txt", dir);
    Mark m = mark(scratch());
    Str have = read_file(scratch(), stamp);
    bool current = have.ptr && str_eq(str_trim(have), narrow(scratch(), LLAMA_BUILD));
    rewind(m);
    if (!exists(exe) || !current) {
        wchar_t names[2][128];
        swprintf(names[0], 128, L"llama-%ls-bin-win-cuda-12.4-x64.zip", LLAMA_BUILD);
        swprintf(names[1], 128, L"cudart-llama-bin-win-cuda-12.4-x64.zip");
        for (i32 i = 0; i < 2; i++) {
            wchar_t url[512], zip[MAX_PATH];
            swprintf(url, COUNT(url), L"https://github.com/ggml-org/llama.cpp/releases/download/%ls/%ls", LLAMA_BUILD, names[i]);
            swprintf(zip, MAX_PATH, L"%ls\\%ls", dir, names[i]);
            if (!download(app, SETUP_SUMMARY_AI, i == 0 ? "llama.cpp" : "the CUDA runtime", url, zip, error, cap)) return false;
            bool ok = unzip(app, zip, dir, error, cap);
            DeleteFileW(zip);
            if (!ok) return false;
        }
        if (!exists(exe)) {
            snprintf(error, cap, "llama.cpp's download did not hold llama-server.exe");
            return false;
        }
        write_file(stamp, narrow(scratch(), LLAMA_BUILD));
    }
    if (!exists(job->target) && !download(app, SETUP_SUMMARY_AI, "the model", job->url, job->target, error, cap)) return false;
    return true;
}

static DWORD WINAPI setup_thread(void* p) {
    Job* holder = (Job*)p;
    SetupJob* job = (SetupJob*)holder->data;
    App* app = job->app;
    const char* name = job->part == SETUP_TRANSCRIBER ? "transcriber" : "summary AI";
    LOG("setting up the %s with %s", name, job->model);
    SetupDone done = {};
    done.part = job->part;
    done.ok = job->part == SETUP_TRANSCRIBER ? setup_transcriber(job, done.error, sizeof(done.error))
                                             : setup_summary_ai(job, done.error, sizeof(done.error));
    if (done.ok && job->voice_only) {
        LOG("voice commands are set up");
    } else if (done.ok) {
        LOG("the %s is set up", name);
        SetupModel m = {job->part};
        SET(m.model, str(job->model));
        event_send_wait(app, setup_model_apply, m);
    } else {
        WARN("setting up the %s failed: %s", name, done.error);
    }
    // Must arrive, or the part would show "setting up" until the program restarts.
    event_send_wait(app, setup_done_apply, done);
    job_drop(holder);
    return 0;
}

static bool setup_busy(App* app) {
    for (i32 i = 0; i < SETUP_PARTS; i++)
        if (app->setup.part[i].running) return true;
    return app->setup.move.running;
}

// ---------------------------------------------------------------------------------------------
// What to delete at the next start: config's ai.cleanup, folders separated by '|'.

static void cleanup_add(App* app, const wchar_t* dir) {
    char* list = app->config.ai.cleanup;
    Mark m = mark(scratch());
    Str d = narrow(scratch(), dir);
    i64 len = (i64)strlen(list);
    if (len + d.len + 2 < (i64)sizeof(app->config.ai.cleanup))
        snprintf(list + len, sizeof(app->config.ai.cleanup) - len, "%s%s", len ? "|" : "", d.ptr);
    config_save_string("ai", "cleanup", list);
    LOG("%s is in use: it is deleted when lifelog starts again", d.ptr);
    rewind(m);
}

// A folder to delete at the next start.
struct CleanupDir {
    char path[MAX_PATH * 2];  // UTF-8
};

static void cleanup_add_apply(App* app, Event* e) {
    Mark m = mark(scratch());
    cleanup_add(app, wide(scratch(), str(event_record<CleanupDir>(e)->path)));
    rewind(m);
}

static bool starts_with_path(Str s, Str dir) {
    return s.len >= dir.len && _strnicmp(s.ptr, dir.ptr, (size_t)dir.len) == 0 && (s.len == dir.len || s.ptr[dir.len] == '\\');
}

// Takes the folders under `dir` off the list: they are wanted again (set up again, or moved back).
static void cleanup_keep(App* app, Str dir) {
    char* list = app->config.ai.cleanup;
    if (!list[0]) return;
    char kept[sizeof(app->config.ai.cleanup)] = "";
    i64 at = 0;
    Str rest = str(list);
    while (rest.len) {
        i64 bar = 0;
        while (bar < rest.len && rest.ptr[bar] != '|') bar++;
        Str one = str_sub(rest, 0, bar);
        if (one.len && !starts_with_path(one, dir))
            at += snprintf(kept + at, sizeof(kept) - (size_t)at, "%s%.*s", at ? "|" : "", (int)one.len, one.ptr);
        rest = bar < rest.len ? str_sub(rest, bar + 1, rest.len - bar - 1) : Str{};
    }
    if (strcmp(kept, list) == 0) return;
    snprintf(list, sizeof(app->config.ai.cleanup), "%s", kept);
    config_save_string("ai", "cleanup", list);
}

// Only ever an AI folder lifelog made: ...\whisper, ...\llm or a build folder under whisper.
static bool ai_tree(Str path) {
    i64 slash = path.len;
    while (slash > 0 && path.ptr[slash - 1] != '\\') slash--;
    Str last = str_sub(path, slash, path.len - slash);
    if (str_eq_nocase(last, str("whisper")) || str_eq_nocase(last, str("llm"))) return true;
    Str parent = slash > 1 ? str_sub(path, 0, slash - 1) : Str{};
    i64 p = parent.len;
    while (p > 0 && parent.ptr[p - 1] != '\\') p--;
    return last.len && str_eq_nocase(str_sub(parent, p, parent.len - p), str("whisper"));
}

void setup_cleanup(Config* cfg) {
    if (!cfg->ai.cleanup[0]) return;
    Mark m = mark(scratch());
    Str rest = str(cfg->ai.cleanup);
    while (rest.len) {
        i64 bar = 0;
        while (bar < rest.len && rest.ptr[bar] != '|') bar++;
        Str one = str_sub(rest, 0, bar);
        if (one.len && ai_tree(one)) {
            LOG("deleting %.*s, left from before", (int)one.len, one.ptr);
            wchar_t* dir = wide(scratch(), one);
            remove_tree(dir);
            // A whisper folder emptied of its last build goes too.
            wchar_t parent[MAX_PATH];
            wcsncpy_s(parent, dir, _TRUNCATE);
            wchar_t* slash = wcsrchr(parent, L'\\');
            if (slash) {
                *slash = 0;
                RemoveDirectoryW(parent);
            }
        }
        rest = bar < rest.len ? str_sub(rest, bar + 1, rest.len - bar - 1) : Str{};
    }
    rewind(m);
    cfg->ai.cleanup[0] = 0;
    config_save_string("ai", "cleanup", "");
}

// ---------------------------------------------------------------------------------------------
// Removing

// Deletes `dir` and all in it, but for the whisper.cpp build in use (`keep`, may be empty), which
// is put in `kept`. False if something else would not go.
static bool remove_ai_dir(const wchar_t* dir, wchar_t kept[MAX_PATH]) {
    kept[0] = 0;
    if (!exists(dir)) return true;
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (g_whisper_from[0] && _wcsicmp(path, g_whisper_from) == 0) {
                wcsncpy_s(kept, MAX_PATH, path, _TRUNCATE);
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path);
            else DeleteFileW(path);
            if (exists(path)) ok = false;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);  // fails while something is kept: fine
    return ok;
}

bool setup_remove(App* app, SetupPart part, const char* model) {
    if (setup_busy(app)) {
        ui_say(app, "Wait for the download or the move to finish first");
        return false;
    }
    if (part == SETUP_SUMMARY_AI && app->summary.writing) {
        ui_say(app, "The summary AI is writing: try again when it is done");
        return false;
    }
    wchar_t path[MAX_PATH];
    model_path(app->config.ai.dir, part, model, path);
    app->setup.scanned_ts = 0;
    if (!DeleteFileW(path) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        WARN("could not delete the model %s (error %lu)", model, GetLastError());
        ui_say(app, "Could not delete %s: it may be in use. Try again in a minute.", model);
        return false;
    }
    LOG("removed the %s model %s", part == SETUP_TRANSCRIBER ? "transcription" : "summary", model);
    ui_say(app, "Removed %s", model);
    worker_wake(app);
    return true;
}

void setup_remove_all(App* app) {
    if (setup_busy(app)) {
        ui_say(app, "Wait for the download or the move to finish first");
        return;
    }
    if (app->summary.writing) {
        ui_say(app, "The summary AI is writing: try again when it is done");
        return;
    }
    wchar_t whisper[MAX_PATH], llm[MAX_PATH], kept[MAX_PATH], none[MAX_PATH];
    whisper_dir(app->config.ai.dir, whisper);
    llm_dir(app->config.ai.dir, llm);
    bool ok = remove_ai_dir(whisper, kept);
    ok = remove_ai_dir(llm, none) && ok;
    if (kept[0]) cleanup_add(app, kept);
    app->setup.scanned_ts = 0;
    worker_wake(app);
    LOG("removed lifelog's AI from %s", app->config.ai.dir);
    if (!ok) ui_say(app, "Some of the AI files are in use: see %s", app->config.ai.dir);
    else if (kept[0]) ui_say(app, "Removed. whisper.cpp itself is in use, and goes when lifelog restarts.");
    else ui_say(app, "Removed lifelog's AI");
}

// ---------------------------------------------------------------------------------------------
// Moving the AI folder

// The move's data, in a Job (events.h) held by the move thread and, once the copy is done, by
// the queue that takes move_apply to the main thread. The last to let go closes `applied`.
struct MoveJob {
    App* app;
    wchar_t from[MAX_PATH];  // the old AI folder
    wchar_t to[MAX_PATH];
    char to_utf8[MAX_PATH];
    f64 total, done, said;
    HANDLE applied;  // set by the main thread once the settings point at `to`
};

static void move_cleanup(Job* job) {
    MoveJob* j = (MoveJob*)job->data;
    if (j->applied) CloseHandle(j->applied);
}

static f64 tree_bytes(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    f64 sum = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t sub[MAX_PATH];
            swprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            sum += tree_bytes(sub);
        } else {
            sum += (f64)(((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return sum;
}

static DWORD CALLBACK move_progress(LARGE_INTEGER, LARGE_INTEGER copied, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, void* p) {
    MoveJob* j = (MoveJob*)p;
    if (stopping(j->app)) return PROGRESS_CANCEL;
    f64 now = j->done + (f64)copied.QuadPart;
    if (now - j->said >= 100e6) {
        j->said = now;
        setup_say(j->app, SETUP_PARTS, "Moving: %.1f of %.1f GB", now / 1e9, j->total / 1e9);
    }
    return PROGRESS_CONTINUE;
}

// Copies `from` into `to`, folders and all; a file already there at the same size is taken as
// copied. Not .part files (cut-off downloads).
static bool copy_tree(MoveJob* j, const wchar_t* from, const wchar_t* to, char* error, i32 cap) {
    CreateDirectoryW(to, 0);
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", from);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        swprintf(src, MAX_PATH, L"%ls\\%ls", from, fd.cFileName);
        swprintf(dst, MAX_PATH, L"%ls\\%ls", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = copy_tree(j, src, dst, error, cap);
            continue;
        }
        size_t n = wcslen(fd.cFileName);
        if (n > 5 && _wcsicmp(fd.cFileName + n - 5, L".part") == 0) continue;
        f64 size = (f64)(((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        WIN32_FILE_ATTRIBUTE_DATA have;
        if (GetFileAttributesExW(dst, GetFileExInfoStandard, &have) && (f64)(((u64)have.nFileSizeHigh << 32) | have.nFileSizeLow) == size) {
            j->done += size;
            continue;
        }
        wchar_t part[MAX_PATH];
        swprintf(part, MAX_PATH, L"%ls.part", dst);
        BOOL cancel = FALSE;
        if (!CopyFileExW(src, part, move_progress, j, &cancel, 0) || !MoveFileExW(part, dst, MOVEFILE_REPLACE_EXISTING)) {
            DWORD err = GetLastError();
            DeleteFileW(part);
            if (stopping(j->app)) snprintf(error, cap, "lifelog was closed");
            else snprintf(error, cap, "could not copy %ls (error %lu; is the disk full?)", fd.cFileName, err);
            ok = false;
        }
        j->done += size;
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// The new AI folder has everything. Hands it to the transcriber and the summary at once, so they
// load from there from here on, then lets the move thread clear the old one.
static void move_apply(App* app, Job* job) {
    MoveJob* j = (MoveJob*)job->data;
    SET(app->config.ai.dir, str(j->to_utf8));
    config_save_string("ai", "dir", app->config.ai.dir);
    cleanup_keep(app, str(app->config.ai.dir));
    LOG("the AI files are now in %s", app->config.ai.dir);
    worker_options_publish(app);
    summary_options_publish(app);
    SetEvent(j->applied);
}

static DWORD WINAPI move_thread(void* p) {
    Job* job = (Job*)p;
    MoveJob* j = (MoveJob*)job->data;
    App* app = j->app;
    SetupDone done = {};
    done.part = SETUP_PARTS;
    char* error = done.error;
    const wchar_t* subs[2] = {L"whisper", L"llm"};
    wchar_t from[2][MAX_PATH], to[2][MAX_PATH];
    for (i32 i = 0; i < 2; i++) {
        swprintf(from[i], MAX_PATH, L"%ls\\%ls", j->from, subs[i]);
        swprintf(to[i], MAX_PATH, L"%ls\\%ls", j->to, subs[i]);
        j->total += tree_bytes(from[i]);
    }
    LOG("moving the AI files (%.1f GB) to %s", j->total / 1e9, j->to_utf8);
    bool ok = SHCreateDirectoryExW(0, j->to, 0) == ERROR_SUCCESS || exists(j->to);
    if (!ok) snprintf(error, sizeof(done.error), "could not make the folder %s (error %lu)", j->to_utf8, GetLastError());
    ULARGE_INTEGER free_bytes = {};
    if (ok && GetDiskFreeSpaceExW(j->to, &free_bytes, 0, 0) && (f64)free_bytes.QuadPart < j->total - tree_bytes(to[0]) - tree_bytes(to[1])) {
        snprintf(error, sizeof(done.error), "%s has %.1f GB free; the AI files need %.1f GB", j->to_utf8, (f64)free_bytes.QuadPart / 1e9, j->total / 1e9);
        ok = false;
    }
    bool moved[2] = {}, applied = false;
    for (i32 i = 0; ok && i < 2; i++) {
        if (!exists(from[i])) continue;
        // On the same drive a folder moves at once, unless something in it is open.
        if (!exists(to[i]) && MoveFileExW(from[i], to[i], 0)) {
            moved[i] = true;
            j->done += tree_bytes(to[i]);
            continue;
        }
        ok = copy_tree(j, from[i], to[i], error, sizeof(done.error));
    }
    if (ok) {
        job->apply = move_apply;
        job_post_wait(app, job_hold(job));
        // Old files go once the settings point at the new folder; if lifelog closes first, they
        // stay (the settings may still name them).
        HANDLE waits[2] = {j->applied, app->stop};
        applied = WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0;
        if (!applied) moved[0] = moved[1] = true;
        for (i32 i = 0; i < 2; i++) {
            if (moved[i]) continue;
            wchar_t kept[MAX_PATH];
            Mark m = mark(scratch());
            if (!remove_ai_dir(from[i], kept)) WARN("could not delete all of %s", narrow(scratch(), from[i]).ptr);
            if (kept[0]) {
                CleanupDir c;
                SET(c.path, narrow(scratch(), kept));
                event_send_wait(app, cleanup_add_apply, c);
            }
            rewind(m);
        }
        LOG("moved the AI files");
    } else {
        WARN("moving the AI files failed: %s", error);
    }
    done.ok = ok;
    event_send_wait(app, setup_done_apply, done);
    job_drop(job);  // a move_apply still waiting keeps it until it has run
    return 0;
}

void setup_move(App* app, Str dir) {
    dir = str_trim(dir);
    while (dir.len > 3 && (dir.ptr[dir.len - 1] == '\\' || dir.ptr[dir.len - 1] == '/')) dir.len--;
    if (!dir.len || str_eq_nocase(dir, str(app->config.ai.dir))) return;
    if (setup_busy(app)) {
        ui_say(app, "Wait for the download to finish first");
        return;
    }
    if (app->summary.writing) {
        ui_say(app, "The summary AI is writing: try again when it is done");
        return;
    }
    // Not into the folders being moved.
    Mark m = mark(scratch());
    Str old = str(app->config.ai.dir);
    if (starts_with_path(dir, str_fmt(scratch(), "%s\\whisper", old.ptr)) || starts_with_path(dir, str_fmt(scratch(), "%s\\llm", old.ptr))) {
        ui_say(app, "Pick a folder outside the one the AI files are in now");
        rewind(m);
        return;
    }
    rewind(m);
    Job* job = job_new(0);
    MoveJob* j = PUSH(&job->arena, MoveJob);
    job->data = j;
    job->cleanup = move_cleanup;
    j->app = app;
    j->applied = CreateEventW(0, TRUE, FALSE, 0);
    CHECK(j->applied, "an event for the move (error %lu)", GetLastError());
    m = mark(scratch());
    wcsncpy_s(j->from, wide(scratch(), old), _TRUNCATE);
    wcsncpy_s(j->to, wide(scratch(), dir), _TRUNCATE);
    rewind(m);
    set(j->to_utf8, sizeof(j->to_utf8), dir);
    SetupState* s = &app->setup.move;
    s->running = true;
    s->failed = false;
    SET(s->text, str("Moving"));
    HANDLE t = CreateThread(0, 0, move_thread, job, 0, 0);
    CHECK(t, "the move thread to start (error %lu)", GetLastError());
    CloseHandle(t);
}

// ---------------------------------------------------------------------------------------------
// What is there

static void scan_part(App* app, SetupPart part, const wchar_t* dir, const wchar_t* pattern, const char* prefix, const char* suffix) {
    Setup* s = &app->setup;
    s->file_count[part] = 0;
    f64 all = tree_bytes(dir);
    // Voice commands' model is not listed with the transcriber's, unless the transcriber uses it too.
    // An old one, no longer chosen, is listed, so it can be deleted.
    char voice[64] = {};
    if (part == SETUP_TRANSCRIBER && strcmp(app->config.voice.model, app->config.worker.model))
        snprintf(voice, sizeof(voice), "ggml-%s.bin", app->config.voice.model);
    Str hidden = str(voice);
    wchar_t find[MAX_PATH];
    swprintf(find, MAX_PATH, L"%ls\\%ls", dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(find, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Mark m = mark(scratch());
            Str name = narrow(scratch(), fd.cFileName);
            Str pre = str(prefix), suf = str(suffix);
            if (name.len > pre.len + suf.len && str_eq(str_sub(name, name.len - suf.len, suf.len), suf) &&
                !str_eq(name, str(WHISPER_VAD_MODEL)) && !str_eq(name, hidden) && s->file_count[part] < (i32)COUNT(s->files[part])) {
                AiFile* f = &s->files[part][s->file_count[part]++];
                SET(f->name, str_sub(name, pre.len, name.len - pre.len - suf.len));
                f->bytes = (f64)(((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
            }
            rewind(m);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    s->total_bytes += all;
}

void ai_scan(App* app) {
    wchar_t whisper[MAX_PATH], llm[MAX_PATH];
    whisper_dir(app->config.ai.dir, whisper);
    llm_dir(app->config.ai.dir, llm);
    app->setup.total_bytes = 0;
    scan_part(app, SETUP_TRANSCRIBER, whisper, L"ggml-*.bin", "ggml-", ".bin");
    scan_part(app, SETUP_SUMMARY_AI, llm, L"*.gguf", "", ".gguf");
    app->setup.scanned_ts = now_ts();
}

// ---------------------------------------------------------------------------------------------

static void setup_begin(App* app, SetupPart part, const char* model, bool voice_only) {
    SetupState* s = &app->setup.part[part];
    if (setup_busy(app)) return;
    const AiModel* known = ai_model(part, model);
    if (!known) {
        const AiModel* models;
        ai_models(part, &models);
        known = &models[0];
    }
    s->running = true;
    s->failed = false;
    SET(s->text, str("Starting"));
    SET(s->model, str(known->name));
    // Set up again here: what was left to delete at the next start is wanted after all.
    cleanup_keep(app, str(app->config.ai.dir));
    Job* holder = job_new(0);
    SetupJob* job = PUSH(&holder->arena, SetupJob);
    holder->data = job;
    job->app = app;
    job->part = part;
    job->voice_only = voice_only;
    SET(job->voice, str(app->config.voice.model));
    SET(job->model, str(known->name));
    wcsncpy_s(job->url, known->url, _TRUNCATE);
    if (part == SETUP_TRANSCRIBER) {
        whisper_dir(app->config.ai.dir, job->dir);
        whisper_bin_dir(app->config.ai.dir, job->bins);
    } else {
        llm_dir(app->config.ai.dir, job->dir);
    }
    model_path(app->config.ai.dir, part, known->name, job->target);
    Mark m = mark(scratch());
    SHCreateDirectoryExW(0, wide(scratch(), str(app->config.ai.dir)), 0);
    rewind(m);
    HANDLE t = CreateThread(0, 0, setup_thread, holder, 0, 0);
    CHECK(t, "the setup thread to start (error %lu)", GetLastError());
    CloseHandle(t);
}

void setup_start(App* app, SetupPart part, const char* model) { setup_begin(app, part, model, false); }

void setup_start_voice(App* app) { setup_begin(app, SETUP_TRANSCRIBER, app->config.worker.model, true); }

void voice_model_path(const char* ai_dir, const char* model, wchar_t out[MAX_PATH]) {
    wchar_t dir[MAX_PATH];
    whisper_dir(ai_dir, dir);
    swprintf(out, MAX_PATH, L"%ls\\ggml-%hs.bin", dir, model);
}

i32 voice_models(const VoiceModel** out) {
    *out = VOICE_MODELS;
    return (i32)COUNT(VOICE_MODELS);
}

const VoiceModel* voice_model(Str name) {
    for (i32 i = 0; i < (i32)COUNT(VOICE_MODELS); i++)
        if (str_eq(name, str(VOICE_MODELS[i].name))) return &VOICE_MODELS[i];
    return 0;
}

void summary_set_endpoint(App* app, Str url, Str model) {
    url = str_trim(url);
    model = str_trim(model);
    SET(app->config.summary.endpoint, url);
    config_save_string("summary", "endpoint", app->config.summary.endpoint);
    if (model.len) {
        SET(app->config.summary.model, model);
        config_save_string("summary", "model", app->config.summary.model);
    }
    LOG("summary AI: %s", url.len ? app->config.summary.endpoint : "lifelog's own");
    summary_options_publish(app);
}
