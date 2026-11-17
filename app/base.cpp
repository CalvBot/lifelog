// base.cpp: what base.h declares.

#include <shlobj.h>

// ---------------------------------------------------------------------------------------------
// fail

static SRWLOCK log_lock = SRWLOCK_INIT;
static HANDLE log_file = INVALID_HANDLE_VALUE;

[[noreturn]] void fail(const char* file, int line, const char* cond, const char* fmt, ...) {
    char said[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(said, sizeof(said), fmt, args);
    va_end(args);
    const char* name = strrchr(file, '\\');
    name = name ? name + 1 : file;
    log_line("FATAL", "%s:%d: expected %s (%s)", name, line, said, cond);
    if (IsDebuggerPresent()) __debugbreak();
    ExitProcess(3);
}

// ---------------------------------------------------------------------------------------------
// Arena

static const u64 ARENA_RESERVE = GB(4);
static const u64 ARENA_COMMIT = MB(1);

void* arena_push(Arena* a, u64 size, u64 align) {
    if (!a->base) {
        a->base = (u8*)VirtualAlloc(nullptr, ARENA_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
        CHECK(a->base, "%llu bytes of address space for an arena", ARENA_RESERVE);
        a->reserved = ARENA_RESERVE;
    }
    u64 at = (a->used + align - 1) & ~(align - 1);
    u64 end = at + size;
    CHECK(end <= a->reserved, "an arena to hold %llu bytes (it holds %llu)", end, a->reserved);
    if (end > a->committed) {
        u64 grow = ((end - a->committed + ARENA_COMMIT - 1) / ARENA_COMMIT) * ARENA_COMMIT;
        CHECK(VirtualAlloc(a->base + a->committed, grow, MEM_COMMIT, PAGE_READWRITE), "%llu more bytes of memory", grow);
        a->committed += grow;
    }
    a->used = end;
    // Pages reused after a reset still hold the old bytes.
    memset(a->base + at, 0, size);
    return a->base + at;
}

void arena_reset(Arena* a) { a->used = 0; }

void arena_release(Arena* a) {
    if (a->base) VirtualFree(a->base, 0, MEM_RELEASE);
    *a = {};
}

Arena* scratch() {
    static thread_local Arena arena;
    return &arena;
}

// ---------------------------------------------------------------------------------------------
// Str

Str str_copy(Arena* a, Str s) {
    char* out = PUSH_ARRAY(a, char, s.len + 1);
    if (s.len) memcpy(out, s.ptr, s.len);
    return {out, s.len};
}

Str str_fmt(Arena* a, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    CHECK(n >= 0, "a valid format string: %s", fmt);
    char* out = PUSH_ARRAY(a, char, n + 1);
    va_start(args, fmt);
    vsnprintf(out, n + 1, fmt, args);
    va_end(args);
    return {out, n};
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

Str str_lower(Arena* a, Str s) {
    Str out = str_copy(a, s);
    for (i64 i = 0; i < out.len; i++) ((char*)out.ptr)[i] = lower(out.ptr[i]);
    return out;
}

bool str_eq(Str a, Str b) { return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0); }

bool str_eq_nocase(Str a, Str b) {
    if (a.len != b.len) return false;
    for (i64 i = 0; i < a.len; i++)
        if (lower(a.ptr[i]) != lower(b.ptr[i])) return false;
    return true;
}

bool str_starts(Str s, Str p) { return s.len >= p.len && memcmp(s.ptr, p.ptr, p.len) == 0; }
bool str_ends(Str s, Str p) { return s.len >= p.len && memcmp(s.ptr + s.len - p.len, p.ptr, p.len) == 0; }

Str str_trim(Str s) {
    while (s.len && (u8)s.ptr[0] <= ' ') s.ptr++, s.len--;
    while (s.len && (u8)s.ptr[s.len - 1] <= ' ') s.len--;
    return s;
}

i64 str_find(Str s, Str n) {
    if (n.len == 0) return 0;
    for (i64 i = 0; i + n.len <= s.len; i++)
        if (memcmp(s.ptr + i, n.ptr, n.len) == 0) return i;
    return -1;
}

Str str_sub(Str s, i64 from, i64 len) {
    if (from > s.len) from = s.len;
    if (len < 0 || from + len > s.len) len = s.len - from;
    return {s.ptr + from, len};
}

wchar_t* wide(Arena* a, Str s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.ptr, (int)s.len, nullptr, 0);
    wchar_t* out = PUSH_ARRAY(a, wchar_t, n + 1);
    MultiByteToWideChar(CP_UTF8, 0, s.ptr, (int)s.len, out, n);
    return out;
}

Str narrow(Arena* a, const wchar_t* w, i64 len) {
    if (len < 0) len = (i64)wcslen(w);
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
    char* out = PUSH_ARRAY(a, char, n + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, (int)len, out, n, nullptr, nullptr);
    return {out, n};
}

void set(char* dst, i64 cap, Str s) {
    i64 n = s.len < cap - 1 ? s.len : cap - 1;
    // Don't cut a UTF-8 character in half.
    while (n > 0 && n < s.len && ((u8)s.ptr[n] & 0xC0) == 0x80) n--;
    if (n > 0) memcpy(dst, s.ptr, n);
    dst[n] = 0;
}

// ---------------------------------------------------------------------------------------------
// Names

bool names_has(Names* n, Str name) {
    for (i32 i = 0; i < n->count; i++)
        if (str_eq_nocase(str(n->items[i].s), name)) return true;
    return false;
}

void names_set(Names* n, Str name, bool on) {
    for (i32 i = 0; i < n->count; i++)
        if (str_eq_nocase(str(n->items[i].s), name)) list_remove(n, i--);
    if (!on) return;
    Name item = {};
    SET(item.s, name);
    for (char* c = item.s; *c; c++) *c = lower(*c);
    i32 at = 0;
    while (at < n->count && strcmp(n->items[at].s, item.s) < 0) at++;
    list_push(n);
    memmove(&n->items[at + 1], &n->items[at], sizeof(Name) * (n->count - 1 - at));
    n->items[at] = item;
}

// ---------------------------------------------------------------------------------------------
// Time

f64 now_ts() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (f64)(t - 116444736000000000ULL) / 1e7;
}

