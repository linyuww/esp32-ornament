param(
  [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
  [string[]]$PayloadArgs,
  [string]$Endpoint = $env:CODEX_ORNAMENT_ENDPOINT,
  [string]$Token = $env:CODEX_ORNAMENT_TOKEN
)

if ([string]::IsNullOrWhiteSpace($Endpoint)) {
  $Endpoint = "http://127.0.0.1:8787/hook/codex"
}

$IsNotifyPayload = $PayloadArgs.Count -gt 0 -and -not [string]::IsNullOrWhiteSpace($PayloadArgs[0])
if ($IsNotifyPayload) {
  $Payload = $PayloadArgs -join " "
} else {
  $Payload = [Console]::In.ReadToEnd()
}

if ([string]::IsNullOrWhiteSpace($Payload)) {
  $Payload = "{}"
}

$Payload = $Payload.Trim()
if ($Payload.StartsWith("'") -and $Payload.EndsWith("'")) {
  $Payload = $Payload.Substring(1, $Payload.Length - 2)
}

try {
  $Headers = @{}
  if (-not [string]::IsNullOrWhiteSpace($Token)) {
    $Headers["X-Codex-Ornament-Token"] = $Token
  }
  Invoke-RestMethod `
    -Method Post `
    -Uri $Endpoint `
    -ContentType "application/json" `
    -Headers $Headers `
    -Body $Payload `
    -TimeoutSec 2 | Out-Null
} catch {
}

if (-not $IsNotifyPayload) {
  Write-Output '{"continue":true}'
}
