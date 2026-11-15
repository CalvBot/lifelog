# Build lifelog and install it as a per-user app that starts (hidden, in the tray) at login.
#   scripts\install.ps1              build, install or update, and start
#   scripts\install.ps1 -Uninstall   stop, remove the app and its autostart (data and settings are kept)
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot
$dest = Join-Path $env:LOCALAPPDATA 'Programs\lifelog'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

function Stop-Lifelog {
    $cli = Join-Path $dest 'lifelog.exe'
    if ((Get-Process lifelogw, lifelog -ErrorAction SilentlyContinue) -and (Test-Path $cli)) {
        & $cli quit 2>$null | Out-Null   # clean stop: lets the worker finish its file
        Start-Sleep 3
    }
    $running = @(Get-Process lifelogw, lifelog, lifelog-ui -ErrorAction SilentlyContinue)
    $running | Stop-Process -Force
    # Killed processes let go of their exe a moment later.
    $running | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
}

if ($Uninstall) {
    Stop-Lifelog
    Remove-ItemProperty -Path $runKey -Name lifelog -ErrorAction SilentlyContinue
    if (Test-Path $dest) {
        # Remove the link to the worker first so the repo's worker folder is not touched.
        $link = Join-Path $dest 'worker'
        if (Test-Path $link) { (Get-Item $link).Delete() }
        Remove-Item -Recurse -Force $dest
    }
    'lifelog removed. Your data folder and settings were left alone.'
    return
}

Push-Location $repo
try { the build build --release --workspace; if ($LASTEXITCODE) { throw 'build failed' } } finally { Pop-Location }
if (-not (Test-Path (Join-Path $repo 'worker\.venv\Scripts\python.exe'))) {
    throw 'worker\.venv is missing: see Setup in README.md'
}

# The summary AI (fetched once; quick when it is already there), unless the settings name an endpoint.
$config = Join-Path $env:APPDATA 'lifelog\config.toml'
if (-not ((Test-Path $config) -and (Select-String -Path $config -Pattern '^\s*endpoint\s*=\s*"[^"]+"' -Quiet))) {
    & (Join-Path $PSScriptRoot 'get-llm.ps1')
}

Stop-Lifelog
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $repo 'target\release\lifelog.exe'), (Join-Path $repo 'target\release\lifelogw.exe') $dest -Force
# The window used to be its own program; it is part of lifelogw.exe now.
Remove-Item (Join-Path $dest 'lifelog-ui.exe') -ErrorAction SilentlyContinue
# The worker (Python + its 2 GB of libraries) stays in the repo; the app just links to it.
$link = Join-Path $dest 'worker'
if (-not (Test-Path $link)) { New-Item -ItemType Junction -Path $link -Target (Join-Path $repo 'worker') | Out-Null }

$app = Join-Path $dest 'lifelogw.exe'
Set-ItemProperty -Path $runKey -Name lifelog -Value ('"' + $app + '"')
Start-Process $app
"lifelog installed in $dest and started; it will start at every login. Look for the blue speech bubble in the tray."
