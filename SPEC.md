# lifelog: what it should do

This file is the requirements for a rewrite of the window and the state it reads. It lists what the app does today,
then how the parts should fit together. Edit it freely: cut what you don't want and add ideas under each heading.
Lines marked **(idea)** are not built yet.

Recording, transcription and the summary writer stay as they are. The rewrite covers the window, the tray menu and
how they get at the app's state.

## Architecture: one shared state

One program, `lifelogw.exe`, started at login. The recorder, the tray icon and the summary writer run on background
threads, the window on the main thread. Transcription is a whisper.cpp server the app starts while the GPU is free.

**One live state, shared in memory.** The app has a single `App` value that holds what the program is doing right
now: the settings, whether it is listening, which apps are being recorded, the transcription queue, the GPU load
and what the summary writer is doing. The recorder threads and the window share it. There are no copies of it.

- The window reads `App` every time it draws. Nothing is fetched, polled or turned into JSON.
- A button calls a method on `App`, for example `app.set_max_words(300)`. The method changes the value in memory,
  saves `config.toml` if it is a setting, and returns. The next frame draws the new value. There are no optimistic
  updates, because there is no reply to wait for.
- When a background thread changes something the window shows (a new line is heard, the summary finishes), it
  asks the window to redraw. Nothing checks on a timer to see whether anything changed.
- Undo keeps the old value and sets it back with the same method.

All of today's globals and request handling go into that one struct, plus functions that work on it directly.
Roughly:

```
App
  config       the settings, as loaded from config.toml
  apps         every app seen: exe, name, path, included/excluded, game or not, lines, last heard
  streams      what is being recorded now, with the handles of their capture threads
  worker       the transcription process, its mode, its queue, the GPU load
  storage      the database
  timeline     the day on screen and its lines and files
  journal      the summaries, and what the summary writer is doing
  deadlines    reminders and when each is due
  ui           which tab, the Show filter, the selection, undo
```

The program's own job is small: book-keeping around the recording (what came from which app, and when), and
writing to the database. Transcription and summaries are done by other programs it starts.

**Crashes say why.** Nothing stops the program without saying what it expected, what it got instead, and where.
Something that can go wrong in normal use (a missing drive, a busy port, an app that closed) is handled and shown,
not a crash. Today the code calls `unwrap()` in many places, and a failure there ends the program with a vague
message.

**Slow work stays off the window's thread.** Writing a summary, loading the AI, cutting an audio clip and long
searches run on a thread. The thread reports its progress into `App` (for example "writing, 2 of 5 hours"), and the
window draws whatever is there, so it keeps responding even while the AI loads.

**The database.** Lines, files, summaries and reminders live in SQLite. The window reads them directly. A day's
lines are read once and kept until that day changes. The worker tells the app when it writes new lines (one line on
its output per batch), and the app tells the window. **(to decide: the worker has no such signal today.)**

**Outside callers.** The CLI (`lifelog.exe`), a Stream Deck and notification buttons are other processes, so they
need a way in. A small local HTTP API stays for them only, and each route calls the same `App` methods. The window
doesn't use it.

## Language: C++ with Dear ImGui

C++ built with MSVC, in a plain data-oriented style:

- **Unity build.** One `build.bat` calls `cl.exe` on one `.cpp` file that includes the rest. No CMake, Ninja,
  vcpkg or project files. It should build in seconds.
- **No OOP.** Structs of plain data and free functions that work on them. No classes with behaviour, inheritance,
  virtual functions or getters and setters.
- **No RAII.** Nothing cleans up in destructors. Memory comes from arenas that are freed all at once (per frame,
  per day loaded, per program run), and handles are closed where the code says so.
- **ZII (zero is initialisation).** Every struct is valid when zeroed, so `App app = {};` is a working empty
  state, and a missing value reads as zero.
- **Data-oriented.** Arrays of records laid out for how they are read, for example a day's lines as one array,
  not a list of objects pointing at each other.
