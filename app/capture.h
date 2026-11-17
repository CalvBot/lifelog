// capture.h: recording. One stream for the mic and one per app that plays sound (WASAPI process
// loopback), each cut into short 16 kHz mono WAV files in the spool folder with a .json beside
// it for the worker. Excluded apps and private mode record nothing.

#pragma once

enum Source : u8 {
    SOURCE_MIC,
    SOURCE_APP,
};

// Made and freed by the main thread; its capture thread reads it.
struct Stream {
    Source source;
    char exe[64];       // "" for the mic
    u32 pid;            // the process tree's root
    wchar_t spool[MAX_PATH];  // where its WAVs go, and the loudness gate: set before it starts
    f32 gate;
    f64 playing_ts;     // the last time its app had a sound session playing (the main thread's)
    SRWLOCK lock;       // guards the three below, which both threads use
    char title[256];    // the app's window title, kept current by the main thread
    char device[128];   // the mic's name, from the capture thread
    f64 loud_ts;        // the last time it was louder than the gate, from the capture thread
    App* app;
    HANDLE thread;
    volatile LONG stop;
    volatile LONG discard;  // drop the sentence in progress when stopping (the app was just excluded)
};

struct Capture {
    volatile LONG paused;    // private mode: nothing is recorded, the sentence in progress is dropped
    List<Stream*> streams;   // the mic first, then apps; a Stream lives until its thread ends
    List<Stream*> ending;    // told to stop, their threads still writing out; freed once ended
    HANDLE watcher;          // the thread that starts and stops app streams
    SRWLOCK mic_lock;        // guards mic_want
    wchar_t mic_want[256];   // config.capture.mic_device, for the mic's thread: "" is the default
};

struct MicDevice {
    char id[256];
    char name[128];
};

// Starts the mic and the watcher that starts and stops a stream per app.
void capture_start(App* app);
// Main thread: record this mic ("" for the Windows default), saved; the mic's stream switches
// within 3 seconds.
void capture_set_mic(App* app, Str id, Str name);
// Main thread: the mics plugged in now, up to `cap`, and the Windows default's name into
// `default_name`. Returns how many.
i32 capture_mics(MicDevice* out, i32 cap, char* default_name, i32 name_cap);
// Private mode on or off; the tray icon follows.
void capture_set_paused(App* app, bool paused);
// Stops every stream and writes out what each had (when the program ends). It lets go of the
// lock while the threads finish, so others are not held up, and has it again when it returns.
void capture_stop(App* app);
