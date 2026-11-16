// setup.cpp: see setup.h.

// Pinned like scripts\get-llm.ps1; bump both together.
static const wchar_t* LLAMA_BUILD = L"b11873";
static const wchar_t* QWEN_URL = L"https://huggingface.co/Qwen/Qwen3-14B-GGUF/resolve/main/Qwen3-14B-Q4_K_M.gguf";
static const char* QWEN_MODEL = "qwen3-14b";
static const wchar_t* WHISPER_BUILD = L"b5454";
static const wchar_t* WHISPER_ZIP = L"whisper-bin-win-cuda-12.4.0-x64.zip";
static const char* WHISPER_MODEL = "large-v3-turbo";
static const wchar_t* WHISPER_MODEL_URL = L"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin";
static const char* WHISPER_VAD_MODEL = "ggml-silero-v6.2.0.bin";
static const wchar_t* WHISPER_VAD_URL = L"https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin";

static void setup_say(App* app, SetupPart part, const char* fmt, ...) {
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    app_lock(app);
    SET(app->setup.part[part].text, str(text));
    app_unlock(app);
    wake_window(app);
}

static void setup_dir(const wchar_t* name, wchar_t out[MAX_PATH]) { swprintf(out, MAX_PATH, L"%ls\\%ls", local_dir(), name); }

void whisper_dir(wchar_t out[MAX_PATH]) { setup_dir(L"whisper", out); }

bool whisper_ready(const char* model) {
    wchar_t dir[MAX_PATH], exe[MAX_PATH], bin[MAX_PATH], vad[MAX_PATH];
    whisper_dir(dir);
    swprintf(exe, MAX_PATH, L"%ls\\Release\\whisper-server.exe", dir);
    swprintf(bin, MAX_PATH, L"%ls\\ggml-%hs.bin", dir, model);
    swprintf(vad, MAX_PATH, L"%ls\\%hs", dir, WHISPER_VAD_MODEL);
    return GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(bin) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW(vad) != INVALID_FILE_ATTRIBUTES;
}

bool summary_ai_ready(const char* model) {
    wchar_t exe[MAX_PATH], gguf[MAX_PATH];
    swprintf(exe, MAX_PATH, L"%ls\\llm\\llama-server.exe", local_dir());
    swprintf(gguf, MAX_PATH, L"%ls\\llm\\%hs.gguf", local_dir(), model);
    return GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(gguf) != INVALID_FILE_ATTRIBUTES;
}

static bool exists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

// ---------------------------------------------------------------------------------------------
// Downloading

