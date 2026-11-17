// worker.h: transcription. Takes the WAV files the capture leaves in the spool folder, oldest
// first, has them transcribed and writes the lines to the database. A clip with speech then moves
// to <data_dir>\audio\YYYY-MM-DD\ so its lines can be played back; one without is deleted.
//
// The transcribing is done by whisper.cpp in this process (whisper.dll, downloaded from
// Settings, see setup.h), or by any OpenAI-compatible transcription server set as [worker]
// endpoint. Our model is on the GPU only while it may be: in auto mode it is freed while a game
// is running or the 3D load is over the limit (a clip in progress is cut short and done again
// later), and loaded again once the GPU has been quiet for `resume_after_secs`. While the
// summary writes, it steps aside too, so the two models never share the GPU.
//
// The mode, limit and wait are in App.config.processing; the functions below change them there
// and in config.toml, and hand the transcription threads their own copy (WorkerOptions). The
// supervisor posts its state, the queue and "lines written" to the main thread (events.h), which
// keeps the fields below.

#pragma once

// What the supervisor and the GPU monitor need of the settings.
struct WorkerOptions {
    Mode mode;
    u32 gpu_busy_percent;
    bool pause_for_games;
    u32 resume_after_secs;
    char model[128];
    char language[16];
    char endpoint[512];
    char api_key[256];
    char ai_dir[MAX_PATH];
};

struct Worker {
    char state[160];      // what it is doing, for the window: "transcribing", "waiting for the game to close", ...
    i32 queued;           // WAV files waiting in the spool folder
    f64 queued_secs;      // how much audio that is
    // Atomics (events.h): the GPU monitor writes the first two itself.
    volatile LONG gpu_3d_percent;   // the 3D engine's load now, as in Task Manager
    volatile LONG gpu_busy;         // over the limit
    volatile LONG summary_needs_gpu;  // set by the summary while its model is (to be) on the GPU
    volatile LONG live_queued;      // `queued` and `running` as the supervisor has them now, for
    volatile LONG live_running;     // the summary writer
    HANDLE live_changed;  // auto-reset: set when live_queued or live_running change
    HANDLE gpu_free;      // manual-reset: set while our model is not on the GPU; the summary
                          // waits for it after setting summary_needs_gpu
    u64 lines_version;    // bumped each time lines are written, so the timeline reloads
    HANDLE done;          // set when the supervisor has stopped and freed the model
    HANDLE wake;          // auto-reset: something the supervisor waits on changed (worker_wake)
    Latest seen;          // the supervisor's newest status, for the main thread (events.h)
    // The options as the main thread last handed them over; each thread copies them when
    // `options_version` moves.
    SRWLOCK options_lock;
    WorkerOptions options;
    volatile LONG options_version;
};

void worker_start(App* app);
// Hands the transcription threads the settings in App.config now and wakes them. The setters
// below call it; so does anything else that changes what they use (the AI folder).
void worker_options_publish(App* app);
void worker_set_mode(App* app, Mode mode);
void worker_set_gpu_limit(App* app, u32 percent);
// Whether auto mode waits while a game runs (saved to the config).
void worker_set_pause_for_games(App* app, bool on);
// Your own transcription server instead of lifelog's (empty `url`: lifelog's own). Saved to
// config.toml. The main thread.
void worker_set_endpoint(App* app, Str url, Str model);
// Tells the supervisor to look again: a new WAV, a game started or ended, the GPU or the summary
// changed, a setting changed, the transcriber was set up. Any thread.
void worker_wake(App* app);
// Waits (up to 15 s) for transcription to stop after app->stop is set, when the program ends. A
// file being transcribed stays in the queue for next time. The main thread.
void worker_stop(App* app);
