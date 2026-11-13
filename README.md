# lifelog

Always-on, local, searchable transcript of what is said and heard on this PC. Design and roadmap: [PLAN.md](PLAN.md).

**Status: phases 1 (core) and 4 (daily summary) work.** Mic and each audio-playing app are captured as separate
streams, transcribed locally on the GPU, stored in SQLite, can be clipped to the clipboard, and each evening a local
AI sums up the day. Speakers and the phone app are later phases.

## How it fits together

- `src/` — native. `lifelogw.exe` is the recorder: a tray icon, no window. It captures the Windows default mic
  (following it when it changes) and one stream per app (WASAPI process loopback), cuts the audio into short 16 kHz
  WAV files in `<data_dir>\spool`, serves a local API and the app's pages, and runs the worker. `lifelog.exe` is the
  command line for the same thing.
- `ui/` — `lifelog-ui.exe`, the window (WebView2): one window with tabs (Browser, Your days, Settings), and small
  popups by the tray. It is a separate program that only runs while a window is open, so closing or ending it never
  stops recording. Icons are [Lucide](https://lucide.dev), vendored in `src/icons.js`.
- `worker/` — Python worker. Takes spool files oldest-first, transcribes with faster-whisper (large-v3-turbo),
  writes lines to `<data_dir>\lifelog.db`, then keeps the audio for a week (for playback).
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
(data folder, dismissed apps, model, when to transcribe). If the data drive is not plugged in at start, the
app waits for it. Log: `%LOCALAPPDATA%\lifelog\lifelog.log`.

## Use

Tray icon (a speech bubble with sound bars): red = recording, grey = private mode.

- **Left click** (or "Open browser") opens lifelog's window on the Browser tab: the whole transcript as one timeline, starting at now and
  loading more as you scroll up (new lines appear live at the bottom). Find a word or phrase to jump there. Pasted
  files sit in the timeline at the moment they were pasted: images show as images, videos and audio play in place,
  text shows as text, with "Open" (default app) and "Show in folder" (your default file manager) buttons.
  To copy, pick a range: click a line, Shift+click another (or type exact from/to times); "Clear" (or Esc) drops it.
  Toggle speakers off, then "Copy as file" (a .txt lands on the clipboard, ready to paste), "Copy text" or
  "Copy audio" (a .wav of the range, last 7 days). Lines from the last 7 days have a play button (or click a line
  and press Space).
- **Right click** gives a short menu:
  - **Open browser** → the browser above.
  - **Paste here** → saves a copy of whatever is on the clipboard (copied files, an image, or text) at this
    moment in the timeline, under `files/` in the data folder.
  - **Copy last…** → 10 minutes, 30 minutes, 1 hour, or "Input time…": a small box by the tray to type a number of
    minutes or hours, previewing the conversation where the copy would start as you type.
  - **Add game** → the apps that are open now; tick one to mark it as a game. While a game is running lifelog
    only records, and transcribes after it closes.
  - **Dismiss app** → the same list; tick an app to stop recording it (what it already has stays).
  - **Settings** → the window's Settings tab: recording on/off (private mode), apps (the ones playing now; "Edit
    list" pages through every app heard, 10 at a time; dismiss one to stop recording it and hide it, "Show
    dismissed" brings it back; the lock keeps an app from being dismissed), games (dismiss an open app as "not a
    game" so it stops being suggested here and in the tray; "Edit list" brings dismissed ones back), when to transcribe
    (automatic / always / paused, and the GPU load that also counts as "a game is running" for games not on the
    list), the daily summary, and buttons to open the data folder, log and config file. Changes apply at once and
    are saved.
  - **Quit**

**Daily summary.** At 21:00 (set in Settings) a notification says the summary starts in 5 minutes, since the local
AI needs the graphics card for a minute or two: **Start now**, **In an hour**, **Delay until…** (a small window to
pick a time) or **Skip today**. Ignored, it starts by itself after 5 minutes. It never asks during a game: it waits
until no game has run for 10 minutes, and a game starting during the 5 minutes puts it off the same way. While it writes, transcription steps aside so the two models
never share the GPU, and the model is unloaded straight after. The summary is about what you said; other people and
apps are only used as context (app audio is only given to the model when it is within a minute of you speaking).
The result is a few bullets per part of the day, "To remember" (every moment you said "need to remember", always
included, falling back to your own words if the model misses one), and "things you said you'd do" (only lines where
you actually said "I'll…", "remind me…", "I need to…" and so on). Read
it from the notification, or the "Your days" tab; it is also saved as `<data_dir>\summaries\YYYY-MM-DD.md`.
The day runs 04:00 to 04:00, so a late night counts as the day before. The model also gets the last month's
summaries (the last week in full, older days as one line) to recognise recurring people and follow-ups. Hover a
point in "Your days" and press its x to remove it and never mention that topic again (the list is in Settings).

The same from a terminal or a Stream Deck "System → Open" button, using `%LOCALAPPDATA%\Programs\lifelog\lifelog.exe`:

```powershell
lifelog clip 15m                 # or 1h, 2d, today
lifelog toggle                   # private mode; also pause, resume
lifelog exclude spotify.exe      # dismiss an app (stop recording it); "include" takes it back
lifelog game eldenring.exe       # only record while it runs; "not-game" undoes it
lifelog transcription paused     # or auto, always
lifelog summary now              # or later, skip; optionally a day: lifelog summary now 2026-11-13
lifelog status
```

Local API on `127.0.0.1:47800`: `GET /status`, `POST /pause`, `/resume`, `/toggle`, `/clip?range=15m`,
`/transcription?mode=auto`, `/summary?do=now|later|skip`, `/quit`. Notification buttons open `lifelog:` links,
registered (per user) to `lifelogw.exe`, which passes them to the running app.

## Privacy rules built in

- Every app that makes a sound is recorded by default; dismissed apps (`excluded_apps`) are not. Everyone recorded knows and
  consents.
- Private mode drops audio immediately, including the sentence in progress.
- Audio with speech is kept for 7 days (`keep_audio_days`) in `<data_dir>\audio\YYYY-MM-DD\` so lines can be
  played back, then deleted; the text stays. Clips without speech are deleted at once. Nothing leaves the PC; the
  summary model runs locally too.