- **Asserts with messages** in place of `unwrap()`: what was expected, what came instead, and the file and line.
- Little of the C++ standard library; a few single-file libraries vendored into the repo:
  - Dear ImGui for the window, on Win32 and Direct3D 11;
  - SQLite (the amalgamation);
  - a small TOML reader for `config.toml`.
- Windows calls (WASAPI process loopback, the tray icon, notifications) made directly.
- whisper.cpp (whisper-server) and llama.cpp (llama-server) as child processes, downloaded from Settings.
- One `lifelog.exe`.

## Recording

- Records the mic and every app that plays sound, each as its own stream, with the app's exe, process and window
  title.
- Private mode (paused) records nothing and drops the sentence in progress.
- New apps are recorded until you decide: included (keep recording) or excluded (record nothing).
- Excluded apps can still count as games.
- Discord calls can be recorded one track per person (the Vencord plugin); then Discord's mixed sound is skipped.
- Audio is kept 7 days for playback, then deleted. The text is kept for good.
- If the data drive is not plugged in, it waits for it instead of recording somewhere else.
- A second copy of the app exits straight away.

## Transcription

- whisper.cpp transcribes with large-v3-turbo on the GPU (Silero voice detection skips silence and music); the app
  writes the lines to SQLite. Or any OpenAI-compatible transcription server set in Settings.
- Mode:
  - **Auto:** waits while a game is running or the GPU is busier than the limit (40 % by default), then catches up
    once it has been quiet for 60 s.
  - **Always:** transcribes no matter what.
  - **Paused:** only records.
- Games are a list of exe names. While one is running, Auto waits whatever the GPU load is.

## Daily summary

- At the ask time (21:00 by default) a notification says the summary starts in 5 minutes, with how much was said
  today. Buttons: Start now, In an hour, Delay until… (opens the delay box), Skip today.
- The AI is lifelog's own llama.cpp server with Qwen3 14B, started only while it writes. Or any OpenAI-compatible
  endpoint set in the settings.
- The summary is capped at a word limit (250 by default), reads the last few days' summaries so it doesn't repeat
  them, and leaves out topics on the "never mention" list.
- It picks out things you said you'd do ("I'll…", "remind me to…", "we should…") as reminders, with the time you
  said them.
- A journal day runs 04:00 to 04:00.
- If it fails, a notification offers Try again.

## Reminders

- After a summary, each reminder that said when ("tomorrow", "at 5") is offered as a notification: "Remind you
  tomorrow at 09:00?" with Remind me / No thanks. At most a few per summary. This can be turned off.
- When one is due, a notification shows what you said and when, with Done / In an hour.

## Window

Three tabs (Timeline, Journal, Settings) and a Listening / Paused button on the top right that pauses and resumes.
"lifelog is not running" shows if the recorder has stopped. Errors show as a short message at the bottom.

The window only exists while it is open. Closing it frees its memory; the recorder keeps going.

### Timeline

- One page per day, oldest at the top. Today starts at the bottom and grows as lines arrive. New lines write
  themselves out word by word.
- Each line: play button (if its audio is kept), time, app icon and name, text.
- A gap of a while between lines gets its own row. Files and links pasted in show among the lines at their time.
- At the bottom of today: "Listening", "Listening · writing up N more…" or "Paused".
- **Days:** ◀ day ▶ and Today on the right of the search bar, ← and → keys. The day opens a month calendar where
  days with something heard stand out.
- **Search:** "Search everything heard", across all days, newest first. Ctrl+F goes to the box. Enter and
  Shift+Enter step through matches like a browser's find; a match on another day opens that day. Matches are highlighted. Only shows
  what the Show filter lets through.
- **Show filter:** a chip per app heard on the page, most lines first. Click to hide or show that app.
  "All apps…" opens a window with every app ever heard: filter, Show all, Hide all, and "Show excluded apps".
  Hidden apps are also left out of what is copied and played. The filter is kept across restarts.
- **Picking lines:** click a line, Shift+click another to pick a stretch. Right-click:
  - on a stretch: Copy N selected lines, Copy selected audio, Copy selected as a file (Ctrl+Enter), Copy just this
    line, Clear selection (Esc).
  - on a line: Copy line, Copy line's audio.
- **Playback:** the play button plays
  - everything heard, mixed, playing on (click);
  - everything heard at that moment (Shift);
  - that line's app, playing on (Ctrl);
  - just that line (Ctrl+Shift).
  Hovering the button marks what it would play. The word being heard is highlighted. While playing: Stop (Esc)
  and Playing (scroll to it). When scrolled up: Now (jump to the bottom).
- **Files:** open (or open the link in the browser), Show in folder, Copy the path. Images show a preview.

### Journal

- Left: a month calendar and the month's days, newest first.
  - Bold days have a summary; a dot means things to do; plain means heard but no summary.
  - The list shows "· N to do", "· skipped" or "· no summary".
  - Today, This month, and the month arrows.
- Right: the day's summary.
  - "No summary yet" or "Skipped", with **Write it now**.
  - While writing, a spinner and what it is doing.
  - Each point, on hover: Show in timeline (if it has a time), Dismiss, Never mention this topic (type a topic,
    Save).
  - **You said you'd:** each reminder with its time and your words. Bell to turn its notification on or off,
    Show in timeline, Dismiss. A set bell stays lit.
  - "Written <day> <time> · <model>" and **Rewrite**. A dismissed point comes back on rewrite.

### Settings

- **Apps:** tabs New / Included / Excluded with counts, a search box, 10 per page.
  - Each row: icon, name, "N lines · x min ago", path; ✓ include, ✕ exclude; a gamepad to mark it as a game.
  - Right-click: Add as game / Not a game, Include, Exclude, Open file location.
  - Search with no match offers "Add <name>.exe".
- **Transcription:** Auto / Always / Paused, what it is doing, minutes waiting. In Auto, the GPU limit and the
  current load.
- **Daily summary:** on/off, what it is doing, ask time, word limit, Write today's now, reminder notifications
  on/off, and the never-mention topics (click one to allow it again).
- **Files:** open the recordings folder, the log and the config file.
- **Ctrl+Z** undoes the last change on this page (up to 50).

### Tray

- The icon shows listening (blue) or paused (grey, with a pause badge).
- **Left-click:** lifelog's own menu by the tray.
  - Listening switch.
  - Copy the last [15] min/h as File, Text or Audio, with a preview of the first lines in that stretch.
  - Timeline, Paste here, Settings, Restart, Quit.
  - Closes when you click elsewhere or press Esc.
- **Right-click:** a plain Windows menu: Pause/Resume, Restart, Quit.
- **Paste here:** saves the files, images or links on the clipboard into the timeline at the current time.
- **Delay box** (from the summary notification): pick a time (+2 h, +3 h, 23:30, or type one) and Delay.

## Copying

- Text: lines as "HH:MM app: text" on the clipboard.
- File: a `.txt` put on the clipboard as a file, so it pastes straight into Discord.
- Audio: the stretch mixed into one file on the clipboard.
- All three leave out hidden apps (and excluded ones unless they are shown).

## CLI and API

```
lifelog clip 15m | 1h | 2d | today
lifelog toggle | pause | resume
lifelog exclude X.exe | include X.exe
lifelog summary now | later | skip
lifelog status
lifelog check-ai
```

HTTP on 127.0.0.1:47800: `/status`, `/pause`, `/resume`, `/toggle`, `/exclude`, `/include`, `/game`,
`/not-game`, `/transcription`, `/clip`, `/summary`, `/remind`, `/show`, `/popup`, `/quit`, `/restart`.
Browsers can't call it (Host and Origin are checked).

## Install

- `scripts\install.ps1` builds, installs, starts at login and downloads the summary AI (skipped when an endpoint is
  set). `-Uninstall` keeps the data.
- **(idea)** An MSI with the programs, which downloads the worker and models on first run.

## Not built yet (from PLAN.md)

- Voice enrolment and speaker labels within one stream.
- Phone app: location and a meeting record button, uploads at home.
- Travel map page with an offline map.

## Your ideas

-
