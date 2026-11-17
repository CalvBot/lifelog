// events.cpp: see events.h.

bool event_post(App* app, EventApply* apply, const void* record, u32 size) {
    CHECK(size <= (u32)EVENT_BYTES, "an event record of at most %d bytes, not %u", EVENT_BYTES, size);
    EventQueue* q = &app->events;
    AcquireSRWLockExclusive(&q->lock);
    bool full = q->tail - q->head >= (u32)COUNT(q->items);
    bool was_empty = q->tail == q->head;
    if (full) {
        q->dropped++;
    } else {
        Event* e = &q->items[q->tail % COUNT(q->items)];
        e->apply = apply;
        e->size = size;
        if (size) memcpy(e->data, record, size);
        q->tail++;
    }
    HWND w = q->window;
    ReleaseSRWLockExclusive(&q->lock);
    // The main thread takes every event once woken, so one message per empty-to-not is enough.
    if (!full && was_empty && w) PostMessageW(w, WM_APP_EVENT, 0, 0);
    return !full;
}

bool event_post_wait(App* app, EventApply* apply, const void* record, u32 size) {
    while (!event_post(app, apply, record, size))
        if (WaitForSingleObject(app->stop, 50) == WAIT_OBJECT_0) return false;
    return true;
}

static bool event_take(EventQueue* q, Event* out, u32* dropped) {
    AcquireSRWLockExclusive(&q->lock);
    bool have = q->head != q->tail;
    if (have) {
        Event* e = &q->items[q->head % COUNT(q->items)];
        out->apply = e->apply;
        out->size = e->size;
        if (e->size) memcpy(out->data, e->data, e->size);
        q->head++;
    }
    *dropped = q->dropped;
    q->dropped = 0;
    ReleaseSRWLockExclusive(&q->lock);
    return have;
}

// ---------------------------------------------------------------------------------------------
// Latest

void latest_post(App* app, Latest* slot, EventApply* apply, const void* record, u32 size) {
    CHECK(size <= (u32)EVENT_BYTES, "a status record of at most %d bytes, not %u", EVENT_BYTES, size);
    AcquireSRWLockExclusive(&slot->lock);
    slot->size = size;
    memcpy(slot->data, record, size);
    bool post = !slot->waiting;
    slot->waiting = true;
    ReleaseSRWLockExclusive(&slot->lock);
    if (post && !event_send(app, apply, slot)) {
        // The ring was full: the next post tries again.
        AcquireSRWLockExclusive(&slot->lock);
        slot->waiting = false;
        ReleaseSRWLockExclusive(&slot->lock);
    }
}

void latest_take(Event* e, void* out, u32 size) {
    Latest* slot = *event_record<Latest*>(e);
    AcquireSRWLockExclusive(&slot->lock);
    CHECK(slot->size == size, "a status record of %u bytes, not %u", slot->size, size);
    memcpy(out, slot->data, size);
    slot->waiting = false;
    ReleaseSRWLockExclusive(&slot->lock);
}

// ---------------------------------------------------------------------------------------------
// Jobs

Job* job_new(JobApply* apply) {
    Arena a = {};
    Job* job = PUSH(&a, Job);
    job->arena = a;  // from here on the arena is job->arena
    job->apply = apply;
    job->refs = 1;
    return job;
}

Job* job_hold(Job* job) {
    if (job) InterlockedIncrement(&job->refs);
    return job;
}

void job_drop(Job* job) {
    if (!job) return;
    LONG left = InterlockedDecrement(&job->refs);
    CHECK(left >= 0, "a job dropped once more than it was held");
    if (left) return;
    if (job->cleanup) job->cleanup(job);
    Arena a = job->arena;  // the job is inside it
    arena_release(&a);
}

static void job_apply(App* app, Event* e) {
    Job* job = *event_record<Job*>(e);
    if (job->apply) job->apply(app, job);
    job_drop(job);
}

void job_post(App* app, Job* job) {
    if (!event_send(app, job_apply, job)) job_drop(job);
}

void job_post_wait(App* app, Job* job) {
    if (!event_send_wait(app, job_apply, job)) job_drop(job);
}

void events_apply(App* app) {
    Event e;
    u32 dropped = 0;
    while (event_take(&app->events, &e, &dropped)) e.apply(app, &e);
    if (dropped) WARN("events: %u dropped, the queue was full", dropped);
}
