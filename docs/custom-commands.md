# Custom voice commands

Voice commands that you add yourself. lifelog understands what was said, and a small script does the work.
For example, "Jarvis, play Bad Guy" can go to a Spotify script, "Jarvis, download Iron Man" to a Jellyfin one,
and "Jarvis, turn on my lights" to Home Assistant. lifelog knows nothing about those services; each one is a
folder with a script in it.

Status: step 1 (the core) works. Steps 1b to 4 are plans.

To start, copy [commands/example](../commands/example) from this repo into `%APPDATA%\lifelog\commands\`, open
Settings, and allow it. Then "lifelog, say hi to Sam" shows "Hello, Sam!" at the top of the screen.

## A command is a folder

Commands live in `%APPDATA%\lifelog\commands\`, one folder each:

```
commands\spotify\
  command.toml
  spotify.py
```

`command.toml` says how to run the script, which secrets it needs, and what to say for each action. Each
`[table]` is one action:

```toml
name = "Spotify"
description = "Plays music on Spotify"
run = "python spotify.py"
secrets = ["client_id"]

[play]
say = ["play {song}", "put on {song}"]
description = "Play a song, an album or an artist"

[find_by_lyrics]
say = ["the song that goes {lyrics}", "what's the song that goes {lyrics}"]
description = "Find a song from some of its words and play it"

[skip]
say = ["skip", "next song"]
```

`run` is any command line. It runs in the command's folder, so Python, PowerShell or an `.exe` all work.
The templates use Python with only its standard library, so there is nothing to install with pip.

## How a sentence is matched

The sentence has to start with the wake word, as it does for the built-in commands. After that, matching is
loose:

- Filler words are dropped: "um", "uh", "ah", "oh", "please", "can you", "could you", "I think" and the like.
- The words of a phrase must come in order, but they can be anywhere in the sentence. A word may have an
  ending added: "play" also matches "playing".
- A `{slot}` takes the words between its neighbours. A slot at the end takes the rest of the sentence.
- When several phrases match, the one with the most fixed words wins.
- A phrase that starts the sentence ("Jarvis, play Bad Guy") is tried before the built-in commands. A phrase
  found later in the sentence is tried after them. So "Jarvis, remind me to play guitar" is still a reminder.

"Jarvis, um, can you play Bad Guy by Billie Eilish" gives `spotify.play` with song = "bad guy by billie
eilish". Slots arrive in lower case without punctuation; the services searched with them do not mind.

From a terminal:

- `lifelog voice "TEXT"` prints what a sentence would match, without running anything.
- `lifelog commands` lists the commands, their phrases, and whether each is allowed.
- `lifelog command "play bad guy"` runs the command a sentence matches (without the wake word) and prints its
  answer.
- `lifelog commands allow ID` allows a command, as the Settings button does.
- `lifelog commands secret ID KEY` saves a secret typed or piped in, so it is not left in the shell's history.

## What the script gets and gives back

lifelog writes one JSON object to the script's standard input:

```json
{
  "action": "play",
  "slots": {"song": "bad guy by billie eilish"},
  "said": "um can you play bad guy by billie eilish",
  "secrets": {"client_id": "..."},
  "data_dir": "C:\\Users\\you\\AppData\\Local\\lifelog\\command-data\\spotify"
}
```

`said` is the command's own sentence after the wake word, in lower case without punctuation. A script never gets
the rest of the transcript.
`data_dir` is the script's own folder for anything it keeps, such as a login token.

The script prints one JSON line as its last line of output:

```json
{"ok": true, "banner": "Playing bad guy · Billie Eilish"}
{"ok": false, "message": "Spotify is not open on any device"}
```

- `ok: true` plays the done chime and shows `banner` at the top of the screen.
- `ok: false` plays the error chime and shows `message` in Settings beside the last command.
- A script that prints no JSON line counts as `ok` when it exits with code 0. The last line it printed is used
  as the message.
- A script has 20 seconds. After that it is stopped, along with anything it started.
- Python scripts run with `PYTHONUTF8=1`, so standard input and output are UTF-8.

## Safety

- A new command does not run until you allow it in Settings. If its files change after that, it has to be
  allowed again. lifelog keeps a hash of the folder's files to tell.
- Secrets such as tokens are typed into Settings and stored encrypted for your Windows account (DPAPI), in
  `%LOCALAPPDATA%\lifelog\secrets\`. They are never in the command's folder, so a template can be shared
  without them.
- Custom commands do not run in private mode.

## Settings

A "Custom commands" card lists each command. For each one it shows:

- a switch to turn it on or off, and an Allow button when it is new or has changed;
- its phrases;
- a field for each secret;
- any error in its `command.toml`.

Below the list are Open folder, Reload, and a box to type a sentence and see what it matches, with a button to
run it.

## Step 1b: an AI for loose wording, and "that"

Phrase matching cannot cope with everything:

- "Jarvis, what's that song, it goes like... it's Bad Guy by Billie Eilish I think"
- "...Living on a Prayer, that'd be a good one... Jarvis, can you play that song?"
- "I need to take the pizza out in ten minutes. Jarvis, can you set an alarm for that?"

When the wake word was said and no phrase matched, or the sentence says "that" or "it" where a slot should be,
a small local AI picks the command instead. It gets:

- the sentence;
- the last two minutes of what you said into the mic, so "that" can be found;
- the list of commands, built-in and custom, with each action's description and slots.

It answers in JSON: which command, which action and which slots, or none. Its answer goes through the same path
as a matched phrase, and the banner shows what it chose.

The last two minutes come from the voice listener, which already runs every mic sentence through its small
Whisper model to look for the wake word. They are kept in memory only, never saved, and cleared in private mode.
This works while the main transcription waits for the GPU. Only the local AI sees them.

The AI is a small model (about 2 to 4 billion parameters) that runs on the processor, so it does not wait for
the GPU while a game runs. It should answer in 2 to 5 seconds. The listening chime plays while it thinks.

## Step 2: media keys and Home Assistant

- Built in, with no script: pause, play, skip, previous, and "what's playing", for any player, through the
  Windows media controls.
- A Home Assistant template sends the sentence to Home Assistant's own Assist API with a long-lived token. Home
  Assistant then understands "turn on my lights", "set the bedroom to 20 degrees" and so on without a mapping.

## Step 3: Spotify

- A one-time sign-in in the browser using Spotify's PKCE login flow. It needs a client ID from a free Spotify
  developer app, but no client secret. The script keeps a refresh token in its `data_dir`.
- "Play X" searches Spotify and plays the top track on this PC, starting Spotify first if it is not running.
- "The song that goes X" searches Genius by lyrics (a free token), then plays the top match on Spotify.
- Queue, like this song, what's playing, volume.
- Playback control needs Spotify Premium.

## Step 4: Jellyfin downloads

"Jarvis, download Iron Man":

1. The script searches Jellyfin for the title, using a Jellyfin API key.
2. If it is there: "Iron Man (2008) is already in Jellyfin".
3. If not, it asks the downloader to queue the top match: "Queued Iron Man (2008)".

This needs the downloader to have an API that takes a token. That is a separate piece of work on the server.
