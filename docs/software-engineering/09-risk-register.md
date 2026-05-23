# Risk Register

| ID | Risk | Impact | Mitigation |
| --- | --- | --- | --- |
| R-01 | Screen is QSPI but wired as SPI | Black screen | Confirm seller pinout and driver before soldering |
| R-02 | Backlight lacks current limiting | Hardware damage | Measure module circuit or add external limit/MOS control |
| R-03 | ChatGPT usage endpoint changes | Quota unavailable | Keep `quota-core` parser tolerant and expose clear error status |
| R-04 | Hook script blocks Codex | Bad user experience | Use short timeout and swallow bridge errors |
| R-05 | LAN reads display sensitive task text | Privacy leak | Clip, redact, or disable task message forwarding |
| R-06 | Windows firewall blocks ESP32 | ESP32 offline | Document firewall rule and health endpoint |
| R-07 | ESP32 heap fragmentation | Firmware instability | Fixed response buffer and small JSON payload |
| R-08 | PC IP changes | ESP32 cannot poll bridge | Use DHCP reservation or mDNS in a later version |
