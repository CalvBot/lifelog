# Sets up lifelog's own summary AI in %LOCALAPPDATA%\lifelog\llm: llama.cpp's server (CUDA build)
# and the Qwen3 14B model. Safe to run again; it only fetches what is missing.
#   scripts\get-llm.ps1
# The model is taken from Ollama's store if it has qwen3:14b (hard link, no extra space or download);
# otherwise it is downloaded (9 GB).
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar

$build = 'b11873'   # llama.cpp release; bump to update
$model = 'qwen3-14b'
$modelUrl = 'https://huggingface.co/Qwen/Qwen3-14B-GGUF/resolve/main/Qwen3-14B-Q4_K_M.gguf'

$dir = Join-Path $env:LOCALAPPDATA 'lifelog\llm'
New-Item -ItemType Directory -Force $dir | Out-Null
$stamp = Join-Path $dir 'build.txt'

# 1. The server.
if (-not (Test-Path (Join-Path $dir 'llama-server.exe')) -or ((Get-Content $stamp -ErrorAction SilentlyContinue) -ne $build)) {
    $base = "https://github.com/ggml-org/llama.cpp/releases/download/$build"
    $tmp = Join-Path $env:TEMP "lifelog-llm-$build"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    try {
        foreach ($zip in "llama-$build-bin-win-cuda-12.4-x64.zip", 'cudart-llama-bin-win-cuda-12.4-x64.zip') {
            "Downloading $zip"
            $file = Join-Path $tmp $zip
            Invoke-WebRequest "$base/$zip" -OutFile $file
            Expand-Archive $file -DestinationPath $dir -Force
        }
    } finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
    # Some builds put the files in a subfolder.
    $exe = Get-ChildItem $dir -Recurse -Filter llama-server.exe | Select-Object -First 1
    if ($exe.DirectoryName -ne $dir) { Get-ChildItem $exe.DirectoryName | Move-Item -Destination $dir -Force }
    Set-Content $stamp $build
}

# 2. The model.
$gguf = Join-Path $dir "$model.gguf"
if (-not (Test-Path $gguf)) {
    $manifest = Join-Path $env:USERPROFILE '.ollama\models\manifests\registry.ollama.ai\library\qwen3\14b'
    $blob = $null
    if (Test-Path $manifest) {
        $digest = ((Get-Content $manifest -Raw | ConvertFrom-Json).layers | Where-Object mediaType -eq 'application/vnd.ollama.image.model').digest
        $blob = Join-Path $env:USERPROFILE ('.ollama\models\blobs\' + ($digest -replace ':', '-'))
    }
    if ($blob -and (Test-Path $blob)) {
        'Using the model Ollama already has'
        try { New-Item -ItemType HardLink -Path $gguf -Target $blob | Out-Null } catch { Copy-Item $blob $gguf }
    } else {
        'Downloading the model (9 GB)'
        Invoke-WebRequest $modelUrl -OutFile "$gguf.part"
        Move-Item "$gguf.part" $gguf
    }
}
"Summary AI ready in $dir"
