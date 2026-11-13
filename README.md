# lifelog

Always-on, local, searchable transcript of what is said and heard on this PC. Design and roadmap: [PLAN.md](PLAN.md).

**Status: phase 1 (core) works.** Mic and each audio-playing app are captured as separate streams, transcribed
locally on the GPU, stored in SQLite, and can be clipped to the clipboard. Speakers, UI, daily summary and the
phone app are later phases.

## How it fits together

- `src/` — native service (`lifelog.exe`). Captures the mic and one stream per app (WASAPI process loopback), cuts the
  audio into short 16 kHz WAV files in `<data_dir>\spool`, serves a local API, and runs the worker.
- `worker/` — Python worker. Takes spool files oldest-first, transcribes with faster-whisper (large-v3-turbo),
  writes lines to `<data_dir>\lifelog.db`, then deletes the audio. If the GPU is busy it just falls behind and catches up.
- `schema.sql` — database schema, used by both.

## Setup

```powershell
the build build --release
python -m venv worker\.venv
worker\.venv\Scripts\python.exe -m pip install -e worker
```

The first run downloads the Whisper model (~1.6 GB) and writes a default config to `%APPDATA%\lifelog\config.toml`
(data folder, excluded apps, hotkey, model).

## Use

```powershell
target\release\lifelog.exe run          # start capturing; Ctrl+C to stop
target\release\lifelog.exe clip 15m     # transcript of the last 15m / 1h / 2d / today → .txt file on the clipboard
target\release\lifelog.exe pause        # private mode (also: resume, toggle, or the hotkey, default Ctrl+Alt+P)
target\release\lifelog.exe status
```

Stream Deck, until there is a proper plugin: a "System → Open" button running `lifelog.exe clip 15m` (or `toggle`).

Local API on `127.0.0.1:47800`: `GET /status`, `POST /pause`, `/resume`, `/toggle`, `/clip?range=15m`.

## Privacy rules built in

- Apps in `excluded_apps` are never captured. **Discord is excluded by default** and should stay that way until the
  consent-based bot from phase 2 exists (see "Legal" in the plan).
- Private mode drops audio immediately, including the sentence in progress.
- Audio is deleted once transcribed; nothing leaves the PC.
