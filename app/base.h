// base.h: the few things every file uses. Types, CHECK, arenas, strings, lists, time and the log.
//
// The rules for the whole program (see SPEC.md):
//   - plain structs and free functions; no classes with behaviour, no inheritance, no virtuals;
//   - nothing is cleaned up in a destructor: memory comes from arenas, handles are closed by hand;
//   - every struct is valid when zeroed (`Foo foo = {};`), so a missing value reads as zero;
//   - CHECK states what was expected when it is not true, and where.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <wchar.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;

#define COUNT(a) ((i32)(sizeof(a) / sizeof((a)[0])))
#define KB(n) ((u64)(n) << 10)
#define MB(n) ((u64)(n) << 20)
#define GB(n) ((u64)(n) << 30)

// ---------------------------------------------------------------------------------------------
// CHECK: for things that must be true. When one is not, the log gets what was expected, what
// came instead (the message), and the file and line; then the program stops.

[[noreturn]] void fail(const char* file, int line, const char* cond, const char* fmt, ...);
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) fail(__FILE__, __LINE__, #cond, __VA_ARGS__); \
    } while (0)

// ---------------------------------------------------------------------------------------------
// Arena: memory handed out in order and freed all at once. A zeroed Arena reserves its address
// space on first use; pages are committed as they are reached.

struct Arena {
    u8* base;
    u64 used;
    u64 committed;
    u64 reserved;
};

void* arena_push(Arena* a, u64 size, u64 align = 8);   // zeroed
void arena_reset(Arena* a);                             // keeps the pages for next time
void arena_release(Arena* a);                           // gives them back
#define PUSH(arena, T) ((T*)arena_push((arena), sizeof(T), alignof(T)))
#define PUSH_ARRAY(arena, T, n) ((T*)arena_push((arena), sizeof(T) * (u64)(n), alignof(T)))

// A per-thread arena for work that ends with the function: take its position, use it, put it back.
Arena* scratch();
struct Mark {
    Arena* arena;
    u64 used;
};
inline Mark mark(Arena* a) { return {a, a->used}; }
inline void rewind(Mark m) { m.arena->used = m.used; }

// ---------------------------------------------------------------------------------------------
// Str: UTF-8 text that does not own its bytes. Strings made by the functions here are also
// zero-terminated, so `.ptr` can go straight to C functions.

struct Str {
    const char* ptr;
    i64 len;
};

inline Str str(const char* s) { return {s, s ? (i64)strlen(s) : 0}; }
Str str_copy(Arena* a, Str s);
Str str_fmt(Arena* a, const char* fmt, ...);
Str str_lower(Arena* a, Str s);
bool str_eq(Str a, Str b);
bool str_eq_nocase(Str a, Str b);
bool str_starts(Str s, Str prefix);
bool str_ends(Str s, Str suffix);
Str str_trim(Str s);
i64 str_find(Str s, Str needle);   // -1 if not there
Str str_sub(Str s, i64 from, i64 len = -1);
// `s` without a trailing ".exe" (any case): "Discord.EXE" is "Discord".
Str strip_exe(Str s);
wchar_t* wide(Arena* a, Str s);     // for Windows calls
Str narrow(Arena* a, const wchar_t* w, i64 len = -1);

// Copies `s` into a fixed buffer, cutting it if it does not fit (always zero-terminated).
void set(char* dst, i64 cap, Str s);
#define SET(buf, s) set((buf), sizeof(buf), (s))

// ---------------------------------------------------------------------------------------------
// List: a growable array on the heap. Zeroed is empty. list_free gives the memory back.

template <typename T>
struct List {
    T* items;
    i32 count;
    i32 cap;
    T& operator[](i32 i) {
        CHECK(i >= 0 && i < count, "index %d inside a list of %d", i, count);
        return items[i];
    }
};

template <typename T>
T* list_push(List<T>* l, T item = {}) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (T*)realloc(l->items, sizeof(T) * l->cap);
        CHECK(l->items, "memory for a list of %d", l->cap);
    }
    l->items[l->count] = item;
    return &l->items[l->count++];
}

template <typename T>
void list_remove(List<T>* l, i32 i) {
    CHECK(i >= 0 && i < l->count, "index %d inside a list of %d", i, l->count);
    memmove(&l->items[i], &l->items[i + 1], sizeof(T) * (l->count - i - 1));
    l->count--;
}

template <typename T>
void list_free(List<T>* l) {
    free(l->items);
    *l = {};
}

// ---------------------------------------------------------------------------------------------
// Names: short names (exe names, speakers) kept in fixed slots, for the config's lists.

struct Name {
    char s[64];
};
// A folder's full path, for lists of folders.
struct PathName {
    char s[MAX_PATH];
};
typedef List<PathName> Folders;
typedef List<Name> Names;
bool names_has(Names* n, Str name);           // ignoring case
void names_set(Names* n, Str name, bool on);  // adds or removes; kept sorted

// ---------------------------------------------------------------------------------------------
// Time. Timestamps are unix seconds (UTC) as f64, as in the database.

f64 now_ts();
// Local date and time parts of `ts`.
struct LocalTime {
    i32 year, month, day, hour, minute, second, weekday;  // weekday 0 = Monday
};
LocalTime local_time(f64 ts);
f64 local_ts(i32 year, i32 month, i32 day, i32 hour, i32 minute);  // local wall clock to unix
// A journal day runs 04:00 to 04:00 local time. "YYYY-MM-DD" of the day `ts` falls in.
void day_of(f64 ts, char out[11]);
void day_bounds(const char* day, f64* from, f64* to);
// A day as "YYYY-MM-DD", checked. False if it is not one.
bool parse_day(const char* day, i32* y, i32* m, i32* d);
// "14:05", local time.
void hm_of(f64 ts, char out[6]);
extern const char* const WEEKDAYS[7];  // "Monday" first, as LocalTime.weekday
extern const char* const MONTHS[12];

// ---------------------------------------------------------------------------------------------
// Log: %LOCALAPPDATA%\lifelog\lifelog.log, one line per call, from any thread.

void log_open();
void log_line(const char* level, const char* fmt, ...);
#define LOG(...) log_line("INFO", __VA_ARGS__)
#define WARN(...) log_line("WARN", __VA_ARGS__)
#define ERR(...) log_line("ERROR", __VA_ARGS__)
const wchar_t* log_path();

// Folders: %APPDATA%\lifelog (settings) and %LOCALAPPDATA%\lifelog (log, models, icons).
const wchar_t* roaming_dir();
const wchar_t* local_dir();
// The folder lifelog.exe is in.
const wchar_t* exe_dir();
// Opens `file` (a document, folder, web address or program, with `args`, may be null) the way
// Explorer would. Waits until the shell has handed it over, which a Store app (Notepad) needs from
// a thread with no message loop. False if it could not.
bool shell_open(const wchar_t* file, const wchar_t* args);
// Opens `path` with whatever is registered (a folder in the file manager), or with `in_folder`
// shows it selected in Explorer. False if the shell could not.
bool shell_show(const wchar_t* path, bool in_folder);

// The header of a 16-bit mono PCM WAV holding `data_bytes` of samples at `rate`. Every WAV lifelog
// writes (spool, kept audio, copies, chimes) has exactly this header, so its samples start at
// WAV_HEADER.
static const i32 WAV_HEADER = 44;
void wav_header(u8 out[WAV_HEADER], u32 rate, u32 data_bytes);

// Whole files.
Str read_file(Arena* a, const wchar_t* path);   // .ptr is null if it could not be read
bool write_file(const wchar_t* path, Str data);