LocalTime local_time(f64 ts) {
    __time64_t t = (__time64_t)floor(ts);
    tm parts = {};
    _localtime64_s(&parts, &t);
    return {parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec,
            (parts.tm_wday + 6) % 7};
}

f64 local_ts(i32 year, i32 month, i32 day, i32 hour, i32 minute) {
    tm parts = {};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_isdst = -1;
    return (f64)_mktime64(&parts);
}

void day_of(f64 ts, char out[11]) {
    LocalTime t = local_time(ts - 4 * 3600.0);
    snprintf(out, 11, "%04d-%02d-%02d", t.year, t.month, t.day);
}

const char* const WEEKDAYS[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
const char* const MONTHS[12] = {"January", "February", "March",     "April",   "May",      "June",
                                "July",    "August",   "September", "October", "November", "December"};

bool parse_day(const char* day, i32* y, i32* m, i32* d) {
    if (!day || strlen(day) != 10 || day[4] != '-' || day[7] != '-') return false;
    for (i32 i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdigit((u8)day[i])) return false;
    *y = atoi(day);
    *m = atoi(day + 5);
    *d = atoi(day + 8);
    return *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31;
}

void hm_of(f64 ts, char out[6]) {
    LocalTime t = local_time(ts);
    snprintf(out, 6, "%02d:%02d", t.hour, t.minute);
}

void day_bounds(const char* day, f64* from, f64* to) {
    i32 y = 0, m = 0, d = 0;
    CHECK(parse_day(day, &y, &m, &d), "a day as YYYY-MM-DD, not \"%s\"", day);
    *from = local_ts(y, m, d, 4, 0);
    *to = local_ts(y, m, d + 1, 4, 0);
}

// ---------------------------------------------------------------------------------------------
// Folders and files

static const wchar_t* known_folder(const KNOWNFOLDERID& id, wchar_t* buf) {
    if (buf[0]) return buf;
    PWSTR path = nullptr;
    CHECK(SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)), "Windows to name a standard folder");
    swprintf(buf, MAX_PATH, L"%s\\lifelog", path);
    CoTaskMemFree(path);
    CreateDirectoryW(buf, nullptr);
    return buf;
}

