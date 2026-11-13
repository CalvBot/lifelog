# Start lifelog hidden at login (per-user Run key, no admin needed).
#   scripts\autostart.ps1          turn on, and start it now
#   scripts\autostart.ps1 -Off     turn off, and stop it
param([switch]$Off)

$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
# lifelogw.exe starts "lifelog.exe run" with no window.
$exe = Join-Path (Split-Path $PSScriptRoot) 'target\release\lifelogw.exe'

if ($Off) {
    Remove-ItemProperty -Path $key -Name lifelog -ErrorAction SilentlyContinue
    Get-Process lifelog -ErrorAction SilentlyContinue | Stop-Process
    'lifelog will no longer start at login, and has been stopped.'
    return
}

if (-not (Test-Path $exe)) { throw "Build first: the build build --release ($exe not found)" }
Set-ItemProperty -Path $key -Name lifelog -Value ('"' + $exe + '"')
if (-not (Get-Process lifelog -ErrorAction SilentlyContinue)) {
    Start-Process $exe
}
"lifelog starts at login. Log: $env:LOCALAPPDATA\lifelog\lifelog.log"
