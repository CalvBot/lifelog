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
    Get-Process lifelogw, lifelog, lifelog-ui -ErrorAction SilentlyContinue | Stop-Process -Force
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

Stop-Lifelog
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $repo 'target\release\lifelog.exe'), (Join-Path $repo 'target\release\lifelogw.exe'), (Join-Path $repo 'target\release\lifelog-ui.exe') $dest -Force
# The worker (Python + its 2 GB of libraries) stays in the repo; the app just links to it.
$link = Join-Path $dest 'worker'
if (-not (Test-Path $link)) { New-Item -ItemType Junction -Path $link -Target (Join-Path $repo 'worker') | Out-Null }

$app = Join-Path $dest 'lifelogw.exe'
Set-ItemProperty -Path $runKey -Name lifelog -Value ('"' + $app + '"')
Start-Process $app
"lifelog installed in $dest and started; it will start at every login. Look for the red speech bubble in the tray."
