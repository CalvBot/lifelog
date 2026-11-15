// setup.h: fetches the AI that lifelog doesn't ship, when asked to from Settings. Nothing AI is in
// the installer: each part is downloaded from where it is published, or you point lifelog at your
// own (a Python with the worker installed, or an OpenAI-compatible endpoint for the summary).
//
//   transcriber: uv (astral-sh/uv) makes a Python environment in %LOCALAPPDATA%\lifelog\transcriber
//                with the worker's dependencies: faster-whisper and the CUDA libraries, about 3 GB.
//                The worker's own code stays next to lifelog.exe and runs from there.
//                faster-whisper fetches the Whisper model itself the first time it runs.
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
// The Python of the environment the setup makes (it may not exist yet).
void transcriber_python(wchar_t out[MAX_PATH]);
// The worker's source folder: <exe dir>\worker when installed, <repo>\worker when run from the
// build. The setup's Python runs `-m lifelog_worker` from there.
bool worker_source(wchar_t out[MAX_PATH]);
// lifelog's own summary AI is there: llama-server.exe and <model>.gguf in the llm folder.
bool summary_ai_ready(const char* model);

// Your own instead of lifelog's: saved to config.toml. With the lock.
void worker_set_python(App* app, Str python);
void summary_set_endpoint(App* app, Str url, Str model);
