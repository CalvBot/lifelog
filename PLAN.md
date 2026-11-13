# lifelog — always-on personal transcript (plan)

Goal: a searchable, private record of everything said and heard on this PC, plus a phone app for location history and
meeting recordings, with a quick "clip the last N minutes" button and a short daily summary from a local AI. No wearable device.

## What it does
- Listens all the time to: my mic, and each app that plays audio (Discord, browser, games, video players), as **separate streams**.
- Transcribes locally, labels each line with: time, speaker, app (exe + PID + window title), source device.
- Skips excluded apps (list of exe names) and has a "private mode" pause (hotkey + Stream Deck + tray).
- Stores text forever on this PC; raw audio optional with a retention window.
- Stream Deck: "clip last 15m / 1h / today / custom" → a .txt file is put on the clipboard (as a file, so it pastes straight into Discord).
- End of day: a local AI writes a short summary (word limit set in settings), including things I said I'd do.
- UI: calendar → day timeline → read, search, filter by speaker or app.

## Hardware check (this PC)
RTX 4070 Ti 12 GB, Ryzen 7 7800X3D, 64 GB RAM, 2.6 TB free on G:. Enough for real-time Whisper on several streams plus
a 14B-class local model for the nightly summary. Whisper shares the GPU with games, so transcription runs in a queue and may
lag a few minutes while gaming; that's fine for a log.

## Architecture
```
capture (native service)            worker (Python)                    storage                  front ends
 mic (WASAPI)            ─┐        VAD (Silero) → chunks               SQLite + FTS5 (text,     tray app
 per-app loopback (WASAPI ├─16kHz─► faster-whisper large-v3-turbo ───► speakers, app, PID,      Stream Deck plugin
  process loopback, PID)  │        speaker labelling                    window title)           web UI (calendar)
 app/window watcher      ─┘        nightly: summary via Ollama          Opus audio (optional)   ntfy daily summary
```
- **Per-app audio with PID:** Windows "process loopback" capture (Win10 2004+) records one process tree's audio. The
  watcher enumerates active audio sessions, starts a stream per non-excluded app, and records the window title
  (e.g. the YouTube video name) alongside.
- **Capture natively** (the `wasapi` package supports application loopback); ML in Python (faster-whisper, pyannote).
  The same capture core can feed the clipping tool (see `../clipback/PLAN.md`).
- **Speakers:**
  - Mic = me (my voice is enrolled, so other voices picked up by the mic are flagged).
  - Discord: best option is a small **Discord bot** that joins my calls and receives each user's audio separately, so
    names are exact and friends can see it's recording. Without the bot, the Discord stream is diarized (pyannote) and
    matched against enrolled voice prints of friends.
  - Videos/music: speaker = app + title; diarized as "Speaker 1/2" within it.
- **Storage:** `G:\lifelog\` (SQLite DB + audio). Encrypt (BitLocker on G:, or SQLCipher for the DB). Nightly backup
  to pve/Nextcloud. Rough size: text ~5–20 MB/day; Opus 16 kbps audio ~170 MB per stream-day.
- **Daily summary:** Ollama with a ~14B model (Q4, fits 12 GB). Runs when idle at night: summarise each hour, then
  the day, capped at N words. Pulls out "I'll…/remind me/we should…" lines as reminders. Sent via ntfy + stored with the day.
- **Clip to clipboard:** the Stream Deck plugin calls the service's local API → writes `lifelog-<range>.txt` to temp →
  puts it on the clipboard as a file (CF_HDROP).

## Phases
1. **Core:** mic + per-app capture, exclusions, transcription, SQLite, clip button. (Usable on its own.)
2. **Speakers:** voice enrolment, diarization, Discord bot.
3. **UI:** calendar, timeline, search, filters, private-mode controls.
4. **Daily summary** via Ollama + ntfy.
5. **Phone app (Flutter) — deliberately simple.** It only collects and hands off; everything is viewed on the PC.
   - Logs location in the background all the time.
   - One record button for meetings.
   - When it reaches the PC on home Wi-Fi: uploads everything new, PC confirms it arrived intact, phone deletes it.
     No viewing, maps or history on the phone.
6. **Travel history on the desktop:** a map page in the lifelog UI, like the old Google Timeline: pick a day on the
   calendar → that day's route as one line, with stops marked; zoom out to see many days. Fully private: the map uses
   an **offline map file** (OpenStreetMap data, e.g. a Protomaps file for WA/Australia) stored on the PC, so no map
   provider ever sees where I've been.

## Things to decide
- Keep raw audio (and for how long), or text only?
- Discord bot (visible, exact names) vs diarization only?
- Storage drive and encryption method.
## Legal (WA) — design rules
Surveillance Devices Act 1998 (WA): recording a private conversation, even as a party, needs every speaker's consent
(express or implied); a transcript counts as a record, so deleting audio doesn't change this. Sharing one is a separate
offence (s9). Up to $5,000 / 12 months. So:
- Own voice, videos, music, public streams: fine.
- Discord: only with consent: visible bot + friends told. Option to skip calls/users who haven't agreed.
- Meetings (phone record button): only when everyone knows — record openly and say so.
- Location history: my own data, no consent issue.
- Clip button defaults to my own lines + consenting speakers.
- Not legal advice; check with a WA lawyer before relying on implied consent.
