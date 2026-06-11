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

function Test-TruthyEnv($Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $false
    }
    return $Value -match '^(1|true|yes|on)$'
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

function Get-OrnamentBridgePort {
    if ($env:CODEX_ORNAMENT_BIND -and $env:CODEX_ORNAMENT_BIND -match ':(\d+)$') {
        return $Matches[1]
    }
    return "8787"
}

function Get-RunningBridgeProcesses {
    return @(Get-Process -Name "codex-ornament-bridge" -ErrorAction SilentlyContinue)
}

function Get-BridgeDiscoverIp($Port) {
    try {
        $response = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/discover" -TimeoutSec 2 -ErrorAction Stop
        return $response.localIp
    } catch {
        return $null
    }
}

function Test-BridgeReady($Port) {
    try {
        $response = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/ready" -TimeoutSec 2 -ErrorAction Stop
        return [bool]$response.ok
    } catch {
        return $false
    }
}

function Get-NewestBridgeSourceWriteTime {
    $paths = @(
        (Join-Path $root "Cargo.toml"),
        (Join-Path $root "Cargo.lock"),
        (Join-Path $root "crates\codex-ornament-bridge")
    )
    $items = foreach ($path in $paths) {
        if (Test-Path -LiteralPath $path) {
            Get-ChildItem -LiteralPath $path -Recurse -File
        }
    }
    if (-not $items) {
        return [datetime]::MinValue
    }
    return ($items | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
}

function Test-BridgeExecutableStale {
    if (-not (Test-Path -LiteralPath $exe)) {
        return $true
    }

    $exeTime = (Get-Item -LiteralPath $exe).LastWriteTimeUtc
    return (Get-NewestBridgeSourceWriteTime) -gt $exeTime
}

function Build-OrnamentBridge {
    Push-Location $root
    try {
        cargo build -p codex-ornament-bridge
    } finally {
        Pop-Location
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
$lockLanIp = Test-TruthyEnv $env:CODEX_ORNAMENT_LOCK_LAN_IP
$lockMusicBaseUrl = Test-TruthyEnv $env:CODEX_ORNAMENT_LOCK_MUSIC_PUBLIC_BASE_URL
$detectedLanIp = Find-OrnamentLanAddress
if ((Test-UsableLanAddress $detectedLanIp) -and (-not $lockLanIp)) {
    $env:CODEX_ORNAMENT_LAN_IP = $detectedLanIp
} elseif (-not (Test-UsableLanAddress $env:CODEX_ORNAMENT_LAN_IP) -and (Test-UsableLanAddress $detectedLanIp)) {
    $env:CODEX_ORNAMENT_LAN_IP = $detectedLanIp
}

if ((Test-UsableLanAddress $env:CODEX_ORNAMENT_LAN_IP) -and (-not $lockMusicBaseUrl)) {
    $port = Get-OrnamentBridgePort
    $env:CODEX_ORNAMENT_MUSIC_PUBLIC_BASE_URL = "http://$($env:CODEX_ORNAMENT_LAN_IP):$port"
}

$bridgeProcesses = Get-RunningBridgeProcesses
if ($bridgeProcesses.Count -gt 0) {
    $port = Get-OrnamentBridgePort
    $advertisedIp = Get-BridgeDiscoverIp $port
    $bridgeReady = Test-BridgeReady $port
    $bridgeStale = Test-BridgeExecutableStale
    if (
        (-not $bridgeStale) -and
        $bridgeReady -and
        (Test-UsableLanAddress $env:CODEX_ORNAMENT_LAN_IP) -and
        $advertisedIp -eq $env:CODEX_ORNAMENT_LAN_IP
    ) {
        return
    }

    $bridgeProcesses | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

if (Test-BridgeExecutableStale) {
    Build-OrnamentBridge
}

Start-Process `
    -FilePath $exe `
    -WorkingDirectory $root `
    -WindowStyle Hidden `
    -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr

$port = Get-OrnamentBridgePort
for ($attempt = 0; $attempt -lt 20; $attempt++) {
    if (Test-BridgeReady $port) {
        return
    }
    Start-Sleep -Milliseconds 500
}

throw "codex-ornament-bridge did not become ready on port $port; see $stderr"
