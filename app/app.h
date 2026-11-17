// app.h: App, the one value that holds what the program is doing. The main thread owns it: it is
// the only thread that reads or changes it, so nothing locks it and a frame always sees one
// moment. Other threads get what they need in a job, from atomics or from their own copy of the
// settings (worker_options_publish, summary_options_publish), and hand their results back as
// events and jobs (events.h); slow work runs on the database thread (db.h), the file writer
// (disk.h) and the pool (pool.h).
//
// Functions that take `App*` are for the main thread unless their comment says otherwise. When
// another thread changes something the window shows, the main thread calls wake_window() as it
// applies the change; nothing polls.

#pragma once

struct App {
    HANDLE stop;  // manual-reset event: set when the program is ending
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
    Voice voice;        // voice commands (voice.h)
    EventQueue events;  // results from other threads, for the main thread (events.h)
};

// Ask the open window (if any) to draw again. Any thread.
void wake_window(App* app);
// Whether the program is ending; for loops in background threads.
bool stopping(App* app);
// Sleeps up to `ms`, or less if the program ends. False when it is ending.
bool sleep_unless_stopping(App* app, u32 ms);

// Starts a background thread running `fn(app)`.
void start_thread(App* app, const char* name, LPTHREAD_START_ROUTINE fn);  // fn gets the App*
