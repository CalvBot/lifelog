// pool.h: a few threads for the window's small slow tasks: copying a stretch to the clipboard,
// pasting, opening files and folders, file previews, the folder picker, app icons. The main
// thread fills in a Job, posts it with the function to run, and gets it back as an event: `run`
// works on a pool thread with only what is in the job, then job->apply runs on the main thread.
//
// The threads have COM set up (single-threaded apartment), as the shell and WIC want.
//
// Tasks that last as long as something else does keep their own thread: playback, the setup
// downloads, the summary writer.

#pragma once

typedef void PoolRun(Job* job);

void pool_start(App* app);
// After App.stop is set. Tasks not started yet are dropped.
void pool_stop();
// The main thread.
void pool_post(Job* job, PoolRun* run);
