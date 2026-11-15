# lifelog

Always-on, local, searchable transcript of what is said and heard on this PC. Design and roadmap: [PLAN.md](PLAN.md).

**Status: phases 1 (core) and 4 (daily summary) work.** Mic and each audio-playing app are captured as separate
streams, transcribed locally on the GPU, stored in SQLite, can be clipped to the clipboard, and each evening a local
AI sums up the day. Speakers and the phone app are later phases.

## How it fits together

- `src/` — native, the library behind `lifelogw.exe`, the app: a tray icon and the recorder. It captures the Windows default mic
  (following it when it changes) and one stream per app (WASAPI process loopback), cuts the audio into short 16 kHz
  WAV files in `<data_dir>\spool`, serves a local API, and runs the worker. Every few hours it also scans installed
  software (Steam, Epic, GOG, EA, Ubisoft, Battle.net, Riot, Xbox, Start menu) to fill the Games and Apps lists. `lifelog.exe` is the
  command line for the same thing.
- `ui/` — builds `lifelogw.exe`: the recorder runs on its own threads and the window on the main thread, native
  native ([immediate-mode](https://github.com/emilk/immediate-mode)): one window with tabs (Timeline, Journal, Settings), and small popups
  by the tray. The window only exists while it is open, and calls the API's routes in-process; the local port is
  for the CLI, the Stream Deck and notification buttons. Icons are [Lucide](https://lucide.dev) as a font,
  vendored in `ui/assets/`; apps show their own exe icons.
- `worker/` — Python worker. Takes spool files oldest-first, transcribes with faster-whisper (large-v3-turbo),
  writes lines to `<data_dir>\lifelog.db`, then keeps the audio for a week (for playback).
- `schema.sql` — database schema, used by both.
- Daily summary — lifelog's own [llama.cpp](https://github.com/ggml-org/llama.cpp) server with Qwen3 14B, in
  `%LOCALAPPDATA%\lifelog\llm` (`src/llm`, `src/summary`). It runs only while a summary is written and is
  killed straight after, so nothing else has to be installed or kept running.

Capturing costs almost nothing and never stops. Transcribing uses the GPU (about 2.3 GB of VRAM), so by default
("auto") the worker is shut down while an app you marked as a game is running, or while anything is loading the
GPU's 3D engine, and starts again afterwards; the audio waits in the queue and is caught up much faster than real time.

## Setup

```powershell
python -m venv worker\.venv
worker\.venv\Scripts\python.exe -m pip install -e worker
scripts\install.ps1        # builds, installs to %LOCALAPPDATA%\Programs\lifelog, starts it, starts at every login
                           # and runs scripts\get-llm.ps1: the summary AI (~650 MB + a 9 GB model; reuses Ollama's
                           # qwen3:14b if it is there). Check it with: lifelog check-ai
```

Run `scripts\install.ps1` again after changing the code to update; `-Uninstall` removes the app (not the data).
The first run downloads the Whisper model (~1.6 GB) and writes a default config to `%APPDATA%\lifelog\config.toml`
(data folder, dismissed apps, model, when to transcribe). If the data drive is not plugged in at start, the
app waits for it. Log: `%LOCALAPPDATA%\lifelog\lifelog.log`.

## Use

Tray icon (a speech bubble with sound bars): blue = listening, grey with a pause badge = not listening.

- **Left click** opens lifelog's own small menu by the tray (click again to close it):
  - **Listening** on/off.
  - **Copy the last** few minutes or hours (type a number, pick min or h), previewing where the copy starts;
    then **File** (a .txt on the clipboard, ready to paste), **Text** or **Audio** (a .wav).
  - **Timeline**, **Paste here** (saves whatever is on the clipboard — files, an image, or text — at this moment
    in the timeline, under `files/` in the data folder; copied text that is a file or folder path or a web address
    becomes a link instead, with no copy kept), **Settings**, **Restart**, **Quit**.
- **Right click**: Pause/Resume, Restart, Quit.
- **Timeline** tab: one page per day (a day runs 04:00 to 04:00), oldest at the top. Today opens at the bottom and new
  lines write themselves in as they are transcribed. ◀ ▶ (or the arrow keys) go a day back or forward; the day's name
  opens a calendar to jump weeks or months back. Search works like Ctrl+F: matches are marked, Enter goes to the most
  recent one, then older ones (Shift+Enter goes back), switching days as needed. Pasted files sit at the moment they
  were pasted: images show inline, other files have "Open" and "Show in folder" buttons.
  **Show** lists the apps heard on the page; click one to hide its lines. Excluded apps (no longer listened
  to) are hidden unless "Excluded apps" is ticked; "All apps…" opens a window listing every app ever heard.
  Right-click a line to copy it (text or its audio). To copy several, click a line and Shift+click another (Esc
  drops the pick); right-clicking inside the pick copies all of it as text, audio or a file (also Ctrl+Enter),
  leaving out hidden apps. Lines from the last 7 days have a play button (or pick a line and press Space):
  click = everything from there on, mixed; Shift = just that moment, every app; Ctrl = only that app, from there on;
  Ctrl+Shift = just that line. Hovering the button marks the lines it would play (hold Shift/Ctrl to see the
  difference), and while playing, the words being heard light up.
- **Settings**: listening on/off, apps (every app heard plus installed ones, 10 at a time; exclude one to stop
  recording it, "Excluded" brings it back; the lock keeps an app from being excluded), games (installed games are
  found and marked automatically; switch one off and it stays a not-game; running programs can be marked by hand),
  when to transcribe (automatic / always / paused, and the GPU load that also counts as "a game is running"), the
  daily summary, and buttons to open the data folder, log and config file. Changes apply at once and are saved.

**Daily summary.** At 21:00 (set in Settings) a notification says the summary starts in 5 minutes, since the local
AI needs the graphics card for a minute or two: **Start now**, **In an hour**, **Delay until…** (a small window to
pick a time) or **Skip today**. Ignored, it starts by itself after 5 minutes. It never asks during a game: it waits
until no game has run for 10 minutes, and a game starting during the 5 minutes puts it off the same way. While it writes, transcription steps aside so the two models
never share the GPU, and the model is unloaded straight after. The summary is about what you said; other people and
apps are only used as context (app audio is only given to the model when it is within a minute of you speaking).
The result is a few bullets per part of the day, "To remember" (every moment you said "need to remember", always
included, falling back to your own words if the model misses one), and "things you said you'd do" (only lines where
you actually said "I'll…", "remind me…", "I need to…" and so on). Read
it from the notification, or the Journal tab; it is also saved as `<data_dir>\summaries\YYYY-MM-DD.md`.
The day runs 04:00 to 04:00, so a late night counts as the day before. The model also gets the last month's
summaries (the last week in full, older days as one line) to recognise recurring people and follow-ups. Hover a
point in the Journal and press its x to remove it and never mention that topic again (the list is in Settings).

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

## Contributing

The GitHub repo is a mirror: it is overwritten from the main copy on every sync, so nothing pushed to it directly
survives. To send a change, fork it on GitHub, push a branch to your fork and open a pull request. It gets merged on
the main copy, and the mirror then shows the pull request as merged. Don't merge pull requests on GitHub.
