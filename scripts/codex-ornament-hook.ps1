param(
  [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
  [string[]]$PayloadArgs,
  [string]$EventName = $env:CODEX_HOOK_EVENT_NAME,
  [string]$Endpoint = $env:CODEX_ORNAMENT_ENDPOINT,
  [string]$Token = $env:CODEX_ORNAMENT_TOKEN,
  [switch]$DryRun
)

if ([string]::IsNullOrWhiteSpace($Endpoint)) {
  $Endpoint = "http://127.0.0.1:8787/hook/codex"
}

function Test-JsonObject($Text) {
  if ([string]::IsNullOrWhiteSpace($Text)) {
    return $null
  }
  try {
    $Parsed = ConvertFrom-Json -InputObject $Text -ErrorAction Stop
    if ($Parsed -is [string]) {
      return Test-JsonObject $Parsed
    }
    return $Parsed
  } catch {
    return $null
  }
}

function Add-MissingEventName($Object, $FallbackEventName) {
  if ([string]::IsNullOrWhiteSpace($FallbackEventName)) {
    return $Object
  }
  if ($null -eq $Object.PSObject.Properties["hook_event_name"] -and
      $null -eq $Object.PSObject.Properties["type"]) {
    $Object | Add-Member -NotePropertyName "hook_event_name" -NotePropertyValue $FallbackEventName
  }
  return $Object
}

function Add-MissingTextProperty($Object, $Name, $Value) {
  if ([string]::IsNullOrWhiteSpace($Value)) {
    return $Object
  }
  if ($null -eq $Object.PSObject.Properties[$Name]) {
    $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
  }
  return $Object
}

function Add-HookContext($Object) {
  $Object = Add-MissingTextProperty $Object "session_id" $env:CODEX_THREAD_ID
  $Object = Add-MissingTextProperty $Object "cwd" (Get-Location).Path
  return $Object
}

function Find-EventName($Text, $FallbackEventName) {
  if (-not [string]::IsNullOrWhiteSpace($FallbackEventName)) {
    return $FallbackEventName
  }
  if ($Text -match "UserPromptSubmit") {
    return "UserPromptSubmit"
  }
  if ($Text -match "\bStop\b") {
    return "Stop"
  }
  if ($Text -match "agent-turn-complete") {
    return "agent-turn-complete"
  }
  return "codex-event"
}

function New-FallbackPayload($RawPayload, $FallbackEventName) {
  $Kind = Find-EventName $RawPayload $FallbackEventName
  $Message = if ([string]::IsNullOrWhiteSpace($RawPayload)) { $Kind } else { $RawPayload.Trim() }
  if ($Message.Length -gt 160) {
    $Message = $Message.Substring(0, 160) + "..."
  }
  $Payload = [pscustomobject]@{
    hook_event_name = $Kind
    message = $Message
  }
  $Payload = Add-HookContext $Payload
  return $Payload | ConvertTo-Json -Compress
}

function ConvertTo-HookPayloadJson($RawPayload, $FallbackEventName) {
  $Payload = if ($null -eq $RawPayload) { "" } else { $RawPayload.Trim() }

  $Candidates = [System.Collections.Generic.List[string]]::new()
  $Candidates.Add($Payload)

  if ($Payload.StartsWith("'") -and $Payload.EndsWith("'") -and $Payload.Length -ge 2) {
    $Candidates.Add($Payload.Substring(1, $Payload.Length - 2))
  }
  if ($Payload.StartsWith('"') -and $Payload.EndsWith('"') -and $Payload.Length -ge 2) {
    $Candidates.Add($Payload.Substring(1, $Payload.Length - 2))
  }
  $Candidates.Add(($Payload -replace '\\"', '"'))
  $Candidates.Add(($Payload -replace '""', '"'))

  foreach ($Candidate in $Candidates) {
    $Parsed = Test-JsonObject $Candidate
    if ($null -ne $Parsed) {
      $Parsed = Add-MissingEventName $Parsed $FallbackEventName
      $Parsed = Add-HookContext $Parsed
      return $Parsed | ConvertTo-Json -Compress -Depth 8
    }
  }

  return New-FallbackPayload $Payload $FallbackEventName
}

$IsNotifyPayload = $PayloadArgs.Count -gt 0 -and -not [string]::IsNullOrWhiteSpace($PayloadArgs[0])
if ($IsNotifyPayload) {
  $RawPayload = $PayloadArgs -join " "
} else {
  $RawPayload = [Console]::In.ReadToEnd()
}

$Payload = ConvertTo-HookPayloadJson $RawPayload $EventName

try {
  $Headers = @{}
  if (-not [string]::IsNullOrWhiteSpace($Token)) {
    $Headers["X-Codex-Ornament-Token"] = $Token
  }
  if ($DryRun) {
    Write-Output $Payload
  } else {
    Invoke-RestMethod `
      -Method Post `
      -Uri $Endpoint `
      -ContentType "application/json" `
      -Headers $Headers `
      -Body $Payload `
      -TimeoutSec 2 | Out-Null
  }
} catch {
}

if ((-not $IsNotifyPayload) -and (-not $DryRun)) {
  Write-Output '{"continue":true}'
}
