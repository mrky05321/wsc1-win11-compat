param([string]$BackendArchive = '', [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
    & cmake -S . -B build -G 'Visual Studio 17 2022' -A Win32
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
    & cmake --build build --config Release --target wsc1_ddraw
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
    if (-not $BuildOnly) {
        if ($BackendArchive) { & python package.py --backend-archive $BackendArchive }
        else { & python package.py }
        if ($LASTEXITCODE -ne 0) { throw 'Packaging failed' }
    }
} finally { Pop-Location }
