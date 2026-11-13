# lifelog

Always-on, local, searchable transcript of what is said and heard on this PC. Design and roadmap: [PLAN.md](PLAN.md).

**Status: phases 1 (core) and 4 (daily summary) work.** Mic and each audio-playing app are captured as separate
streams, transcribed locally on the GPU, stored in SQLite, can be clipped to the clipboard, and each evening a local
AI sums up the day. Speakers and the phone app are later phases.

## How it fits together

- `src/` — native. `lifelogw.exe` is the app: a tray icon, no window. It captures the mic and one stream per app
  (WASAPI process loopback), cuts the audio into short 16 kHz WAV files in `<data_dir>\spool`, serves a local API,
  and runs the worker. `lifelog.exe` is the command line for the same thing.
- `worker/` — Python worker. Takes spool files oldest-first, transcribes with faster-whisper (large-v3-turbo),
  writes lines to `<data_dir>\lifelog.db`, then deletes the audio.
- `schema.sql` — database schema, used by both.
- Daily summary — [Ollama](https://ollama.com) with `qwen3:14b`, called by the app (`src/summary`).

Capturing costs almost nothing and never stops. Transcribing uses the GPU (about 2.3 GB of VRAM), so by default
("auto") the worker is shut down while an app you marked as a game is running, or while anything is loading the
GPU's 3D engine, and starts again afterwards; the audio waits in the queue and is caught up much faster than real time.

## Setup

```powershell
python -m venv worker\.venv
worker\.venv\Scripts\python.exe -m pip install -e worker
winget install Ollama.Ollama; ollama pull qwen3:14b     # for the daily summary (9.3 GB)
scripts\install.ps1        # builds, installs to %LOCALAPPDATA%\Programs\lifelog, starts it, starts at every login
```

Run `scripts\install.ps1` again after changing the code to update; `-Uninstall` removes the app (not the data).
The first run downloads the Whisper model (~1.6 GB) and writes a default config to `%APPDATA%\lifelog\config.toml`
(data folder, excluded apps, hotkey, model, when to transcribe). If the data drive is not plugged in at start, the
app waits for it. Log: `%LOCALAPPDATA%\lifelog\lifelog.log`.

## Use

Tray icon (a speech bubble with sound bars): red = recording, grey = private mode.

- **Left click** opens the clip window: pick any range (presets, or exact from/to times), or type a word or phrase
  you remember from the start of a conversation and pick the hit to start the clip there. The transcript is
  previewed live; click a line to start there, Shift+click to end there, toggle speakers off, then "Copy as file"
  (a .txt lands on the clipboard, ready to paste) or "Copy text".
- **Right click** gives a short menu:
  - **Copy last…** → 10 minutes, 30 minutes, 1 hour, "Input time (minutes)" (a small box to type any number),
    and "Advanced" (the clip window above).
  - **Add game** → the apps that are open now; tick one to mark it as a game. While a game is running lifelog
    only records, and transcribes after it closes.
  - **Add exclusion** → the same list; tick an app to never record it.
  - **Settings** → the settings window: private mode, apps to record, games, when to transcribe (automatic /
    always / paused, and the GPU load that also counts as "a game is running" for games not on the list), and
    buttons to open the data folder, log and config file. Changes apply at once and are saved.
  - **Quit**

**Daily summary.** At 21:00 (set in Settings) a notification asks whether the local AI may sum up the day, since it
needs the graphics card for a minute or two: **Write it now**, **In an hour** or **Skip today**. Ignored, it asks again
an hour later; while a game is running it waits to ask. While it writes, transcription steps aside so the two models
never share the GPU, and the model is unloaded straight after. The result is a few bullets per part of the day plus
"things you said you'd do" (only lines where you actually said "I'll…", "remind me…", "I need to…" and so on). Read
it from the notification, or Settings → "Read your days"; it is also saved as `<data_dir>\summaries\YYYY-MM-DD.md`.
The day runs 04:00 to 04:00, so a late night counts as the day before.

Private mode also has a hotkey, Ctrl+Alt+P by default.

The same from a terminal or a Stream Deck "System → Open" button, using `%LOCALAPPDATA%\Programs\lifelog\lifelog.exe`:

```powershell
lifelog clip 15m                 # or 1h, 2d, today
lifelog toggle                   # private mode; also pause, resume
lifelog exclude spotify.exe      # never capture an app; "include" allows it again
lifelog game eldenring.exe       # only record while it runs; "not-game" undoes it
lifelog transcription paused     # or auto, always
lifelog summary now              # or later, skip; optionally a day: lifelog summary now 2026-11-13
lifelog status
```

Local API on `127.0.0.1:47800`: `GET /status`, `POST /pause`, `/resume`, `/toggle`, `/clip?range=15m`,
`/transcription?mode=auto`, `/summary?do=now|later|skip`, `/quit`. Notification buttons open `lifelog:` links,
registered (per user) to `lifelogw.exe`, which passes them to the running app.

## Privacy rules built in

- Apps in `excluded_apps` are never captured. **Discord is excluded by default** and should stay that way until the
  consent-based bot from phase 2 exists (see "Legal" in the plan).
- Private mode drops audio immediately, including the sentence in progress.
- Audio is deleted once transcribed; nothing leaves the PC. The summary model runs locally too.
