# CrowPanel-Flock-You — Developer Documentation

Technical documentation for building, modifying, and extending the firmware. Same code base runs on the **Elecrow CrowPanel ESP32-S3 Terminal 3.5"** (`env:crowpanel`) and the **Cheap Yellow Display ESP32-2432S028R** (`env:cyd`); `env:xiao_esp32s3` is the headless S3 build.

---

## Board abstraction (port structure)

Per the port design (`docs/port/bob-port-design.md`, ADR-2):

- `board_config.h` is the **only** place board pins/dimensions/touch type live. It defines `FY_UI_BUILD` (1 for CYD and CrowPanel, 0 headless), `FY_TOUCH_XPT2046_BITBANG` (CYD) or `FY_TOUCH_FT6236_I2C` (CrowPanel), plus per-board pin maps.
- Shared feature code (display/GPS/SD/BLE-UART state, `dualPrintf` BLE mirroring, detection record hooks, loop ticks) is gated by `FY_UI_BUILD`, not by a board name.
- Touch is reached only through `boardTouchInit()` / `boardTouchPressed()` / `boardTouchDebug()` — callers never use coordinates, so no calibration is needed.
- TFT_eSPI stays flag-driven (no `User_Setup.h`); pins/driver come from `platformio.ini` build flags per env.
- The BLE advertisement name (`CYD-Flock-You`) and protocol version (1) are **wire constants** — identical on every board, required by the shipped `deflock-app`.

## Architecture Overview

The firmware runs on an ESP32 or ESP32-S3 and performs passive WiFi and BLE monitoring for Flock Safety camera signatures. It communicates with the FlockFree Navigation Android app over Bluetooth LE.

### Core Modules

```
main.cpp                    # Entry point, WiFi/BLE scanning, detection logic, display,
                            # board-specific touch/display/SD glue (behind board_config.h)
board_config.h              # Per-board pin maps, dimensions, touch controller
platformio.ini              # Board envs: cyd / crowpanel / xiao_esp32s3
partitions_cyd.csv           # 4MB single-OTA partition table
partitions_crowpanel.csv    # 8MB dual-OTA partition table
```

### Detection Pipeline

```
WiFi promiscuous mode
    ↓
Frame parsed (probe/beacon)
    ↓
OUI match against known Flock list
    ↓
SSID keyword check (flock, flck, test_flck)
    ↓
Detection event created
    ↓
├── SD card CSV log
├── BLE JSON to phone
├── TFT display update
├── Buzzer chirp
└── Full-screen red FLOCK FOUND flash
```

### BLE Detection Pipeline

```
BLE active scan (continuous)
    ↓
Advertise payload parsed
    ↓
Manufacturer data match (0x09c8 XUNTONG)
    ↓
Device name pattern match (Penguin-##########)
    ↓
Battery level extraction (FS Ext Battery, 10-digit names)
    ↓
Detection event via existing JSON path
```

## Build Environment

### PlatformIO

The project uses PlatformIO for building. Configuration is in `platformio.ini`:

| Env | Board | Display | Notes |
|-----|-------|---------|-------|
| `env:cyd` | `esp32dev` (ESP32-2432S028R) | ILI9341 240×320, HSPI | Original board |
| `env:crowpanel` | `esp32-s3-devkitc-1` (DLC35020S) | ILI9488 320×480, FSPI | New port; 8MB dual-OTA, qio_opi PSRAM |
| `env:xiao_esp32s3` | `seeed_xiao_esp32s3` | none | Headless Wi-Fi-only build |

### Build

```bash
pio run -e crowpanel   # CrowPanel ESP32-S3 Terminal 3.5"
pio run -e cyd          # Cheap Yellow Display
pio run -e xiao_esp32s3  # headless S3
# or all at once:
pio run -e crowpanel -e cyd -e xiao_esp32s3
```

### Flash

Connect the board via USB. CrowPanel uses the ESP32-S3's native USB (hold **BOOT**, tap **RST** if needed for download mode):

```bash
pio run -e crowpanel -t upload   # CrowPanel
pio run -e cyd -t upload         # CYD
```

### Serial Monitor

```bash
pio device monitor -e crowpanel
pio device monitor -e cyd
```

### Build Artifacts

| File | Location | Purpose |
|------|----------|---------|
| `firmware.bin` | `.pio/build/<env>/firmware.bin` | Flashable binary (use with `esptool.py`) |
| `firmware.elf` | `.pio/build/<env>/firmware.elf` | ELF with debug symbols |
| `firmware.map` | `.pio/build/<env>/firmware.map` | Memory map |

### Flash with esptool (alternative)

```bash
# CYD: classic ESP32 UART bootloader, offset 0x10000
esptool.py --port /dev/ttyUSB0 write_flash 0x10000 firmware.bin

# CrowPanel: S3 native USB; flash app at 0x10000 (ota_0 slot)
esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x10000 firmware.bin
```

## Hardware Reference

### Elecrow CrowPanel ESP32-S3 Terminal 3.5" (DLC35020S) Pin Map

| Pin | Function |
|-----|----------|
| GPIO 13 | LCD + SD MOSI (shared bus) |
| GPIO 14 | LCD + SD MISO |
| GPIO 12 | LCD + SD SCLK |
| GPIO 3 | LCD CS |
| GPIO 42 | LCD D/C |
| GPIO 46 | LCD backlight (active HIGH) |
| GPIO 10 | SD card CS |
| GPIO 2 / 1 | FT6236 touch I2C SDA / SCL (0x38) |
| GPIO 45 | Piezo buzzer |
| GPIO 7/2/1/… | OV2640 camera (deferred, not configured in v1) |

- TFT driver: ILI9488 (SPI, `SPI_18BIT_DRIVER` path in TFT_eSPI), 320×480 native, portrait default (rotation 0)
- SPI: shared LCD/SD bus, 40MHz write / 16MHz read; SD is CS-gated at 4MHz (Elecrow's demo proves the bus stable at 60MHz)
- Touch: FT6236 capacitive, Wire poll, no INT pin; coordinates debug-only
- Memory: 8MB flash (`partitions_crowpanel.csv`, dual 3MB OTA + 1.9MB SPIFFS + coredump), `qio_opi` PSRAM
- If the physical module turns out not to be R8 (octal PSRAM), switch `board_build.arduino.memory_type` to `qio_qspi`

### ESP32-2432S028R Pin Map

| Pin | Function |
|-----|----------|
| GPIO 4 | Red RGB LED (detection feedback, active low) |
| GPIO 5 | SD card CS |
| GPIO 12 | TFT MISO |
| GPIO 13 | TFT MOSI |
| GPIO 14 | TFT SCLK |
| GPIO 15 | TFT CS |
| GPIO 2 | TFT DC |
| GPIO 21 | TFT backlight |
| GPIO 26 | Piezo buzzer |
| GPIO 0 | Boot button / display control |
| GPIO 25 | XPT2046 touch CLK |
| GPIO 32 | XPT2046 touch MOSI |
| GPIO 39 | XPT2046 touch MISO |
| GPIO 33 | XPT2046 touch CS |
| GPIO 36 | XPT2046 touch IRQ |

### TFT Configuration

- Driver: ILI9341_2_DRIVER
- Dimensions: 240×320 (native), rotated to 320×240 landscape at runtime
- Rotation: `CYD_TFT_ROTATION=1`
- SPI bus: HSPI
- Touch controller: XPT2046 on separate SPI bus

### Memory Usage (v1.3.0)

- Flash: 51.6% used (1,590,173 / 3,080,192 bytes)
- RAM: 24.3% used (79,656 / 327,680 bytes)

## BLE Protocol

See [docs/deflock-pairing-protocol.md](docs/deflock-pairing-protocol.md) for the full protocol specification.

### Quick Reference

| Parameter | Value |
|-----------|-------|
| Peripheral name | `CYD-Flock-You` |
| Service UUID | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (write) UUID | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (notify) UUID | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |
| Line format | Newline-delimited text |
| Protocol version | 1 |

### Serial Commands

| Command | Description |
|---------|-------------|
| `FYHELLO` | Returns pairing/status JSON |
| `FYSTATUS` | Returns full telemetry JSON |
| `FYGPS,lat,lon,acc,speed,course,sats,hdop,unix_time,offset` | Phone GPS input |
| `FYSIM` | Simulate a detection for testing |
| `FYSCREEN,next` | Cycle to next display screen |
| `FYTOUCH` | Report raw touch diagnostic values |

## Detection Methods

### WiFi Detection

| Method | Description |
|--------|-------------|
| `wifi_wildcard_probe` | Probe request with wildcard SSID from a known OUI |
| `wifi_oui_addr2` | Transmitter address OUI match |
| `wifi_oui_addr1` | Receiver-side OUI match (quiet/sleeping infrastructure) |
| `wifi_oui_addr3` | BSSID OUI match (disabled by default) |
| `wifi_ssid` | SSID keyword match (flock, flck, test_flck) |
| `wifi_hidden_ssid` | Hidden beacon/probe-response from a known OUI |

### BLE Detection

| Method | Description |
|--------|-------------|
| Penguin name match | Device name matches `Penguin-` + 10 digits |
| Manufacturer data | XUNTONG manufacturer ID `0x09c8` in advertising data |
| FS Ext Battery | Battery-level devices with `FS Ext Battery` name pattern |
| 10-digit battery | 10-digit numeric device names from known OUIs |

### OUI List

Target OUI list: [`datasets/NitekryDPaul_wifi_ouis.md`](datasets/NitekryDPaul_wifi_ouis.md)

## Display System

### Screens

1. **Scan screen** — FF badge, current channel, hit count, GPS/SD/BLE status, local time
2. **GPS screen** — Latest GPS fix details (from phone)
3. **CSV Log screen** — SD card logging status and row count
4. **Last Detection screen** — Most recent detection details

### Controls

| Control | Action |
|---------|--------|
| Touchscreen tap | Cycle to next display screen |
| Boot button press (GPIO 0) | Rotate screen orientation |
| Touchscreen tap (during FLOCK FOUND) | Dismiss the alert flash |

### FLOCK FOUND Alert

On detection, the display flashes a full-screen red **FLOCK FOUND** alert. Tap the touchscreen to dismiss.

## CSV Log Format

When SD is available, detections append to `/flock.csv`:

```
millis,mac,oui,method,rssi,channel,frequency_mhz,lat,lon,accuracy_m,gps_age_ms,speed_kmph,course_deg,count
```

## Companion App Integration

### FlockFree Navigation (OsmAnd fork)

- Repo: [yetisoldier/FlockFree-Navigation](https://github.com/yetisoldier/FlockFree-Navigation)
- Connects over BLE, streams GPS, receives detection events
- CYD auto-pauses when FlockFree detects Flock WiFi beacon
- Detection markers appear on map for manual review

### DeFlock (Flutter app, CYD branch)

- Repo: [yetisoldier/deflock-app](https://github.com/yetisoldier/deflock-app), branch `cyd-flock-you-integration`
- Connects over BLE using `flutter_blue_plus`
- Same Nordic UART protocol
- Detection review flow with OSM upload

## Extending the Firmware

### Adding a New Detection Method

1. Add the method name to the detection method enum/strings in `main.cpp`
2. Implement the detection logic in the appropriate scan callback
3. Emit a detection JSON via `ble_uart_send_detection()`
4. Log to SD via `sd_log_append()`
5. Update the TFT display and trigger buzzer/LED/flash

### Adding a New Display Screen

1. Add the screen to the screen enum in `display.h`
2. Implement the render function in `display.cpp`
3. Add it to the screen cycle in the touch handler
4. Update the `FYSCREEN,next` command to include it

### Adding a New BLE Command

1. Parse the command in `ble_uart.cpp` command handler
2. Implement the response logic
3. Document the command in the serial commands table and README

## Credits and Upstream

- Firmware fork: [colonelpanichacks/flock-you](https://github.com/colonelpanichacks/flock-you)
- Wi-Fi OUI research: ØяĐöØцяöЪöяцฐ / @NitekryDPaul
- BLE manufacturer ID work: [wgreenberg/flock-you](https://github.com/wgreenberg/flock-you)
- Wildcard probe signature: [DeflockJoplin/flock-you](https://github.com/DeflockJoplin/flock-you)

## Disclaimer

This is a passive research and privacy-auditing tool. It does not transmit Wi-Fi, authenticate to networks, or bypass access controls. Wireless reception and public infrastructure mapping laws vary by jurisdiction. Use responsibly.