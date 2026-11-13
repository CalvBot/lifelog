# lifelog

Always-on, local, searchable transcript of what is said and heard on this PC. Design and roadmap: [PLAN.md](PLAN.md).

**Status: phase 1 (core) works.** Mic and each audio-playing app are captured as separate streams, transcribed
locally on the GPU, stored in SQLite, and can be clipped to the clipboard. Speakers, UI, daily summary and the
phone app are later phases.

## How it fits together

- `src/` — native. `lifelogw.exe` is the app: a tray icon, no window. It captures the mic and one stream per app
  (WASAPI process loopback), cuts the audio into short 16 kHz WAV files in `<data_dir>\spool`, serves a local API,
  and runs the worker. `lifelog.exe` is the command line for the same thing.
- `worker/` — Python worker. Takes spool files oldest-first, transcribes with faster-whisper (large-v3-turbo),
  writes lines to `<data_dir>\lifelog.db`, then deletes the audio.
- `schema.sql` — database schema, used by both.

Capturing costs almost nothing and never stops. Transcribing uses the GPU (about 2.3 GB of VRAM), so by default
("auto") the worker is shut down while a game is loading the GPU and starts again a minute after the GPU goes
quiet; the audio waits in the queue and is caught up much faster than real time.

## Setup

```powershell
python -m venv worker\.venv
worker\.venv\Scripts\python.exe -m pip install -e worker
scripts\install.ps1        # builds, installs to %LOCALAPPDATA%\Programs\lifelog, starts it, starts at every login
```

Run `scripts\install.ps1` again after changing the code to update; `-Uninstall` removes the app (not the data).
The first run downloads the Whisper model (~1.6 GB) and writes a default config to `%APPDATA%\lifelog\config.toml`
(data folder, excluded apps, hotkey, model, when to transcribe). If the data drive is not plugged in at start, the
app waits for it. Log: `%LOCALAPPDATA%\lifelog\lifelog.log`.

## Use

Tray icon: red dot = recording, grey dot = private mode. Click it for private mode, "copy last 15 minutes / hour /
today" (a .txt file lands on the clipboard, ready to paste), transcription mode (auto / always / paused), log and quit.
Private mode also has a hotkey, Ctrl+Alt+P by default.

The same from a terminal or a Stream Deck "System → Open" button, using `%LOCALAPPDATA%\Programs\lifelog\lifelog.exe`:

```powershell
lifelog clip 15m                 # or 1h, 2d, today
lifelog toggle                   # private mode; also pause, resume
lifelog transcription paused     # or auto, always
lifelog status
```

Local API on `127.0.0.1:47800`: `GET /status`, `POST /pause`, `/resume`, `/toggle`, `/clip?range=15m`,
`/transcription?mode=auto`, `/quit`.

## Privacy rules built in

- Apps in `excluded_apps` are never captured. **Discord is excluded by default** and should stay that way until the
  consent-based bot from phase 2 exists (see "Legal" in the plan).
- Private mode drops audio immediately, including the sentence in progress.
- Audio is deleted once transcribed; nothing leaves the PC.
