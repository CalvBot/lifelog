// llm.cpp: the AI for the daily summary. By default it is lifelog's own copy of llama.cpp, in
// <ai dir>\llm (put there from Settings, see setup.h): llama-server.exe with its DLLs,
// and <model>.gguf. The server is started only while a summary is written and stopped straight
// after; it is tied to a job object, so it also goes if lifelog crashes.
//
// With [summary] endpoint set, any OpenAI-compatible server (Ollama, LM Studio, a hosted API) is
// used instead and nothing is started.

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// Only lifelog talks to it. A test copy (LIFELOG_CONFIG set) uses another port, so it never meets
// the installed lifelog's server.
static const char* LLM_HOST = "127.0.0.1";
static u16 llm_port() { return GetEnvironmentVariableW(L"LIFELOG_CONFIG", 0, 0) ? 47811 : 47801; }

struct Llm {
    HANDLE process;  // lifelog's own server, if it started one
    HANDLE job;
    char url[512];   // the API base, ending in /v1
    char key[256];
    char model[128];
};

// ---------------------------------------------------------------------------------------------
// HTTP

struct HttpReply {
    u32 status;
    Str body;
};

// One request. `extra_headers` are "Name: value\r\n" lines. False (with `error`) when the server
// could not be reached; an HTTP error status is still a reply.
static bool http_call(Arena* a, const char* method, Str url, Str extra_headers, Str body, u32 timeout_ms, HttpReply* out,
                      char* error, i32 cap) {
    *out = {};
    Mark m = mark(scratch());
    wchar_t* wurl = wide(scratch(), url);
    URL_COMPONENTS parts = {sizeof(parts)};
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = COUNT(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = COUNT(path);
    if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) {
        snprintf(error, cap, "not a URL: %.*s", (int)url.len, url.ptr);
        rewind(m);
        return false;
    }
    // A server on this PC is reached directly; anything else goes through the system proxy, as
    // Settings' downloads do.
    bool local = _wcsicmp(host, L"127.0.0.1") == 0 || _wcsicmp(host, L"localhost") == 0 || _wcsicmp(host, L"::1") == 0;
    HINTERNET session = WinHttpOpen(L"lifelog", local ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, parts.nPort, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, wide(scratch(), str(method)), path, 0, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : 0;
    bool ok = false;
    List<char> data = {};
    if (req) {
        WinHttpSetTimeouts(req, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms);
        wchar_t* headers = extra_headers.len ? wide(scratch(), extra_headers) : 0;
        ok = WinHttpSendRequest(req, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0,
                                (void*)body.ptr, (DWORD)body.len, (DWORD)body.len, 0) &&
             WinHttpReceiveResponse(req, 0);
    }
    if (ok) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                            &size, WINHTTP_NO_HEADER_INDEX);
        out->status = status;
        for (;;) {
            char chunk[16384];
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk, sizeof(chunk), &got)) {
                ok = false;
                break;
            }
            if (got == 0) break;
            for (DWORD i = 0; i < got; i++) list_push(&data, chunk[i]);
        }
    }
    if (!ok) snprintf(error, cap, "WinHTTP error %lu", GetLastError());
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    rewind(m);
    // After the rewind, so the reply survives when `a` is the scratch arena too.
    if (ok) out->body = str_copy(a, {data.items, data.count});
    list_free(&data);
    return ok;
}

// ---------------------------------------------------------------------------------------------
// The server

void llm_close(Llm* llm) {
    if (llm->process) {
        TerminateProcess(llm->process, 0);
        WaitForSingleObject(llm->process, 10000);
        CloseHandle(llm->process);
    }
    if (llm->job) CloseHandle(llm->job);
    *llm = {};
}

// Puts the server in a job that Windows closes, killing it, when lifelog's handle goes.
static HANDLE kill_with_lifelog(HANDLE process) {
    HANDLE job = CreateJobObjectW(0, 0);
    if (!job) return 0;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) || !AssignProcessToJobObject(job, process))
        WARN("could not tie llama-server to lifelog");
    return job;
}

