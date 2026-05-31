$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$hooks = Join-Path $root ".git\hooks"
$hook = Join-Path $hooks "pre-commit"

if (-not (Test-Path -LiteralPath $hooks)) {
    New-Item -ItemType Directory -Path $hooks | Out-Null
}

@'
#!/bin/sh
set -eu

diff="$(git diff --cached -- . ':(exclude).env.example' ':(exclude)scripts/install-secret-guard.ps1')"

if printf '%s\n' "$diff" | grep -E 'api\.caiyunapp\.com/v2\.6/u[A-Za-z0-9]{8,}/[0-9.-]+,[0-9.-]+/(realtime|weather|hourly|daily)' >/dev/null; then
  echo "pre-commit: possible Caiyun token embedded in URL path." >&2
  exit 1
fi

if printf '%s\n' "$diff" | awk '/CODEX_ORNAMENT_CAIYUN_(TOKEN|KEY)[[:space:]]*=[[:space:]]*[^<[:space:]#][^[:space:]]+/ && $0 !~ /replace-with-your-token/ { found=1 } END { exit found ? 0 : 1 }'; then
  echo "pre-commit: possible Caiyun token assigned in tracked content." >&2
  exit 1
fi

if printf '%s\n' "$diff" | grep -E 'u[A-Za-z0-9]{8,}x[A-Za-z0-9]{4,}' >/dev/null; then
  echo "pre-commit: possible Caiyun-style token detected." >&2
  exit 1
fi
'@ | Set-Content -LiteralPath $hook -Encoding ascii

Write-Host "Installed secret guard: $hook"