const wchar_t* roaming_dir() {
    static wchar_t buf[MAX_PATH];
    return known_folder(FOLDERID_RoamingAppData, buf);
}

bool shell_open(const wchar_t* file, const wchar_t* args) {
    SHELLEXECUTEINFOW info = {sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = file;
    info.lpParameters = args;
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != 0;
}

const wchar_t* local_dir() {
    static wchar_t buf[MAX_PATH];
    return known_folder(FOLDERID_LocalAppData, buf);
}

const wchar_t* exe_dir() {
    static wchar_t buf[MAX_PATH];
    if (!buf[0]) {
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        wchar_t* slash = wcsrchr(buf, L'\\');
        if (slash) *slash = 0;
    }
    return buf;
}

Str read_file(Arena* a, const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    char* data = PUSH_ARRAY(a, char, size.QuadPart + 1);
    DWORD got = 0;
    BOOL ok = ReadFile(f, data, (DWORD)size.QuadPart, &got, nullptr);
    CloseHandle(f);
    if (!ok) return {};
    return {data, (i64)got};
}

bool write_file(const wchar_t* path, Str data) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, data.ptr, (DWORD)data.len, &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == (DWORD)data.len;
}

// ---------------------------------------------------------------------------------------------
// Log

const wchar_t* log_path() {
    static wchar_t buf[MAX_PATH];
    if (buf[0]) return buf;
    // A test copy (LIFELOG_CONFIG set) logs next to its config, out of the real log.
    wchar_t config[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LIFELOG_CONFIG", config, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(config, L'\\');
        if (!slash) slash = wcsrchr(config, L'/');
        if (slash) *slash = 0;
        else wcscpy_s(config, L".");
        swprintf(buf, MAX_PATH, L"%s\\lifelog.log", config);
    } else {
        swprintf(buf, MAX_PATH, L"%s\\lifelog.log", local_dir());
    }
    return buf;
}

void log_open() {
    // Keep one previous log once it passes 5 MB.
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (GetFileAttributesExW(log_path(), GetFileExInfoStandard, &info) && info.nFileSizeLow > MB(5)) {
        wchar_t old[MAX_PATH];
        swprintf(old, MAX_PATH, L"%s.1", log_path());
        MoveFileExW(log_path(), old, MOVEFILE_REPLACE_EXISTING);
    }
    log_file = CreateFileW(log_path(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, 0, nullptr);
}

void log_line(const char* level, const char* fmt, ...) {
    char text[2048];
    LocalTime t = local_time(now_ts());
    int n = snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d [%s] ", t.year, t.month, t.day, t.hour,
                     t.minute, t.second, level);
    va_list args;
    va_start(args, fmt);
    int m = vsnprintf(text + n, sizeof(text) - n - 2, fmt, args);
    va_end(args);
    n += m < 0 ? 0 : (m < (int)sizeof(text) - n - 2 ? m : (int)sizeof(text) - n - 3);
    text[n++] = '\n';
    AcquireSRWLockExclusive(&log_lock);
    if (log_file != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(log_file, text, n, &wrote, nullptr);
    }
    // A terminal, when there is one (the CLI, or run by hand).
    HANDLE out = GetStdHandle(STD_ERROR_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(out, text, n, &wrote, nullptr);
    }
    ReleaseSRWLockExclusive(&log_lock);
}


void wav_header(u8 out[WAV_HEADER], u32 rate, u32 data_bytes) {
    u32 riff = 36 + data_bytes, fmt_len = 16, bytes_per_sec = rate * 2;
    u16 pcm = 1, mono = 1, align = 2, bits = 16;
    memcpy(out, "RIFF", 4);
    memcpy(out + 4, &riff, 4);
    memcpy(out + 8, "WAVEfmt ", 8);
    memcpy(out + 16, &fmt_len, 4);
    memcpy(out + 20, &pcm, 2);
    memcpy(out + 22, &mono, 2);
    memcpy(out + 24, &rate, 4);
    memcpy(out + 28, &bytes_per_sec, 4);
    memcpy(out + 32, &align, 2);
    memcpy(out + 34, &bits, 2);
    memcpy(out + 36, "data", 4);
    memcpy(out + 40, &data_bytes, 4);
}