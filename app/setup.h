// setup.h: fetches the AI that lifelog doesn't ship, when asked to from Settings, and takes it off
// the computer again. Nothing AI is in the installer: each part is downloaded from where it is
// published, or you point lifelog at your own (an OpenAI-compatible endpoint, for either).
//
//   transcriber: whisper.cpp (CUDA build, whisper.dll and its libraries), a Whisper model
//                (ggml-<model>.bin) and the Silero voice detection model, into <ai dir>\whisper,
//                where worker.cpp loads them from.
//   summary AI:  llama.cpp's server (CUDA build) and a model (<model>.gguf), into <ai dir>\llm,
//                where llm.cpp looks.
//
// <ai dir> is config.ai.dir, %LOCALAPPDATA%\lifelog unless moved from Settings. The uninstaller
// leaves it, so Settings has Remove for each model and for all of it.
//
// Each download, and a move, runs on its own thread. Its progress is in App.setup; what the tools
// print goes to %LOCALAPPDATA%\lifelog\setup.log.

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
    char model[128];  // the model being downloaded
};

// A model file found in the AI folder.
struct AiFile {
    char name[128];  // as config's model is: "large-v3-turbo", "qwen3-14b"
    f64 bytes;
};

struct Setup {
    SetupState part[SETUP_PARTS];
    SetupState move;  // moving the AI folder

    // What is in the AI folder, looked at again by ai_scan (Settings, every few seconds and after
    // a download, a removal or a move). The main thread's.
    AiFile files[SETUP_PARTS][16];
    i32 file_count[SETUP_PARTS];
    f64 engine_bytes[SETUP_PARTS];  // whisper.cpp's or llama.cpp's own files
    f64 total_bytes;                // all of the AI folder's whisper and llm
    f64 scanned_ts;
};

// A model lifelog knows where to download.
struct AiModel {
    const char* name;   // the config's model
    const char* label;  // "Whisper large-v3-turbo"
    const char* note;   // "Fast and accurate. The default."
    const wchar_t* url;
    f64 gb;             // the download
};
// The models offered for `part`, the default first.
i32 ai_models(SetupPart part, const AiModel** out);
// Null if lifelog does not know `name` (a file you put there, or your server's).
const AiModel* ai_model(SetupPart part, const char* name);

// Starts downloading `part` (its engine if missing, and `model`) unless a download or a move is
// running; when done, `model` is used, and your own server for it is forgotten. The main thread.
void setup_start(App* app, SetupPart part, const char* model);
// Deletes one downloaded model. The main thread. False, saying why, if it could not.
bool setup_remove(App* app, SetupPart part, const char* model);
// Deletes every model and both engines: what an uninstall leaves behind. What is in use right now
// (the transcriber's libraries, once loaded) goes the next time lifelog starts. The main thread.
void setup_remove_all(App* app);
// Moves the AI files to `dir` (its whisper and llm folders) on a thread of its own, then uses
// them there. The main thread.
void setup_move(App* app, Str dir);
// At start, before anything loads: deletes what setup_remove_all or setup_move could not.
void setup_cleanup(Config* cfg);
// Looks at what the AI folder has now (App.setup.files). The main thread.
void ai_scan(App* app);

// <ai dir>\whisper: the ggml-*.bin models, and one folder per whisper.cpp build.
void whisper_dir(const Config* cfg, wchar_t out[MAX_PATH]);
// That build's folder (whisper\b5454): whisper.dll, ggml and the CUDA libraries.
void whisper_bin_dir(const Config* cfg, wchar_t out[MAX_PATH]);
// <ai dir>\llm: llama-server.exe, its libraries and the .gguf models.
void llm_dir(const Config* cfg, wchar_t out[MAX_PATH]);
// Where `part`'s `model` (a name as in config.toml) is kept: whisper\ggml-<model>.bin or
// llm\<model>.gguf.
void model_path(const Config* cfg, SetupPart part, const char* model, wchar_t out[MAX_PATH]);
// llm\llama-server.exe.
void llm_exe_path(const Config* cfg, wchar_t out[MAX_PATH]);
// lifelog's own transcriber is there: whisper.dll, ggml-<model>.bin and the VAD model.
bool whisper_ready(const Config* cfg, const char* model);
// lifelog's own summary AI is there: llama-server.exe and <model>.gguf in the llm folder.
bool summary_ai_ready(const Config* cfg, const char* model);
// The worker loaded whisper.cpp from `dir`: it stays in use until lifelog closes. Any thread.
void whisper_loaded_from(const wchar_t* dir);

// Your own instead of lifelog's: saved to config.toml. The main thread. (worker_set_endpoint is in
// worker.h.)
void summary_set_endpoint(App* app, Str url, Str model);
