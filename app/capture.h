// capture.h: recording. One stream for the mic and one per app that plays sound (WASAPI process
// loopback), each cut into short 16 kHz mono WAV files in the spool folder with a .json beside
// it for the worker. Excluded apps and private mode record nothing.

#pragma once

enum Source : u8 {
    SOURCE_MIC,
    SOURCE_APP,
};

struct Stream {
    Source source;
    char exe[64];       // "" for the mic
    u32 pid;            // the process tree's root
    char title[256];    // the app's window title, kept current
    char device[128];   // the mic's name
    f64 started_ts;
    f64 loud_ts;        // the last time it was louder than the gate
    // The capture thread's own, not read by others:
    HANDLE thread;
    volatile LONG stop;
    void* state;
};

struct Capture {
    bool paused;             // private mode: nothing is recorded, the sentence in progress is dropped
    List<Stream*> streams;   // the mic first, then apps; a Stream lives until its thread ends
};

// Starts the mic and the watcher that starts and stops a stream per app.
void capture_start(App* app);
void capture_set_paused(App* app, bool paused);
// Stops every stream and writes out what each had (when the program ends).
void capture_stop(App* app);
