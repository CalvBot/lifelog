// events.cpp: see events.h.

bool event_post(App* app, EventKind kind, i64 a, i64 b, const char* text) {
    EventQueue* q = &app->events;
    AcquireSRWLockExclusive(&q->lock);
    bool full = q->tail - q->head >= (u32)COUNT(q->items);
    bool was_empty = q->tail == q->head;
    if (full) {
        q->dropped++;
    } else {
        Event* e = &q->items[q->tail % COUNT(q->items)];
        *e = {};
        e->kind = kind;
        e->a = a;
        e->b = b;
        if (text) SET(e->text, str(text));
        q->tail++;
    }
    HWND w = q->window;
    ReleaseSRWLockExclusive(&q->lock);
    // The main thread takes every event once woken, so one message per empty-to-not is enough.
    if (!full && was_empty && w) PostMessageW(w, WM_APP_EVENT, 0, 0);
    return !full;
}

static bool event_take(EventQueue* q, Event* out, u32* dropped) {
    AcquireSRWLockExclusive(&q->lock);
    bool have = q->head != q->tail;
    if (have) {
        *out = q->items[q->head % COUNT(q->items)];
        q->head++;
    }
    *dropped = q->dropped;
    q->dropped = 0;
    ReleaseSRWLockExclusive(&q->lock);
    return have;
}

Job* job_new(JobApply* apply) {
    Arena a = {};
    Job* job = PUSH(&a, Job);
    job->arena = a;  // from here on the arena is job->arena
    job->apply = apply;
    return job;
}

void job_free(Job* job) {
    Arena a = job->arena;  // the job is inside it
    arena_release(&a);
}

void job_post(App* app, Job* job) {
    if (!event_post(app, EVENT_JOB, (i64)job, 0, 0)) job_free(job);
}

void events_apply(App* app) {
    Event e;
    u32 dropped = 0;
    while (event_take(&app->events, &e, &dropped)) {
        app_lock(app);
        if (e.kind == EVENT_JOB) {
            Job* job = (Job*)e.a;
            job->apply(app, job);
            job_free(job);
        }
        app_unlock(app);
    }
    if (dropped) WARN("events: %u dropped, the queue was full", dropped);
}

// ---------------------------------------------------------------------------------------------
// Settings

static Config* volatile g_settings;

const Config* settings(App*) {
    return (const Config*)InterlockedCompareExchangePointer((PVOID volatile*)&g_settings, 0, 0);
}

void settings_publish(App* app) {
    Config now;
    memcpy(&now, &app->config, sizeof(Config));
    now.capture.included_apps = {};
    now.capture.excluded_apps = {};
    now.capture.removed_apps = {};
    now.capture.included_folders = {};
    now.capture.excluded_folders = {};
    now.processing.games = {};
    now.processing.not_games = {};
    now.ui.hidden = {};
    Config* cur = g_settings;
    if (cur && memcmp(cur, &now, sizeof(Config)) == 0) return;
    Config* next = (Config*)malloc(sizeof(Config));
    CHECK(next, "memory for the settings");
    memcpy(next, &now, sizeof(Config));
    InterlockedExchangePointer((PVOID volatile*)&g_settings, next);
}
