# Ornament Weather Provider

The bridge reads weather on the PC side and exposes a normalized `weather` object to the ESP32 over `/state`.

Default location:

```text
Latitude:  39.99540087499999
Longitude: 116.34162524999999
Label:     HAIDIAN
```

## Provider Order

Set `CODEX_ORNAMENT_WEATHER_PROVIDER` to one of:

```text
auto
openmeteo
caiyun
qweather
```

`auto` is the default. In `auto` mode:

1. Use Caiyun when `CODEX_ORNAMENT_CAIYUN_TOKEN` is set.
2. Use QWeather when both `CODEX_ORNAMENT_QWEATHER_HOST` and `CODEX_ORNAMENT_QWEATHER_TOKEN` are set.
3. Fall back to Open-Meteo.

## Caiyun

Caiyun v2.6 authenticates with the token in the URL path. The bridge calls the realtime endpoint:

```text
https://api.caiyunapp.com/v2.6/{token}/{longitude},{latitude}/realtime
```

Configure:

```powershell
$env:CODEX_ORNAMENT_WEATHER_PROVIDER = "caiyun"
$env:CODEX_ORNAMENT_CAIYUN_TOKEN = "replace-with-your-token"
```

The bridge keeps the token out of committed files and builds the URL at runtime.

## Open-Meteo Fallback

Open-Meteo does not need a key. It is kept as the default/fallback provider:

```text
https://api.open-meteo.com/v1/forecast?latitude=...&longitude=...&current=temperature_2m,weather_code,wind_speed_10m&timezone=auto
```

## Location Overrides

```powershell
$env:CODEX_ORNAMENT_WEATHER_LAT = "39.99540087499999"
$env:CODEX_ORNAMENT_WEATHER_LON = "116.34162524999999"
$env:CODEX_ORNAMENT_WEATHER_LABEL = "HAIDIAN"
```