// Starts the server with `model` (a .gguf name in the llm folder) and waits until it has loaded.
static bool llm_start(App* app, const char* ai_dir, const char* model, u32 ctx, Llm* llm, char* error, i32 cap) {
    *llm = {};
    if (!summary_ai_ready(ai_dir, model)) {
        snprintf(error, cap, "the summary AI is not set up: see Settings, Summary");
        return false;
    }
    wchar_t dir[MAX_PATH], exe[MAX_PATH], gguf[MAX_PATH];
    llm_dir(ai_dir, dir);
    llm_exe_path(ai_dir, exe);
    model_path(ai_dir, SETUP_SUMMARY_AI, model, gguf);
    u16 port = llm_port();
    wchar_t cmd[2 * MAX_PATH + 256];
    swprintf(cmd, COUNT(cmd), L"\"%ls\" -m \"%ls\" --host %hs --port %u -ngl 999 -c %u --jinja --parallel 1", exe, gguf, LLM_HOST,
             (unsigned)port, ctx);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, 0, 0, FALSE, CREATE_NO_WINDOW, 0, dir, &si, &pi)) {
        snprintf(error, cap, "could not start llama-server (error %lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    llm->process = pi.hProcess;
    llm->job = kill_with_lifelog(pi.hProcess);
    snprintf(llm->url, sizeof(llm->url), "http://%s:%u/v1", LLM_HOST, (unsigned)port);
    SET(llm->model, str(model));
    LOG("llama-server started on port %u with %s", (unsigned)port, model);

    char health[128];
    snprintf(health, sizeof(health), "http://%s:%u/health", LLM_HOST, (unsigned)port);
    f64 started = now_ts();
    for (;;) {
        DWORD code = 0;
        if (GetExitCodeProcess(llm->process, &code) && code != STILL_ACTIVE) {
            snprintf(error, cap, "llama-server stopped while loading (exit code %lu)", code);
            llm_close(llm);
            return false;
        }
        Mark hm = mark(scratch());
        HttpReply reply = {};
        char ignored[128];
        bool up = http_call(scratch(), "GET", str(health), {}, {}, 2000, &reply, ignored, sizeof(ignored)) && reply.status == 200;
        rewind(hm);
        if (up) return true;
        if (now_ts() - started > 180) {
            snprintf(error, cap, "llama-server did not load the model in 3 minutes");
            llm_close(llm);
            return false;
        }
        if (!sleep_unless_stopping(app, 500)) {
            snprintf(error, cap, "lifelog is closing");
            llm_close(llm);
            return false;
        }
    }
}

// The AI the options ask for: `endpoint` if one is set, else lifelog's own llama.cpp. Not on the
// main thread: it may wait minutes for the model to load.
bool llm_open(App* app, const SummaryOptions* o, u32 ctx, Llm* llm, char* error, i32 cap) {
    Str endpoint = str_trim(str(o->endpoint));
    while (endpoint.len && endpoint.ptr[endpoint.len - 1] == '/') endpoint.len--;
    if (!endpoint.len) return llm_start(app, o->ai_dir, o->model, ctx, llm, error, cap);
    *llm = {};
    SET(llm->url, endpoint);
    SET(llm->key, str_trim(str(o->api_key)));
    SET(llm->model, str(o->model));
    return true;
}

// One chat turn whose answer must match `schema` (JSON text); the parsed answer goes in `out`,
// in `a`. Not on the main thread.
bool llm_json(Llm* llm, Arena* a, Str system, Str prompt, Str schema, Json* out, char* error, i32 cap) {
    Buf body = {};
    buf_cstr(&body, "{\"model\":");
    json_quote(&body, str(llm->model));
    buf_cstr(&body, ",\"messages\":[{\"role\":\"system\",\"content\":");
    json_quote(&body, system);
    buf_cstr(&body, "},{\"role\":\"user\",\"content\":");
    json_quote(&body, prompt);
    buf_cstr(&body, "}],\"temperature\":0.3,\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"name\":\"answer\",\"schema\":");
    buf_add(&body, schema);
    buf_cstr(&body, "}}");
    // Qwen3 on lifelog's own server: answer straight away, without thinking out loud first.
    if (llm->process) buf_cstr(&body, ",\"chat_template_kwargs\":{\"enable_thinking\":false}");
    buf_cstr(&body, "}");

    Mark m = mark(scratch());
    Str request = buf_take(&body, scratch());
    Str url = str_fmt(scratch(), "%s/chat/completions", llm->url);
    Str headers = llm->key[0] ? str_fmt(scratch(), "Content-Type: application/json\r\nAuthorization: Bearer %s\r\n", llm->key)
                              : str("Content-Type: application/json\r\n");
    HttpReply reply = {};
    char why[256];
    bool ok = http_call(a, "POST", url, headers, request, 900 * 1000, &reply, why, sizeof(why));
    if (!ok) {
        snprintf(error, cap, "could not reach the summary AI at %s: %s", url.ptr, why);
        rewind(m);
        return false;
    }
    rewind(m);

    Json parsed = {};
    if (!json_parse(a, reply.body, &parsed, 0, 0)) {
        snprintf(error, cap, "the summary AI answered %u: %.200s", reply.status, reply.body.ptr);
        return false;
    }
    const Json* err = json_get(&parsed, "error");
    Str message = json_text(json_get(err, "message"));
    if (!message.len) message = json_text(err);
    if (message.len) {
        snprintf(error, cap, "the summary AI: %.*s", (int)message.len, message.ptr);
        return false;
    }
    const Json* content = json_get(json_get(json_at(json_get(&parsed, "choices"), 0), "message"), "content");
    if (content->type != JSON_STRING) {
        snprintf(error, cap, "the summary AI sent no answer");
        return false;
    }
    // Some servers wrap JSON in a ```json block even when asked for JSON.
    Str answer = str_trim(content->str);
    while (str_starts(answer, str("```json"))) answer = str_sub(answer, 7);
    while (str_starts(answer, str("```"))) answer = str_sub(answer, 3);
    while (str_ends(answer, str("```"))) answer.len -= 3;
    answer = str_trim(answer);
    if (!json_parse(a, answer, out, 0, 0)) {
        snprintf(error, cap, "the model did not return JSON: %.200s", str_copy(scratch(), answer).ptr);
        return false;
    }
    return true;
}
