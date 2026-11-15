// capture.h: recording. One stream for the mic and one per app that plays sound (WASAPI process
// loopback), each cut into short 16 kHz mono WAV files in the spool folder with a .json beside
// it for the worker. Excluded apps and private mode record nothing.
//
// While the lifelog-vencord plugin records a Discord call one track per person (it keeps
// <data_dir>\discord-calls.active fresh), Discord's mixed sound is not recorded: it would only
// repeat the same speech.

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
    f64 playing_ts;     // the last time its app had a sound session playing (the watcher's)
    // The capture thread's own, not read by others:
    App* app;
    HANDLE thread;
    volatile LONG stop;
    volatile LONG discard;  // drop the sentence in progress when stopping (the app was just excluded)
    void* state;
};

struct Capture {
    bool paused;             // private mode: nothing is recorded, the sentence in progress is dropped
    List<Stream*> streams;   // the mic first, then apps; a Stream lives until its thread ends
    List<Stream*> ending;    // told to stop, their threads still writing out; freed once ended
    HANDLE watcher;          // the thread that starts and stops app streams
};

// Starts the mic and the watcher that starts and stops a stream per app.
void capture_start(App* app);
void capture_set_paused(App* app, bool paused);
// Stops every stream and writes out what each had (when the program ends). It lets go of the
// lock while the threads finish, since they take it, and has it again when it returns.
void capture_stop(App* app);
