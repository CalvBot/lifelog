// voice.h: voice commands. Say the wake word ("lifelog" unless changed in Settings) and a command,
// as one sentence ("lifelog, summarise the last hour") or with a pause after the name (it chimes, then
// listens for six seconds). The commands: summarise the last stretch, remind me / set an
// alarm, open an app, pause and resume (private mode), pause and continue transcription, check
// the AI, and never mind. "Clip" is left for a custom command (commands.h), such as OBS.
//
// The mic's capture thread has a second segmenter that hears even in private mode and ends a
// sentence after a 0.7 s pause (capture.cpp); each sentence comes here (voice_heard), and the
// voice thread runs a small Whisper model (base.en unless changed in Settings) on the processor
// over it, so it works while the transcriber waits for a game. A sentence that does not start
// with the name is dropped at once and never logged. A command is carried out through the API's own handler (api_ask), so it does exactly
// what the CLI's does, and a chime says how it went. In private mode only "resume" is acted on.
//
// What is said also goes into the transcript as usual (the recording segmenter is apart).

#pragma once

enum VoiceState : i32 {
    VOICE_OFF,          // listening is off in Settings
    VOICE_NEEDS_SETUP,  // whisper.cpp or the listener's model (ggml-base.en.bin) is not there
    VOICE_LOADING,
    VOICE_READY,
    VOICE_FAILED,       // the model would not load (see the log)
};

struct VoiceSentence {
    f32* samples;  // malloc
    i32 n;
};

struct Voice {
    // The queue from the mic's thread to the voice thread.
    SRWLOCK lock;
    VoiceSentence queue[4];
    i32 head, count;
    HANDLE wake;              // auto-reset: a sentence is waiting, or the settings changed
    volatile LONG listen;     // config.voice.listen, for the capture thread
    volatile LONG state;      // VoiceState, written by the voice thread
    SRWLOCK wake_lock;        // guards wake_word and model
    char wake_word[64];       // config.voice.wake, for the voice thread
    char model[32];           // config.voice.model: "base.en"
    // The main thread's, for Settings: the last command and what came of it.
    char heard[160];
    char result[160];
    f64 heard_ts;
};

void voice_start(App* app);
// The mic's thread: a sentence it heard, `n` samples at 16 kHz, to be multiplied by `gain`.
void voice_heard(App* app, const f32* samples, i32 n, f32 gain);
// Main thread: turn listening on or off, and save it.
void voice_set_listen(App* app, bool on);
// Main thread: the word that starts a command ("lifelog" when empty), saved. False (with a
// message) if it is not one: under 3 letters, or anything but letters, digits and spaces.
bool voice_set_wake(App* app, Str word);
// Main thread: the model that listens (one of voice_models), saved; the voice thread loads it, and
// Settings offers to download it if it is not there. False if it is not one of them.
bool voice_set_model(App* app, Str model);

// banner.cpp. Main thread: a note at the top of the screen for 2.5 s, "WHO — TEXT" ("Jarvis — Alarm
// set: 4 minutes"); it takes no focus or clicks.
void banner_show(const char* who, const char* text);
