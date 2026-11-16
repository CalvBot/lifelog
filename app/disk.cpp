// disk.cpp: see disk.h. A list of writes behind a small lock, and a thread that does them.

struct DiskOp {
    Arena arena;  // the op lives in it
    DiskOp* next;
    bool append;  // else a config.toml key
    char table[64];
    char key[64];
    wchar_t file[MAX_PATH];
    Str text;
};

static struct {
    SRWLOCK lock;  // guards the fields below
    DiskOp* head;
    DiskOp* tail;
    bool running;  // the thread takes the ops; else they are done at once
    bool closing;  // disk_stop was called: the thread ends once the list is empty
    HANDLE wake;
    HANDLE thread;
} g_disk;

static void disk_write_append(const wchar_t* file, Str text) {
    HANDLE f = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ, 0, OPEN_ALWAYS, 0, 0);
    if (f == INVALID_HANDLE_VALUE) {
        Mark m = mark(scratch());
        WARN("could not add to %s (error %lu)", narrow(scratch(), file).ptr, GetLastError());
        rewind(m);
        return;
    }
    DWORD wrote = 0;
    WriteFile(f, text.ptr, (DWORD)text.len, &wrote, 0);
    CloseHandle(f);
}

static void disk_do(DiskOp* op) {
    if (op->append) disk_write_append(op->file, op->text);
    else config_write_value(op->table, op->key, op->text);
}

static void disk_free(DiskOp* op) {
    Arena a = op->arena;  // the op is inside it
    arena_release(&a);
}

static void disk_post(DiskOp* op) {
    AcquireSRWLockExclusive(&g_disk.lock);
    bool queued = g_disk.running;
    if (queued) {
        if (g_disk.tail) g_disk.tail->next = op;
        else g_disk.head = op;
        g_disk.tail = op;
    }
    ReleaseSRWLockExclusive(&g_disk.lock);
    if (queued) {
        SetEvent(g_disk.wake);
        return;
    }
    disk_do(op);
    disk_free(op);
}

static DiskOp* disk_op(Str text) {
    Arena a = {};
    DiskOp* op = PUSH(&a, DiskOp);
    op->text = str_copy(&a, text);
    op->arena = a;  // from here on the arena is op->arena
    return op;
}

void disk_save_key(const char* table, const char* key, Str value) {
    DiskOp* op = disk_op(value);
    SET(op->table, str(table));
    SET(op->key, str(key));
    disk_post(op);
}

void disk_append(const wchar_t* file, Str text) {
    if (!text.len) return;
    DiskOp* op = disk_op(text);
    op->append = true;
    wcsncpy_s(op->file, MAX_PATH, file, _TRUNCATE);
    disk_post(op);
}

static DWORD WINAPI disk_main(void*) {
    for (;;) {
        AcquireSRWLockExclusive(&g_disk.lock);
        DiskOp* ops = g_disk.head;
        g_disk.head = g_disk.tail = 0;
        // Ending under the lock: a write asked for after this is done at once instead.
        bool end = !ops && g_disk.closing;
        if (end) g_disk.running = false;
        ReleaseSRWLockExclusive(&g_disk.lock);
        if (end) break;
        while (ops) {
            DiskOp* next = ops->next;
            disk_do(ops);
            disk_free(ops);
            ops = next;
        }
        WaitForSingleObject(g_disk.wake, INFINITE);
    }
    return 0;
}

void disk_start() {
    g_disk.wake = CreateEventW(0, FALSE, FALSE, 0);
    CHECK(g_disk.wake, "the file writer's event (error %lu)", GetLastError());
    g_disk.running = true;
    g_disk.thread = CreateThread(0, 0, disk_main, 0, 0, 0);
    CHECK(g_disk.thread, "the file writer to start (error %lu)", GetLastError());
    SetThreadDescription(g_disk.thread, L"file writer");
}

void disk_stop() {
    if (!g_disk.thread) return;
    AcquireSRWLockExclusive(&g_disk.lock);
    g_disk.closing = true;
    ReleaseSRWLockExclusive(&g_disk.lock);
    SetEvent(g_disk.wake);
    WaitForSingleObject(g_disk.thread, INFINITE);
    CloseHandle(g_disk.thread);
    g_disk.thread = 0;
}
