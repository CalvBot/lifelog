# lifelog: what it should do

This file is the requirements for a rewrite of the window and the state it reads. It lists what the app does today,
then how the parts should fit together. Edit it freely: cut what you don't want and add ideas under each heading.
Lines marked **(idea)** are not built yet.

Recording, transcription and the summary writer stay as they are. The rewrite covers the window, the tray menu and
how they get at the app's state.

## Architecture: one shared state

One program, `lifelog.exe`, started at login. The recorder, the tray icon and the summary writer run on background
threads, the window on the main thread. Transcription is whisper.cpp, loaded into the app while the GPU is free.

**One live state, shared in memory.** The app has a single `App` value that holds what the program is doing right
now: the settings, whether it is listening, which apps are being recorded, the transcription queue, the GPU load
and what the summary writer is doing. The main thread owns it; other threads never touch it (see **Threads**
below). There are no copies of it.

- The window reads `App` every time it draws, with no lock. Nothing is fetched, polled or turned into JSON.
- A button calls a method on `App`, for example `summary_set_max_words(app, 300)`. The method changes the value in memory,
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
writing to the database. Transcription runs inside it (whisper.cpp's `whisper.dll`, loaded at run time); summaries
are written by llama.cpp's `llama-server`, a child process it starts.

**Crashes say why.** Nothing stops the program without saying what it expected, what it got instead, and where.
Something that can go wrong in normal use (a missing drive, a busy port, an app that closed) is handled and shown,
not a crash.

**Slow work stays off the window's thread.** Writing a summary, loading the AI, cutting an audio clip and long
searches run on a thread. The thread reports its progress into `App` (for example "writing, 2 of 5 hours"), and the
window draws whatever is there, so it keeps responding even while the AI loads.

**The database.** Lines, files, summaries and reminders live in SQLite. The window reads them directly. A day's
lines are read once and kept until that day changes. The worker posts an event after each file it transcribes,
and the main thread reloads the lines on screen.

**Outside callers.** The CLI (`lifelog.exe`), a Stream Deck and notification buttons are other processes, so they
need a way in. A small local HTTP API stays for them only, and each route calls the same `App` methods. The window
doesn't use it.

## Threads: one owner, messages in

Before this change one lock (`App.lock`) guarded everything, and the window held it for the whole of every frame.
Every other thread waited on it: the audio threads after each packet (about every 10 ms), the transcriber, the
summary writer, the API. The frame also ran slow work under it: database queries (timeline day, search, journal),
config saves, `ShellExecute`. So the program ran one thing at a time. The plan below removed `App.lock`.

**The main thread owns `App`.** It is the only thread that reads or writes it, so it never locks it, and a frame
always sees one moment. The main thread does only quick things: apply messages, draw, start jobs.

**Other threads send messages.** One queue from every thread to the main thread, as clipback's `events.cpp` does: a
fixed ring of `Event { kind, a, b, text[512] }` behind a small lock that is held only to copy one event in or out. Posting
wakes the main thread with a message to the tray window (a window message, so it still arrives while a menu or a
window drag runs its own loop); the message loop applies every queued event, then redraws the window if
one is open. This works with the window closed, so the tray, API and notifications don't depend on it.

**Bigger results come as jobs.** A job is a struct with its own arena. The main thread fills in the request (what to
do, plus copies of whatever the thread needs from `App`), hands it to a thread, and gets it back in an event when it
is done. The main thread copies what it keeps into `App` and frees the job's arena in one go. No thread holds a
pointer into `App`.

**Settings the threads read** come one of two ways:

- **Hot flags are atomics** that the main thread writes and threads read without waiting: `paused`, `game_running`,
  `gpu_busy`, `summary_needs_gpu`, the transcription mode, `stop`. The GPU monitor is the one writer of `gpu_busy` and
  the GPU load, so it writes those atomics directly and wakes the main thread.
- **Everything else is a config snapshot.** When a setting changes, the main thread copies `Config` into a new block
  and swaps an atomic pointer to it. Threads read the current block and never see a half-made one. Old blocks are kept
  until the program ends; settings change a few times a day, so this is a few KB.

**Slow work leaves the main thread.**

- **Database:** a database thread with its own connection runs every query the window needs (a timeline day, search,
  the days with lines, the speakers, a journal day) and every write from the window (dismiss, never-mention,
  reminders, skip). Results come back as jobs. The window shows what it had until they arrive.
- **Files:** a writer thread does `config.toml` saves, `paths.txt` appends, the summary's Markdown file and opening
  files and folders (`ShellExecute`). The main thread hands it the finished text, so a click never waits on the disk.
- **Small tasks go to a pool, not a thread each.** Paste, file open, previews, timeline jobs, copy, the folder
  picker, the journal's days and setup downloads are jobs on one queue that a few fixed worker threads take, and
  their results come back as events. Playback keeps its own thread, since it runs for as long as the sound does.

**Fewer, quieter threads.**

- **One process scan.** The apps watcher and the capture watcher both list every process once a second. One watcher
  does it, with the windows and the playing sessions, and posts one job that the main thread uses for both the
  running apps and the streams.
- **Woken, not polling.** The transcriber sleeps until a capture thread says it wrote a WAV or a setting changed
  (a one-minute time-out stays as a safety net), and lists the spool folder only then.
- **UI state belongs to the main thread.** The globals the window files keep (`g_tl_jobs`, `g_copy`, `g_heard`,
  `g_pick_decision` and the rest) are touched only by the main thread; the threads that fill them send events.
- **Reused job arenas.** A thread that posts a job every second (the watcher) keeps two arenas and uses them in
  turn, instead of reserving and freeing a new one each time.

**Each thread, after the change:**

| Thread | Reads | Sends to the main thread |
|---|---|---|
| Audio streams (mic, one per app) | atomics: `paused`, `stop`; its own copy of exe, pid, device. The window title is the one value the watcher changes during a stream: it sits in the stream's own small block behind its own small lock. | segments go to disk as now; "loud at", "device", "ended" events. The mic also hands each sentence to the voice thread's queue. |
| Voice commands | atomics: `listen`, `paused`; the transcriber's config snapshot (for the AI folder); woken by a sentence | commands through the API's handler (`api_ask`), then the chime and "last heard" |
| Watcher (every 1 s; today the capture and apps watchers, merged into one) | config snapshot | the processes, windows and playing sessions with their paths and titles; the main thread decides which streams to start or end, and which apps run |
| Apps scan | config snapshot | new paths, icons and names (as a job), the line counts, installed games |
| Transcriber | atomics, config snapshot; woken by "new WAV" and setting changes | state, "lines written" |
| GPU monitor | atomics | writes `gpu_busy` and the GPU load itself |
| Summary (every 15 s) and summary writer | atomics, config snapshot; the writer gets the day's app names in its job | phase, countdown, done or failed, the new summary lists |
| Notifications, API | none | the command as an event. The API waits for the answer (with a time-out); the main thread builds `/status` and replies. |
| Database, file writer | the job | results as jobs |

**Locks that stay** are small ones around a few bytes, never held while drawing, waiting, or touching the disk: the
event queue, each stream's title block, the notification queue and the log file.

**Order of work.** Each step builds, runs, and is used for a while before the next:

1. Event queue, jobs, atomics and config snapshots; move capture (watcher and streams) and the GPU monitor onto them.
   Done (`app/events.h`). The snapshot is published by the main thread after each frame and each round of events.
2. Transcriber, summary and summary writer, notifications, setup; the transcriber and summary woken instead of
   polling; one watcher for processes and streams. Done so far: the transcriber (woken), and the summary, writer,
   notification links and setup, which take no lock. The summary thread still looks every 15 s, since reminders
   come due on the clock too; it only reads the database and posts what it read, and the main thread decides
   whether to ask, count down or start. Then one watcher: the capture watcher's process scan also gives the app
   list's "running" and the game check, and paths of exes new this run; the apps thread only fetches icons and
   line counts. Done.
3. Database thread and file writer; nothing in a frame touches the disk. Done (`app/disk.h`, `db_post` in
   `app/db.h`): the main thread has no connection; the window's queries and its summary changes (reminders,
   dismiss, never-mention, skip) run on the database thread, and config.toml and paths.txt are written by the
   file writer.
