![lifelog](assets/banner.svg)

An always-on, local, searchable transcript of what is said and heard on this PC: your mic and every app that plays
sound, each as its own stream, transcribed on the GPU. Each evening a local AI writes a summary of the day.
Nothing leaves the PC. Design and roadmap: [PLAN.md](PLAN.md).

## Parts

- `app/`: `lifelog.exe`, in C++ with [Dear ImGui](https://github.com/ocornut/imgui): the recorder (WASAPI mic
  and per-app loopback), the window, the tray icon, the local API and the CLI. Recording runs on background
  threads and the window on the main thread.
- Transcription: [whisper.cpp](https://github.com/ggml-org/whisper.cpp) with Whisper (large-v3-turbo by default), loaded into
  the app, which writes the lines into SQLite (`schema.sql`). It steps aside while a game is running and catches up
  after.
- Daily summary: a [llama.cpp](https://github.com/ggml-org/llama.cpp) server with Qwen3 14B by default, only running
  while it writes. Or an AI you already run (Ollama, LM Studio, any OpenAI-compatible endpoint).

## Setup

Install `lifelog-<version>.msi`. It installs for your account only (no admin prompt) into
`%LOCALAPPDATA%\Programs\lifelog`, adds a Start menu entry, and starts lifelog in the tray now and at login.

The installer holds no AI. In **Settings**, pick a **Model** under Transcription and under Daily summary, and it
downloads from where it is published: whisper.cpp's CUDA build with a Whisper model (large-v3-turbo, 1.6 GB, by
default; smaller and larger ones are listed), and [llama.cpp](https://github.com/ggml-org/llama.cpp) with a model
for your graphics card (Qwen3 14B, 9 GB, by default; Qwen3 4B and 8B, Gemma 4 12B and Qwen3.8 27B are listed too).
They go into `%LOCALAPPDATA%\lifelog` unless you pick another folder under **AI models**, which moves them. A
`.gguf` you put in its `llm` folder (or a `ggml-*.bin` in `whisper`) shows in the list too. Or pick **Your own
server** instead:

- **Transcriber:** any OpenAI-compatible transcription server (`POST <endpoint>/audio/transcriptions`, such as
  faster-whisper-server or a cloud API) and the model name it knows.
- **Summary:** any OpenAI-compatible server (Ollama, LM Studio, a cloud API) and the model name it knows.

Recordings and the database go to `%LOCALAPPDATA%\lifelog\data` unless `data_dir` says otherwise. Settings live in
`%APPDATA%\lifelog\config.toml`, and the log in `%LOCALAPPDATA%\lifelog\lifelog.log`. Uninstalling keeps your data,
settings and the downloaded AI. To take the AI off, delete a model (its trash button) or use **Remove all** under **AI models**
in Settings before uninstalling; delete the two folders above to remove the rest.

To build it yourself: `app\build.bat` builds `app\build\lifelog.exe` (Visual Studio 2022), and
`scripts\build-msi.ps1` builds the release exe and `dist\lifelog-<version>.msi` (needs WiX Toolset 3).

## Use

- **Tray:** blue means listening. Left-click for the menu (pause, copy the last few minutes as a file, text or audio,
  paste files or links into the timeline). Right-click to pause or quit.
- **Timeline:** one page per day, searchable. Lines from the last 7 days can be played back.
- **Journal:** the daily summaries, a calendar, and reminders for things you said you'd do.
- **Settings:** which apps to record, which are games, when to transcribe, the summary time, and the chimes.

From a terminal or a Stream Deck:

```powershell
lifelog clip 15m             # or 1h, 2d, today
lifelog toggle               # private mode; also pause, resume
lifelog exclude spotify.exe  # stop recording an app; "include" undoes it
lifelog summary now          # or later, skip
lifelog status
```

The local API is on `127.0.0.1:47800` (`/status`, `/pause`, `/resume`, `/toggle`, `/clip?range=15m`).

## Privacy

- Get consent from the people you record. Excluded apps and private mode record nothing, and private mode drops
  the sentence in progress.
- Audio is kept for 7 days for playback and then deleted; the text stays.

## Contributing

GitHub is a mirror that is overwritten on every sync. To send a change, fork it and open a pull request; it gets
merged on the main copy. Don't merge pull requests on GitHub.
