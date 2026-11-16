// app.h: App, the one value that holds what the program is doing. The main thread owns it;
// other threads hand it their results as events (events.h). That move is under way (SPEC.md,
// "Threads"): capture and the GPU monitor are done, the rest still share App under App.lock.
//
// Locking: App.lock guards everything in App. A thread that reads or changes App takes it
// (app_lock / app_unlock) for as short a time as it can, and never while it waits on the disk,
// the network or the GPU: slow work is done first, unlocked, and the result stored after.
// The window holds the lock while it draws a frame, so what it shows is always one moment.
//
// Functions named `app_*` and the module functions that take `App*` expect the caller to hold
// the lock, unless their comment says otherwise. When a background thread changes something the
// window shows, it calls wake_window() so the window draws again; nothing polls.

#pragma once

struct App {
    SRWLOCK lock;
    HANDLE stop;  // manual-reset event: set when the program is ending
    bool quitting;
    bool restarting;

    Config config;
    Apps apps;          // every app seen (apps.h)
    Capture capture;    // the mic and app streams being recorded (capture.h)
    Worker worker;      // transcription (worker.h)
    Storage storage;    // the database (db.h)
    Summary summary;    // the daily summary and reminders (summary.h)
    Setup setup;        // downloading the transcriber and the summary AI (setup.h)
    Clips clips;        // copying, pasting and playback (clip.h)
    Ui ui;              // the window and the tray menu (ui.h)
    EventQueue events;  // results from other threads, for the main thread (events.h)
};

void app_lock(App* app);
void app_unlock(App* app);
// Ask the open window (if any) to draw again. Any thread, with or without the lock.
void wake_window(App* app);
// Whether the program is ending; for loops in background threads.
bool stopping(App* app);
// Sleeps up to `ms`, or less if the program ends. False when it is ending.
bool sleep_unless_stopping(App* app, u32 ms);

// Starts a background thread running `fn(app)`.
void start_thread(App* app, const char* name, LPTHREAD_START_ROUTINE fn);  // fn gets the App*
