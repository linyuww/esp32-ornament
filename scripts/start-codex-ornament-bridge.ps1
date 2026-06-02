$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$exe = Join-Path $root "target\debug\codex-ornament-bridge.exe"
$stdout = Join-Path $root "bridge-stdout.log"
$stderr = Join-Path $root "bridge-stderr.log"
$envFile = Join-Path $root ".env"
$localEnv = Join-Path $root ".env.local"

function Normalize-ProcessPathEnvironment {
    $pathValue = [Environment]::GetEnvironmentVariable("Path", "Process")
    if ([string]::IsNullOrEmpty($pathValue)) {
        $pathValue = [Environment]::GetEnvironmentVariable("PATH", "Process")
    }
    [Environment]::SetEnvironmentVariable("PATH", $null, "Process")
    if (-not [string]::IsNullOrEmpty($pathValue)) {
        [Environment]::SetEnvironmentVariable("Path", $pathValue, "Process")
    }
}

Normalize-ProcessPathEnvironment

function Test-UsableLanAddress($Address) {
    if ([string]::IsNullOrWhiteSpace($Address)) {
        return $false
    }
    return $Address -notmatch '^(127\.|169\.254\.|198\.(18|19)\.)'
}

function Find-OrnamentLanAddress {
    try {
        $addresses = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
            Where-Object {
                $_.AddressState -eq 'Preferred' -and
                (Test-UsableLanAddress $_.IPAddress) -and
                $_.InterfaceAlias -notmatch 'Meta|Clash|Nyanpasu|VMware|Virtual|Hyper-V|Loopback|Bluetooth|Docker|WSL'
            }
        $preferred = $addresses |
            Where-Object { $_.InterfaceAlias -match 'WLAN|Wi-?Fi|Ethernet' } |
            Sort-Object InterfaceMetric |
            Select-Object -First 1
        if ($preferred) {
            return $preferred.IPAddress
        }
        return $addresses | Sort-Object InterfaceMetric | Select-Object -First 1 -ExpandProperty IPAddress
    } catch {
        return $null
    }
}

foreach ($file in @($envFile, $localEnv)) {
    if (Test-Path -LiteralPath $file) {
        Get-Content -LiteralPath $file | ForEach-Object {
            $line = $_.Trim()
            if ($line -eq "" -or $line.StartsWith("#") -or -not $line.Contains("=")) {
                return
            }
            $name, $value = $line.Split("=", 2)
            $name = $name.Trim()
            $value = $value.Trim().Trim('"').Trim("'")
            if ($name -ne "" -and -not [Environment]::GetEnvironmentVariable($name, "Process")) {
                [Environment]::SetEnvironmentVariable($name, $value, "Process")
            }
        }
    }
}

$env:CODEX_ORNAMENT_WEATHER_PROVIDER = if ($env:CODEX_ORNAMENT_WEATHER_PROVIDER) { $env:CODEX_ORNAMENT_WEATHER_PROVIDER } else { "auto" }
$env:CODEX_ORNAMENT_WEATHER_LAT = if ($env:CODEX_ORNAMENT_WEATHER_LAT) { $env:CODEX_ORNAMENT_WEATHER_LAT } else { "39.99540087499999" }
$env:CODEX_ORNAMENT_WEATHER_LON = if ($env:CODEX_ORNAMENT_WEATHER_LON) { $env:CODEX_ORNAMENT_WEATHER_LON } else { "116.34162524999999" }
$env:CODEX_ORNAMENT_WEATHER_LABEL = if ($env:CODEX_ORNAMENT_WEATHER_LABEL) { $env:CODEX_ORNAMENT_WEATHER_LABEL } else { "HAIDIAN" }
if (-not (Test-UsableLanAddress $env:CODEX_ORNAMENT_LAN_IP)) {
    $lanIp = Find-OrnamentLanAddress
    if (Test-UsableLanAddress $lanIp) {
        $env:CODEX_ORNAMENT_LAN_IP = $lanIp
    }
}

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
