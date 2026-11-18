# Custom voice commands

Voice commands that you add yourself. lifelog understands what was said, and a small script does the work.
For example, "Jarvis, play Bad Guy" can go to a Spotify script, "Jarvis, download Iron Man" to a Jellyfin one,
and "Jarvis, turn on my lights" to Home Assistant. lifelog knows nothing about those services; each one is a
folder with a script in it.

To start, copy [commands/example](../commands/example) from this repo into `%APPDATA%\lifelog\commands\`, open
Settings, and allow it. Then "lifelog, say hi to Sam" shows "Hello, Sam!" at the bottom right of the screen.

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

- `ok: true` plays the done chime and shows `banner` at the bottom right of the screen.
- `ok: false` plays the error chime and shows `message` in Settings beside the last command.
- A script that prints no JSON line counts as `ok` when it exits with code 0. The last line it printed is used
  as the message.
- A script has 20 seconds. After that it is stopped, along with anything it started. To start something that
  keeps running after the script ends, start it with the `CREATE_BREAKAWAY_FROM_JOB` flag (see the next section).
- Python scripts run with `PYTHONUTF8=1`, so standard input and output are UTF-8.

## Handing a request to an AI agent

A command can pass what was said to an AI agent, such as Claude Code, and leave the work to it. With lifelog's
MCP server added to the agent (`claude mcp add lifelog -- <path to lifelog.exe> mcp`, see the README), the agent
can read the conversation. So after talking about a song, "Jarvis, ask Claude to play that song" works: the
agent searches the transcript to find out which song "that song" is.

`command.toml`:

```toml
name = "Claude"
description = "Passes a request to Claude Code, which can read the transcript"
run = "python agent.py"

[ask]
say = ["ask claude to {request}", "ask claude {request}"]
description = "Anything; Claude reads the recent conversation when the request refers to it"
```

`agent.py`:

```python
import json, shutil, subprocess, sys

job = json.load(sys.stdin)
prompt = ("Said to my voice assistant just now: " + job["slots"]["request"] + "\n"
          "Use the lifelog tools to read the recent conversation if the request refers to it.")
# An agent takes longer than the 20 seconds a script has, so it is started apart from the script's job.
subprocess.Popen([shutil.which("claude"), "-p", prompt, "--allowedTools", "mcp__lifelog"],
                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                 creationflags=subprocess.CREATE_BREAKAWAY_FROM_JOB | subprocess.CREATE_NO_WINDOW)
print(json.dumps({"ok": True, "banner": "Asked Claude"}))
```

Add the agent's own tools to `--allowedTools` for anything else it should be able to do, such as playing music.
The script ends at once, so the banner only says the request was passed on. Any other agent with a
command-line mode works the same way. An agent with a cloud model sends what it reads from the transcript to
that model's provider.

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