// `url` to `path` (through path.part, so a cut-off download is never taken for a whole one),
// following redirects, with "<what>: x of y GB" as it goes.
static bool download(App* app, SetupPart part, const char* what, const wchar_t* url, const wchar_t* path, char* error, i32 cap) {
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
        while (WaitForSingleObject(pi.hProcess, 500) == WAIT_TIMEOUT) {
            if (stopping(app)) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }
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

static bool setup_transcriber(App* app, char* error, i32 cap) {
    wchar_t dir[MAX_PATH], exe[MAX_PATH], stamp[MAX_PATH];
    whisper_dir(dir);
    CreateDirectoryW(dir, 0);
    swprintf(exe, MAX_PATH, L"%ls\\Release\\whisper-server.exe", dir);
    swprintf(stamp, MAX_PATH, L"%ls\\build.txt", dir);
    Mark m = mark(scratch());
    Str have = read_file(scratch(), stamp);
    bool current = have.ptr && str_eq(str_trim(have), narrow(scratch(), WHISPER_BUILD));
    rewind(m);
    if (!exists(exe) || !current) {
        // Our server holds its files open; the supervisor starts it again once they are in place.
        app_lock(app);
        worker_stop(app);
        app_unlock(app);
        wchar_t url[512], zip[MAX_PATH], release[MAX_PATH];
        swprintf(url, COUNT(url), L"https://github.com/ggml-org/whisper.cpp/releases/download/%ls/%ls", WHISPER_BUILD, WHISPER_ZIP);
        swprintf(zip, MAX_PATH, L"%ls\\%ls", dir, WHISPER_ZIP);
        swprintf(release, MAX_PATH, L"%ls\\Release", dir);
        if (!download(app, SETUP_TRANSCRIBER, "whisper.cpp", url, zip, error, cap)) return false;
        remove_tree(release);  // an older build
        bool ok = unzip(app, zip, dir, error, cap);
        DeleteFileW(zip);
        if (!ok) return false;
        if (!exists(exe)) {
            snprintf(error, cap, "whisper.cpp's download did not hold whisper-server.exe");
            return false;
        }
        write_file(stamp, narrow(scratch(), WHISPER_BUILD));
    }
    wchar_t bin[MAX_PATH], vad[MAX_PATH];
    swprintf(bin, MAX_PATH, L"%ls\\ggml-%hs.bin", dir, WHISPER_MODEL);
    swprintf(vad, MAX_PATH, L"%ls\\%hs", dir, WHISPER_VAD_MODEL);
    if (!exists(bin) && !download(app, SETUP_TRANSCRIBER, "the model", WHISPER_MODEL_URL, bin, error, cap)) return false;
    if (!exists(vad) && !download(app, SETUP_TRANSCRIBER, "the voice detection model", WHISPER_VAD_URL, vad, error, cap)) return false;
    app_lock(app);
    if (strcmp(app->config.worker.model, WHISPER_MODEL) != 0) {
        SET(app->config.worker.model, str(WHISPER_MODEL));
        config_save_string("worker", "model", WHISPER_MODEL);
    }
    app_unlock(app);
    return true;
}

static bool setup_summary_ai(App* app, char* error, i32 cap) {
    wchar_t dir[MAX_PATH], exe[MAX_PATH], stamp[MAX_PATH];
    setup_dir(L"llm", dir);
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
    wchar_t gguf[MAX_PATH];
    swprintf(gguf, MAX_PATH, L"%ls\\%hs.gguf", dir, QWEN_MODEL);
    if (!exists(gguf) && !download(app, SETUP_SUMMARY_AI, "the model", QWEN_URL, gguf, error, cap)) return false;
    app_lock(app);
    if (strcmp(app->config.summary.model, QWEN_MODEL) != 0) {
        SET(app->config.summary.model, str(QWEN_MODEL));
        config_save_string("summary", "model", QWEN_MODEL);
    }
    app_unlock(app);
    return true;
}

struct SetupJob {
    App* app;
    SetupPart part;
};

static DWORD WINAPI setup_thread(void* p) {
    SetupJob job = *(SetupJob*)p;
    free(p);
    App* app = job.app;
    const char* name = job.part == SETUP_TRANSCRIBER ? "transcriber" : "summary AI";
    LOG("setting up the %s", name);
    char error[512] = "";
    bool ok = job.part == SETUP_TRANSCRIBER ? setup_transcriber(app, error, sizeof(error)) : setup_summary_ai(app, error, sizeof(error));
    if (ok) LOG("the %s is set up", name);
    else WARN("setting up the %s failed: %s", name, error);
    app_lock(app);
    SetupState* s = &app->setup.part[job.part];
    s->running = false;
    s->failed = !ok;
    SET(s->text, str(ok ? "" : error));
    app_unlock(app);
    wake_window(app);
    return 0;
}

void setup_start(App* app, SetupPart part) {
    SetupState* s = &app->setup.part[part];
    if (s->running) return;
    s->running = true;
    s->failed = false;
    SET(s->text, str("Starting"));
    SetupJob* job = (SetupJob*)calloc(1, sizeof(SetupJob));
    CHECK(job, "memory for a setup job");
    job->app = app;
    job->part = part;
    HANDLE t = CreateThread(0, 0, setup_thread, job, 0, 0);
    CHECK(t, "the setup thread to start (error %lu)", GetLastError());
    CloseHandle(t);
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
}
