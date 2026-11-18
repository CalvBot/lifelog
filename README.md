![lifelog](assets/banner.svg)

A local, searchable transcript of everything said and heard on this PC: your mic and each app that plays sound,
transcribed on the GPU. Each evening a local AI summarises the day. Nothing leaves the PC.

## Install

Run `lifelog-<version>.msi` (per user, no admin). In **Settings**, pick a transcription model and a summary model
to download, or point either at your own OpenAI-compatible server.

- Settings: `%APPDATA%\lifelog\config.toml`
- Data and models: `%LOCALAPPDATA%\lifelog`
- Uninstalling keeps both.

Build it yourself: `app\build.bat` (Visual Studio 2022). `scripts\build-msi.ps1` builds the installer (WiX 3).

## Use

- **Tray:** left-click for the menu (pause, copy the last few minutes as text or audio); right-click to pause or quit.
- **Timeline:** one page per day, searchable. Lines from the last 7 days can be played back.
- **Journal:** daily summaries and reminders.
- **Settings:** which apps to record, which are games, when to transcribe, and voice commands.
- **Voice commands:** "lifelog, summarise the last 30 minutes", "lifelog, remind me in 10 minutes to check the
  oven", "lifelog, open Spotify", "lifelog, skip". The wake word can be changed.
- **Custom commands:** your own phrases that run a script. See [docs/custom-commands.md](docs/custom-commands.md).

## CLI

```powershell
lifelog clip 15m                 # copy the last 15m (or 1h, today)
lifelog toggle                   # private mode; also pause, resume
lifelog summarise 30m            # a few sentences about the last 30 minutes
lifelog voice "lifelog, skip"    # what a voice command would do
lifelog commands                 # list custom commands
lifelog status
lifelog mcp                      # MCP server for AI agents
```

Local API on `127.0.0.1:47800`: `/status`, `/pause`, `/resume`, `/toggle`, `/clip?range=15m`.

## AI agents (MCP)

```powershell
claude mcp add lifelog -- "<path to lifelog.exe>" mcp
```

The agent gets read-only tools: `transcript`, `search`, `summary`, `status`. To limit how far back they can read,
set `reach = "15m"` (or `1h`, `7d`) under `[mcp]` in `config.toml`. Speakers hidden in the timeline are never
shared. An agent using a cloud model sends what it reads to that provider.

## Privacy

- Get consent from the people you record. Excluded apps and private mode record nothing.
- Voice commands listen even in private mode (so you can say "resume"); what they hear is never saved.
- Audio is deleted after 7 days; the text stays.

## Contributing

GitHub is a mirror and is overwritten on every sync. Open a pull request there; it gets merged on the main copy.
