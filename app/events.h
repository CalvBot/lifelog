// events.h: how other threads hand things to the main thread, which owns App.
//
// A thread that has a result posts it: a small Event goes into a fixed ring behind a tiny lock,
// and the tray window gets a message so the main thread wakes, even with the window closed. The
// main thread takes the events in its message loop and applies them to App.
//
// A bigger result (a list, a page of text) comes as a Job: a block in its own arena, filled by
// the thread that made it, applied by the main thread, then freed whole.
//
// Hot flags read by many threads (private mode, the GPU load) are atomics, and the settings a
// thread needs come from settings(): a copy of App.config that the thread may keep reading.
//
// Until the move is done (SPEC.md, "Threads"), App.lock still guards the rest of App, so the
// main thread applies events with the lock held.

#pragma once

static const UINT WM_APP_EVENT = WM_APP + 0x50;  // to the tray window: events are waiting

// An event names the function that applies it, so each module keeps its own.
struct Event;
typedef void EventApply(App* app, Event* e);
struct Event {
    EventApply* apply;  // run on the main thread, with App.lock for now
    i64 a;
    i64 b;
    char text[512];
};

struct EventQueue {
    SRWLOCK lock;   // guards the ring only; never held while anything else is taken
    Event items[512];
    u32 head;       // the next to take
    u32 tail;       // the next free slot
    u32 dropped;    // posted while the ring was full
    HWND window;    // woken with WM_APP_EVENT
};

struct Job;
typedef void JobApply(App* app, Job* job);
struct Job {
    Arena arena;    // the job itself lives in it, with everything it points to
    JobApply* apply;
    void* data;
};

// Any thread. False if the ring was full (the event is dropped and counted).
bool event_post(App* app, EventApply* apply, i64 a, i64 b, const char* text);

// A new job in its own arena. Fill job->data from job->arena, then post it; once posted it is
// the main thread's.
Job* job_new(JobApply* apply);
void job_post(App* app, Job* job);
void job_free(Job* job);

// The main thread: applies every waiting event, taking App.lock around each.
void events_apply(App* app);

// Atomics for the flags threads share without App.lock.
inline LONG atomic_get(volatile LONG* v) { return InterlockedCompareExchange(v, 0, 0); }
inline void atomic_set(volatile LONG* v, LONG x) { InterlockedExchange(v, x); }

// The settings now: a copy of App.config without its app lists (those stay with the main
// thread). Each change makes a new copy; old ones are kept until exit, so a thread may hold on
// to the one it has. Any thread, no lock.
const Config* settings(App* app);
// Makes a new copy if App.config changed, and says so. With the lock (app_unlock calls it).
bool settings_publish(App* app);
