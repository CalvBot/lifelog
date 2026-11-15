// worker.h: transcription. Runs the Python worker (worker/lifelog_worker) only when it may use the
// GPU: in auto mode it is shut down while a game is running or the 3D load is over the limit, and
// started again once the GPU has been quiet for `resume_after_secs`. While the summary writes,
// it steps aside too, so the two models never share the GPU.
//
// The mode, limit and wait are in App.config.processing; the functions below change them there
// and in config.toml.

#pragma once

struct Worker {
    char state[160];      // what it is doing, for the window: "transcribing", "waiting for the game to close", ...
    bool running;         // the Python process is up
    i32 queued;           // WAV files waiting in the spool folder
    f64 queued_secs;      // how much audio that is
    u32 gpu_3d_percent;   // the 3D engine's load now, as in Task Manager
    bool gpu_busy;        // over the limit
    bool summary_needs_gpu;  // set by the summary while it writes
    u64 lines_version;    // bumped each time the worker writes lines, so the timeline reloads
    f64 waiting_since;    // when transcription last had to stop and wait; 0 while it may run
    // The worker thread's own. Whoever takes them out of here (under the lock) stops the process.
    HANDLE process;
    HANDLE stdin_write;   // closing it asks the worker to finish its file and exit
    HANDLE job;           // holds the process tree (the venv's python.exe starts the real one)
    HANDLE leaving;       // a process the supervisor is stopping now, for worker_stop to wait on
};

void worker_start(App* app);
void worker_set_mode(App* app, Mode mode);
void worker_set_gpu_limit(App* app, u32 percent);
// Ends the Python process (when the program ends). Waits up to 20 s for it to finish its file;
// the caller holds the lock like any app_* call, so call it once the window is gone.
void worker_stop(App* app);