4. API and the small window threads on the worker pool, with the UI globals owned by the main thread; then delete
   `App.lock`, `app_lock` and `app_unlock`. Done (`app/pool.h`): copying, pasting, opening files and folders,
   previews, the clipboard, the folder picker and app icons run on three pool threads; the API thread hands each
   request to the main thread and waits for its answer; the apps-watch thread is gone (icons on the pool, line
   counts on the database thread); playback posts where it is and when it ends. `App.lock` is deleted. Left: the
   summary thread still looks every 15 s, and the watcher still makes a new arena for each post.

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
- **Memory grows in place (planned).** An arena reserves 4 GB of address space and commits pages as they are
  reached, so nothing needs `malloc`, `realloc` or copying. Today `List` is a heap array grown with `realloc` and freed
  by hand (about 70 `list_free` calls). `Buf`, the text builder, also pushes onto a heap list one character at a
  time, then copies the text into an arena. The plan:
  - **Text builders** write straight onto the end of the arena that keeps the result. Finishing adds the zero; there
    is no copy and nothing to free. A check catches anything else pushing onto that arena mid-build.
  - **Short-lived lists** (query results, work lists) grow the same way in the scratch, frame, day or job arena.
  - **Long-lived lists** in `App` (apps, streams, the config's name lists) each get their own reserved block that
    grows in place. Items never move, so a pointer to one stays good.
- **Asserts with messages** in place of `unwrap()`: what was expected, what came instead, and the file and line.
- Little of the C++ standard library; a few single-file libraries vendored into the repo:
  - Dear ImGui for the window, on Win32 and Direct3D 11;
  - SQLite (the amalgamation);
- Windows calls (WASAPI process loopback, the tray icon, notifications) made directly, and a small hand-written
  TOML reader for `config.toml`.
- whisper.cpp (whisper.dll, loaded at run time) and llama.cpp (llama-server, a child process), downloaded from Settings.
- One `lifelog.exe`.

## Recording

- Records the mic and every app that plays sound, each as its own stream, with the app's exe, process and window
  title.
- The mic is the Windows default, followed when it changes, unless one is picked in Settings. A picked mic
  that is unplugged falls back to the default until it is back. Everything is recorded as 16 kHz mono.
- Private mode (paused) records nothing and drops the sentence in progress.
- New apps are recorded until you decide: included (keep recording) or excluded (record nothing).
- Excluded apps can still count as games.
- A folder can be included or excluded as a whole: every app in it, subfolders too. An app's own
  choice wins over its folder's, and a deeper folder's over the one above it.
- Apps are shown by their own name (Start menu, or the exe's description), else the exe name.
- Discord calls can be recorded one track per person (the VoiceStreams Vencord plugin plays each person through their own app and silences them in Discord, so Discord is recorded like any other app).
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
- Games are a list of exe names. While one is running, Auto waits whatever the GPU load is (unless
  "Pause while games are running" is off).

## Daily summary

- At the ask time (21:00 by default) a notification says the summary starts in 5 minutes, with how much was said
  today. Buttons: Start now, In an hour, Delay until… (opens the delay box), Skip today.
- The AI is lifelog's own llama.cpp server with Qwen3 14B, started only while it writes. Or any OpenAI-compatible
  endpoint set in the settings.
- The summary is capped at a word limit (250 by default), reads the last few days' summaries so it doesn't repeat
  them, and leaves out topics on the "never mention" list.
- The AI's instructions (tone, what to focus on) are built in, or `summary-prompt.txt` next to config.toml when it
  has text. **Edit prompt** in the Daily summary card creates it with the built-in text and opens it; deleting it
  goes back to the built-in one. The output format (JSON shape, reminder rules) stays in code.
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
- **Show filter:** one button saying how many of the day's apps are shown. It opens a list to search and tick:
  the day's apps first (most lines first), then every other app ever heard. Show all and Hide all act on what the
  list shows; excluded apps are left out unless "Show excluded apps" is on.
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

- **Processes:** tabs New / Included / Excluded / Folders with counts, a search box, 10 per page. Every
  tab is as tall as a full page. Card titles are bold, on their own line.
  - Each row: icon, name, "N lines · x min ago", path; ✓ include, ✕ exclude; a gamepad to mark it as a game.
  - Right-click: Add as game / Not a game, Include, Exclude, Open file location, Include / Exclude every app in its folder, Remove from list (forgets its choices and hides it until it plays sound again, then it is under New).
  - Folders: each folder rule with its app count; switch it, Remove it, or add one with a folder picker.
  - Search with no match offers "Add <name>.exe".
- **Transcription:** Auto / Always / Paused, what it is doing, minutes waiting. In Auto, "Pause while games are running",
  the GPU limit and the current load.
- **Daily summary:** on/off, what it is doing, ask time, word limit, Edit prompt, reminder notifications
  on/off, and the never-mention topics (click one to allow it again).
- **Voice commands:** on/off with their state (a download button when the model is missing), the wake word, a
  Show all button that opens a table of what to say and what it does, the last one heard and what came of it, and the chimes they make (listening, done, clip saved, didn't
  catch, error, cancelled). Glass (a warm bell, like an electric piano), Soft (a low marimba), Synth (a mellow synth) or Off, a volume, and
  a button to hear each one. They are made in code (`chime.cpp`), not sound files: low and warm, around middle C
  with a body an octave below, in stereo with a soft echo and a little room. lifelog never records its own sound.
- **Custom commands:** each command with a switch, an Allow button when it is new or its files changed, its
  phrases, a password field per secret, and any error in its `command.toml`; then Open folder, Reload, and a box
  to try a sentence, which shows what it matches and has a Run button. Opening Settings reads the folder again.
- **Files:** open the recordings folder, the log and the config file.
- **AI models:** the transcription and summary models on this computer with their sizes; delete one, remove all
  of lifelog's AI, or move the AI folder to another drive.
- **Ctrl+Z** undoes the last change on this page (up to 50).

### Tray

- The icon shows listening (blue) or paused (grey, with a pause badge).
- **Left-click:** lifelog's own menu by the tray.
  - Listening switch.
  - Copy the last [15] min/h as File, Text or Audio, with a preview of the first lines in that stretch.
  - Timeline, Paste here, Settings.
  - Closes when you click elsewhere or press Esc.
- **Right-click:** a plain Windows menu: Pause/Resume, Restart, Quit.
- **Paste here:** saves the files, images or links on the clipboard into the timeline at the current time.
- **Delay box** (from the summary notification): pick a time (+2 h, +3 h, 23:30, or type one) and Delay.

## Voice commands

- Say "lifelog" and a command as one sentence ("lifelog, summarise the last hour"), or "lifelog" alone: it chimes and
  waits 6 s for the command. A "hey" or "ok" before the name is fine. The name in the middle of a sentence ("I was
  working on lifelog") is ignored.
- The wake word is "lifelog" unless changed in Settings (`[voice] wake_word`): at least 3 letters or digits, and
  only letters, digits, spaces, - and '. A word of 6 or more letters also matches when one letter is heard wrong
  ("jarvus" for "jarvis") or split in two ("life log"). Whisper's prompt names it, which makes it heard more often.
- The commands:
  - **summarise** (or "sum up", "recap") with a spoken range ("the last hour", "fifteen minutes", "half an hour",
    "an hour and a half", "today"; 15 minutes by default): the summary AI writes a few sentences about it from
    the newest 24,000 characters of the transcript. They go on the clipboard and show as a notification.
  - **remind me** / **set an alarm** / **set a timer** / **wake me up**: "in 10 minutes", "in an hour and a half",
    "at 5", "at 7 30 pm", "at noon", "tomorrow at 9", "in the morning". A time without am or pm is the next one
    ("at 5" at 10:00 is 17:00) and a passed time is tomorrow's; "tomorrow" alone is 09:00. What it is about is the
    words after "to", "that" or "about" ("Alarm", "Timer" or "Reminder" if none). It is saved like the summary's
    reminders (the alerts table) and shows as a notification when due.
  - **open** / **launch** / **start** / **play** APP: starts an installed app or game from its exe.
  - **pause** / go private and **resume** / stop private mode.
  - **pause transcription** and **continue transcription**, which goes back to the mode it was in before the
    pause (`[processing] mode_before_pause`; auto if it was never paused).
  - **check the AI**, and **cancel** / **never mind**.
- An app is named as it is said: its exe without ".exe" or its display name ("spotify", "visual studio code"),
  a running one first. The API takes such names too.
- The mic has a second segmenter for this (`capture.cpp`). It hears even in private mode and ends a sentence after
  0.7 s of quiet. The voice thread transcribes each sentence with a small Whisper model on the processor (greedy, 4
  threads, below-normal priority), so it works while the transcriber waits for a game. A sentence that is not for
  lifelog is dropped without being logged.
- The model is the listener's own (`[voice] model`), picked in Settings: base.en (0.15 GB, the default), tiny.en
  (0.08 GB, lightest) or small.en (0.49 GB, hears best). Changing it loads the new one at once; one not yet
  downloaded shows a Download button. On 20 s clips, CPU only, each took about 1.0, 1.4 and 4.3 s.
- There is no built-in "clip": the phrase is left for a custom command, such as one that saves an OBS replay.
- In private mode only resume is carried out, and nothing about the sentence is kept.
- Open, private mode and transcription go through the API's own handler, so they do exactly what the CLI's
  do; a chime then says how it went. Its sentence also goes into the transcript as usual.
- When a command works, a banner shows at the top of the screen with the window in front, for 2.5 s, then fades:
  the wake word and what was done ("Jarvis — Alarm set: 4 minutes"). It takes no focus or clicks (`banner.cpp`).
- `lifelog voice "TEXT"` prints what a sentence would do, without doing it.
- Custom commands (`commands.cpp`, [docs/custom-commands.md](docs/custom-commands.md)) are matched on all the
  words after the wake word, polite ones included, so a slot can hold "you" ("play You Belong With Me"). A phrase
  that starts the sentence is tried before the built-in commands; one later in it only when no built-in matched.
  The script runs on its own thread, one at a time, in a job object that ends it and its children after 20 s.
  None run in private mode.

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
lifelog summarise 15m | 1h | today
lifelog transcription auto | always | paused | continue
lifelog launch APP
lifelog mics
lifelog status
lifelog check-ai
lifelog voice "lifelog, summarise the last hour"
lifelog commands [allow ID | secret ID KEY]
lifelog command "play bad guy"
```

HTTP on 127.0.0.1:47800: `/status`, `/pause`, `/resume`, `/toggle`, `/exclude`, `/include`, `/game`,
`/not-game`, `/transcription` (`mode=continue` too), `/launch?app=`, `/setup`, `/clip`, `/summary`, `/remind`, `/show`, `/popup`, `/quit`, `/restart`.
Browsers can't call it (Host and Origin are checked).

## Install

- `scripts\build-msi.ps1` builds `lifelog-<version>.msi`, which installs per user, starts at login and holds no
  AI; Settings downloads the transcriber and summary AI.

## Not built yet (from PLAN.md)

- Voice enrolment and speaker labels within one stream.
- Phone app: location and a meeting record button, uploads at home.
- Travel map page with an offline map.

## Your ideas

-
