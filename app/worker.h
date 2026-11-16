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
// and in config.toml.

#pragma once

struct Worker {
    char state[160];      // what it is doing, for the window: "transcribing", "waiting for the game to close", ...
    bool running;         // transcribing is possible now: our model is loaded, or an endpoint is set and allowed
    bool missing;         // nothing to transcribe with: no endpoint, and whisper.cpp is not set up
    i32 queued;           // WAV files waiting in the spool folder
    f64 queued_secs;      // how much audio that is
    u32 gpu_3d_percent;   // the 3D engine's load now, as in Task Manager
    bool gpu_busy;        // over the limit
    bool summary_needs_gpu;  // set by the summary while it writes
    u64 lines_version;    // bumped each time lines are written, so the timeline reloads
    f64 waiting_since;    // when transcription last had to stop and wait; 0 while it may run
    HANDLE done;          // set when the supervisor has stopped and freed the model
};

void worker_start(App* app);
void worker_set_mode(App* app, Mode mode);
void worker_set_gpu_limit(App* app, u32 percent);
// Your own transcription server instead of lifelog's (empty `url`: lifelog's own). Saved to
// config.toml. With the lock.
void worker_set_endpoint(App* app, Str url, Str model);
// Waits (up to 15 s) for transcription to stop after app->stop is set, when the program ends. A
// file being transcribed stays in the queue for next time. Without the lock.
void worker_stop(App* app);
