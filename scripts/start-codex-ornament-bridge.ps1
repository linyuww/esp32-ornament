$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$exe = Join-Path $root "target\debug\codex-ornament-bridge.exe"
$stdout = Join-Path $root "bridge-stdout.log"
$stderr = Join-Path $root "bridge-stderr.log"

if (Get-Process -Name "codex-ornament-bridge" -ErrorAction SilentlyContinue) {
    return
}

if (-not (Test-Path -LiteralPath $exe)) {
    Push-Location $root
    try {
        cargo build -p codex-ornament-bridge
    } finally {
        Pop-Location
    }
}

Start-Process `
    -FilePath $exe `
    -WorkingDirectory $root `
    -WindowStyle Hidden `
    -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr

