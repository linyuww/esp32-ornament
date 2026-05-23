# Hardware and Software Interface

## Current Hardware

- ESP32-S3-N16R8 dev board, ESP32-S3 module with 16 MB flash and 8 MB PSRAM.
- 1.5 inch round ST77916 TFT screen, 360x360, QSPI, 16P FPC.
- Breadboard.

## Purchased Screen Pinout

The purchased 1.5 inch round screen pin table:

| FPC pin | Symbol | Function |
| --- | --- | --- |
| 1 | K | Backlight cathode |
| 2 | A | Backlight anode |
| 3 | GND | Power ground |
| 4 | CS | Chip select |
| 5 | SCL | QSPI clock |
| 6 | RESET | Display reset |
| 7 | IO3 | QSPI data 3 |
| 8 | IO2 | QSPI data 2 |
| 9 | IO1 | QSPI data 1 |
| 10 | IO0 | QSPI data 0 |
| 11 | TE | Frame sync, optional |
| 12 | VCC | Panel power |
| 13 | IOVCC | IO power |
| 14-16 | GND | Power ground |

## Recommended QSPI Mapping

| Screen | ESP32-S3 |
| --- | --- |
| VCC | 3V3 |
| IOVCC | 3V3 |
| GND | GND |
| SCL | GPIO12 |
| CS | GPIO10 |
| RESET | GPIO8 |
| IO0 | GPIO11 |
| IO1 | GPIO13 |
| IO2 | GPIO14 |
| IO3 | GPIO15 |
| TE | Not connected initially |
| A | 3V3 through proper backlight current limiting if breakout does not provide it |
| K | GPIO7 through MOSFET or LED driver, or GND if always on and current limited |

Do not drive the backlight directly from a GPIO unless the panel breakout has current limiting and
the measured current is safe for the selected GPIO path.

## Firmware Boundary

`display.c` is the only module that should know the screen bus, pins, pixel format, and rotation.
Application logic should only pass `ornament_state_t` into `display_render_state`.

## Implementation Reference

The display driver path uses Espressif's public `esp_lcd_st77916` component rather than a handwritten
ST77916 command table:

```text
https://components.espressif.com/components/espressif/esp_lcd_st77916
https://github.com/espressif/esp-iot-solution/tree/master/components/display/lcd/esp_lcd_st77916
```
