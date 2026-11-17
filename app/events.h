// events.h: how other threads hand things to the main thread, which owns App.
//
// A thread that has a result posts it: an Event, holding the result as a named record (a struct of
// the poster's choosing), goes into a fixed ring behind a tiny lock, and the tray window gets a
// message so the main thread wakes, even with the window closed. The main thread takes the events
// in its message loop and applies them to App. A slot is free again as soon as its event is taken.
//
// A status that only matters as it is now (what the worker is doing, how far a download is) goes
// through a Latest instead: each post replaces the record waiting there, and only one event is in
// the ring for it at a time, so a fast poster can never fill the ring with stale updates.
//
// A bigger result (a list, a page of text) comes as a Job: a block in its own arena, filled by
// the thread that made it, applied by the main thread. Each reader holds a reference (the poster,
// the queue it waits in, a window that keeps the results), and the arena goes back to be reused
// (base.h) when the last one lets go. A job that owns something outside its arena (a handle, a
// file) names a cleanup, run then, whether it was applied, dropped or its thread gave up.
//
// Hot flags read by many threads (private mode, the GPU load) are atomics. The settings a thread
// needs are its own copy, handed over by the main thread when one changes (WorkerOptions in
// worker.h, SummaryOptions in summary.h).

#pragma once

static const UINT WM_APP_EVENT = WM_APP + 0x50;  // to the tray window: events are waiting

// The biggest record an event carries.
static const i32 EVENT_BYTES = 528;

// An event names the function that applies it, so each module keeps its own.
struct Event;
typedef void EventApply(App* app, Event* e);
struct Event {
    EventApply* apply;  // run on the main thread
    u64 size;
    alignas(8) u8 data[EVENT_BYTES];
};

// The record an event carries, as the type it was posted as.
template <typename T>
T* event_record(Event* e) {
    static_assert(sizeof(T) <= EVENT_BYTES, "an event record must fit EVENT_BYTES");
    return (T*)e->data;
}

struct EventQueue {
    SRWLOCK lock;   // guards the ring only; never held while anything else is taken
    Event items[512];
    u32 head;       // the next to take
    u32 tail;       // the next free slot
    u32 dropped;    // posted while the ring was full
    HWND window;    // woken with WM_APP_EVENT
};

// Any thread. False if the ring was full (the event is dropped and counted).
bool event_post(App* app, EventApply* apply, const void* record, u32 size);
// For an event the main thread must get (it ends a state, such as "setting up"): while the ring
// is full, waits and tries again. False only if the program stops first.
bool event_post_wait(App* app, EventApply* apply, const void* record, u32 size);

template <typename T>
bool event_send(App* app, EventApply* apply, const T& record) {
    static_assert(sizeof(T) <= EVENT_BYTES, "an event record must fit EVENT_BYTES");
    return event_post(app, apply, &record, sizeof(T));
}
template <typename T>
bool event_send_wait(App* app, EventApply* apply, const T& record) {
    static_assert(sizeof(T) <= EVENT_BYTES, "an event record must fit EVENT_BYTES");
    return event_post_wait(app, apply, &record, sizeof(T));
}
inline bool event_send(App* app, EventApply* apply) { return event_post(app, apply, 0, 0); }

// A status record that replaces the one before it. Zeroed is ready.
struct Latest {
    SRWLOCK lock;
    u32 waiting;    // its event is in the ring and has not been applied
    u32 size;
    alignas(8) u8 data[EVENT_BYTES];
};

// Any thread: `record` replaces what waits in `slot`; if nothing was waiting, `apply` is posted,
// and when it runs it takes the newest record with latest_take.
void latest_post(App* app, Latest* slot, EventApply* apply, const void* record, u32 size);
template <typename T>
void latest_send(App* app, Latest* slot, EventApply* apply, const T& record) {
    static_assert(sizeof(T) <= EVENT_BYTES, "a status record must fit EVENT_BYTES");
    latest_post(app, slot, apply, &record, sizeof(T));
}
// In the apply, on the main thread: copies out the newest record. The event's record is the slot.
void latest_take(Event* e, void* out, u32 size);
template <typename T>
T latest_take(Event* e) {
    T out;
    latest_take(e, &out, sizeof(T));
    return out;
}

struct Job;
typedef void JobApply(App* app, Job* job);
typedef void JobCleanup(Job* job);
struct Job {
    Arena arena;          // the job itself lives in it, with everything it points to
    JobApply* apply;
    void* data;
    JobCleanup* cleanup;  // gives back what the job owns outside its arena; may be null
    volatile LONG refs;
};

// A new job in its own (reused) arena, with one reference: the caller's. Fill job->data from
// job->arena, then post it, which hands that reference on.
Job* job_new(JobApply* apply);
// Another reader: it must call job_drop when done. Any thread.
Job* job_hold(Job* job);
// This reader is done; the last one runs the cleanup and gives the arena back. Any thread.
void job_drop(Job* job);
// To the main thread, which runs job->apply and then drops the reference posted with it. An apply
// that keeps the results holds the job (job_hold) and drops it when it lets them go.
void job_post(App* app, Job* job);
// For a job the main thread must get: while the queue is full, waits and tries again, until the
// program stops.
void job_post_wait(App* app, Job* job);

// The main thread: applies every waiting event.
void events_apply(App* app);

// Atomics for the flags threads share.
inline LONG atomic_get(volatile LONG* v) { return InterlockedCompareExchange(v, 0, 0); }
inline void atomic_set(volatile LONG* v, LONG x) { InterlockedExchange(v, x); }
