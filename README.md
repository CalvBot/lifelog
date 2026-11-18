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
  paste files or links into the timeline). Right-click to pause, restart or quit.
- **Timeline:** one page per day, searchable. Lines from the last 7 days can be played back.
- **Journal:** the daily summaries, a calendar, and reminders for things you said you'd do.
- **Settings:** which apps to record, which are games, when to transcribe, the summary time, and voice commands.
- **Voice commands:** say "lifelog" and a command, such as "lifelog, summarise the
  last 30 minutes", "lifelog, remind me in 10 minutes to check the oven", "lifelog, open Spotify", "lifelog, pause"
  (the music), "lifelog, skip", "lifelog, pause lifelog" (private mode) or "lifelog, pause transcription". You can also say "lifelog", wait for the chime, then give the command. When
  one works, a note shows at the bottom right of the screen for a moment ("lifelog — Alarm set: 4 minutes"). In
  private mode only "resume lifelog" and the media keys work. Settings lists them and can change the wake word from "lifelog" to your own;
  they are heard with the transcriber's model, and while it waits for a game a small backup model (0.15 GB) on the
  processor listens instead (Settings, Transcription). "Clip" is not built in: it
  is left for a custom command, such as one that saves an OBS replay.
- **Custom commands:** your own voice commands, each a folder with a small script, such as "lifelog, play Bad Guy"
  for a Spotify script. Each one runs only after you allow it in Settings. See
  [docs/custom-commands.md](docs/custom-commands.md) and the template in [commands/example](commands/example).

From a terminal or a Stream Deck:

```powershell
lifelog clip 15m             # or 1h, 2d, today
lifelog toggle               # private mode; also pause, resume
lifelog exclude spotify.exe  # stop recording an app; "include" undoes it
lifelog summary now          # or later, skip
lifelog summarise 30m        # a few sentences about the last 30 minutes, from the summary AI
lifelog launch spotify       # start an installed app or game
lifelog mics                 # the mics plugged in; pick one in Settings (default: the Windows one)
lifelog status
lifelog voice "lifelog, summarise the last hour"   # what a voice command would do, without doing it
lifelog commands             # the custom commands and what to say for each
lifelog command "play bad guy"                # run the custom command a sentence matches
lifelog mcp                  # an MCP server for AI agents (below)
```

### AI agents (MCP)

`lifelog mcp` is an MCP server that talks on standard input and output. An agent that supports MCP, such as
Claude Code or Codex, can use it to read the transcript. To add it to Claude Code:

```powershell
claude mcp add lifelog -- "C:\Program Files\lifelog\lifelog.exe" mcp
```

It has four tools, and all of them only read:

- `transcript`: the lines of the last stretch (`15m`, `1h`, `today`), of a day, or between two times.
- `search`: the lines that have all of some words, in any order, newest first, optionally between two times.
- `summary`: a day's summary.
- `status`: what the running lifelog is doing.

To limit what agents can read, set `reach` under `[mcp]` in `config.toml`, such as `reach = "15m"` for only the
last 15 minutes (or `1h`, `7d`, `today`). The server enforces it: older lines and the summaries of older days are
not shared, whatever the agent asks for. Speakers hidden in the timeline are never shared.

It reads the database directly, so it works while lifelog is closed, except for `status`. It opens no network
port; only the agent that started it can use it. An agent with a cloud model sends what it reads to that
model's provider. A custom voice command can pass a request on to an agent, which then reads the conversation
to work out what "that song" means; see [docs/custom-commands.md](docs/custom-commands.md#handing-a-request-to-an-ai-agent).

The local API is on `127.0.0.1:47800` (`/status`, `/pause`, `/resume`, `/toggle`, `/clip?range=15m`).

## Privacy

- Get consent from the people you record. Excluded apps and private mode record nothing, and private mode drops
  the sentence in progress.
- Voice commands listen to the mic even in private mode, so they can hear "resume". What they hear is never saved
  and never leaves the computer; turn them off in Settings to stop that.
- Audio is kept for 7 days for playback and then deleted; the text stays.

## Contributing

GitHub is a mirror that is overwritten on every sync. To send a change, fork it and open a pull request; it gets
merged on the main copy. Don't merge pull requests on GitHub.
