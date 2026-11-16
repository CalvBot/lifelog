// setup.h: fetches the AI that lifelog doesn't ship, when asked to from Settings. Nothing AI is in
// the installer: each part is downloaded from where it is published, or you point lifelog at your
// own (an OpenAI-compatible endpoint, for either).
//
//   transcriber: whisper.cpp's server (CUDA build), the Whisper large-v3-turbo model (1.6 GB) and
//                the Silero voice detection model, about 2.3 GB, into %LOCALAPPDATA%\lifelog\whisper,
//                where worker.cpp looks.
//   summary AI:  llama.cpp's server (CUDA build) and the Qwen3 14B model (9 GB), into
//                %LOCALAPPDATA%\lifelog\llm, where llm.cpp looks.
//
// Each part runs on its own thread. Its progress is in App.setup; what the tools print goes to
// %LOCALAPPDATA%\lifelog\setup.log.

#pragma once

enum SetupPart : u8 {
    SETUP_TRANSCRIBER,
    SETUP_SUMMARY_AI,
    SETUP_PARTS,
};

struct SetupState {
    bool running;
    bool failed;      // the last try failed; `text` says why
    char text[256];   // "Downloading the model: 2.1 of 9.0 GB", or the error
};

struct Setup {
    SetupState part[SETUP_PARTS];
};

// Starts setting up `part` unless it is already running. With the lock.
void setup_start(App* app, SetupPart part);
// %LOCALAPPDATA%\lifelog\whisper: Release\whisper-server.exe and the ggml-*.bin models.
void whisper_dir(wchar_t out[MAX_PATH]);
// lifelog's own transcriber is there: whisper-server.exe, ggml-<model>.bin and the VAD model.
bool whisper_ready(const char* model);
// lifelog's own summary AI is there: llama-server.exe and <model>.gguf in the llm folder.
bool summary_ai_ready(const char* model);

// Your own instead of lifelog's: saved to config.toml. With the lock. (worker_set_endpoint is in
// worker.h.)
void summary_set_endpoint(App* app, Str url, Str model);
