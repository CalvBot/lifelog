# Builds the release lifelog.exe and packs it into dist\lifelog-<version>.msi (WiX Toolset 3).
# The MSI holds no AI; see installer\lifelog.wxs.
#   scripts\build-msi.ps1
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot
$wix = @("$env:WIX\bin", "${env:ProgramFiles(x86)}\WiX Toolset v3.14\bin", "${env:ProgramFiles(x86)}\WiX Toolset v3.11\bin") |
    Where-Object { $_ -and (Test-Path "$_\candle.exe") } | Select-Object -First 1
if (-not $wix) { throw 'WiX Toolset 3 is not installed (https://github.com/wixtoolset/wix3/releases)' }

# Version 0.1.<commits on this branch>: always higher for a later build, as MSI upgrades need.
$count = [int](git -C $repo rev-list --count HEAD)
$version = "0.1.$count"

cmd /c "`"$repo\app\build.bat`" release"
if ($LASTEXITCODE) { throw 'build failed' }

$build = "$repo\app\build"
$out = "$repo\dist"
$obj = "$out\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
& "$wix\candle.exe" -nologo "-dVersion=$version" "-dBuild=$build" "-dWorker=$repo\worker" "-dIconFile=$repo\assets\lifelog.ico" `
    -out "$obj\" "$repo\installer\lifelog.wxs"
if ($LASTEXITCODE) { throw 'candle failed' }
& "$wix\light.exe" -nologo -spdb -sice:ICE91 -sice:ICE60 -out "$out\lifelog-$version.msi" "$obj\lifelog.wixobj"
if ($LASTEXITCODE) { throw 'light failed' }
Remove-Item -Recurse -Force $obj
"built dist\lifelog-$version.msi"
