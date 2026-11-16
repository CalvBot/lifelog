// pool.cpp: see pool.h. One list of tasks behind a small lock, and a semaphore that counts them.

struct PoolTask {
    PoolTask* next;
    Job* job;  // the task lives in the job's arena
    PoolRun* run;
};

static const i32 POOL_THREADS = 3;

static struct {
    SRWLOCK lock;  // guards head and tail
    PoolTask* head;
    PoolTask* tail;
    HANDLE ready;  // a semaphore: one count per task in the list
    HANDLE threads[POOL_THREADS];
    App* app;
} g_pool;

static DWORD WINAPI pool_main(void*) {
    App* app = g_pool.app;
    CoInitializeEx(0, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HANDLE waits[2] = {app->stop, g_pool.ready};
    while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        AcquireSRWLockExclusive(&g_pool.lock);
        PoolTask* t = g_pool.head;
        if (t) {
            g_pool.head = t->next;
            if (!g_pool.head) g_pool.tail = 0;
        }
        ReleaseSRWLockExclusive(&g_pool.lock);
        if (!t) continue;
        Job* job = t->job;
        Mark m = mark(scratch());
        t->run(job);
        rewind(m);
        job_post_wait(app, job);
    }
    CoUninitialize();
    return 0;
}

void pool_start(App* app) {
    g_pool.app = app;
    g_pool.ready = CreateSemaphoreW(0, 0, 1 << 20, 0);
    CHECK(g_pool.ready, "the pool's semaphore (error %lu)", GetLastError());
    for (i32 i = 0; i < POOL_THREADS; i++) {
        g_pool.threads[i] = CreateThread(0, 0, pool_main, 0, 0, 0);
        CHECK(g_pool.threads[i], "a pool thread to start (error %lu)", GetLastError());
        SetThreadDescription(g_pool.threads[i], L"pool");
    }
}

void pool_stop() {
    if (!g_pool.threads[0]) return;
    // A thread in the folder picker or the shell may take a while; it is not waited for long.
    WaitForMultipleObjects(POOL_THREADS, g_pool.threads, TRUE, 5000);
    for (i32 i = 0; i < POOL_THREADS; i++) {
        CloseHandle(g_pool.threads[i]);
        g_pool.threads[i] = 0;
    }
    AcquireSRWLockExclusive(&g_pool.lock);
    for (PoolTask* t = g_pool.head; t;) {
        PoolTask* next = t->next;
        job_free(t->job);
        t = next;
    }
    g_pool.head = g_pool.tail = 0;
    ReleaseSRWLockExclusive(&g_pool.lock);
}

void pool_post(Job* job, PoolRun* run) {
    CHECK(g_pool.ready, "pool_post after pool_start");
    PoolTask* t = PUSH(&job->arena, PoolTask);
    t->job = job;
    t->run = run;
    AcquireSRWLockExclusive(&g_pool.lock);
    if (g_pool.tail) g_pool.tail->next = t;
    else g_pool.head = t;
    g_pool.tail = t;
    ReleaseSRWLockExclusive(&g_pool.lock);
    ReleaseSemaphore(g_pool.ready, 1, 0);
}
