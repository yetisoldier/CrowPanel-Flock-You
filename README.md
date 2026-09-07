# CrowPanel-Flock-You

Firmware for passive, human-reviewed Flock Safety / ALPR camera discovery, running on the **Elecrow CrowPanel ESP32-S3 Terminal 3.5"** (DLC35020S) and the original Cheap Yellow Display (CYD / ESP32-2432S028R). Pairs with [FlockFree Navigation](https://github.com/yetisoldier/FlockFree-Navigation) for the full mobile integration.

> ### ⚠️ Wire-name constraint (read this first)
> The BLE advertisement name is **`CYD-Flock-You`** and the protocol version is **1** on *every* board, including CrowPanel. This is not branding — the shipped `deflock-app` Android companion pairs on `device == "CYD-Flock-You" && protocol_version == 1`. The repo/product name is CrowPanel-Flock-You, but the on-wire identity is a compatibility contract with the shipped app and cannot change without a coordinated app update and a protocol bump.

This fork ports the CYD firmware ([yetisoldier/CYD-Flock-You](https://github.com/yetisoldier/CYD-Flock-You)) to the CrowPanel. Port design: `docs/port/bob-port-design.md` (Bob) · verified hardware facts: `docs/port-research/scout-hardware-report.md`.

## What It Does

The CYD (Cheap Yellow Display / ESP32-2432S028R) passively monitors 2.4 GHz Wi-Fi traffic for known Flock-style RF signatures. When it detects a likely ALPR camera, it:

1. Logs the detection to SD card (`/flock.csv`)
2. Sends a JSON detection event over Bluetooth LE to the paired phone
3. Updates the TFT display with hit details
4. Chirps the buzzer and flashes the LED
5. Flashes a full-screen red **FLOCK FOUND** alert (touch to dismiss)

The phone app (FlockFree Navigation) receives the detection, places a review marker on the map, and lets you manually verify and submit it to OpenStreetMap. **Nothing uploads automatically.**

The CYD also continuously scans for Flock-style BLE signatures, with a live status indicator on the scan screen.

## Repository Pair

| Repository | Role |
|------------|------|
| [`yetisoldier/CYD-Flock-You`](https://github.com/yetisoldier/CYD-Flock-You) | ESP32 firmware (this repo) |
| [`yetisoldier/FlockFree-Navigation`](https://github.com/yetisoldier/FlockFree-Navigation) | Android app (OsmAnd fork) |

## Hardware Requirements

- **Elecrow CrowPanel ESP32-S3 Terminal 3.5"** (DLC35020S) — ESP32-S3 with ILI9488 320×480 SPI TFT, FT6236 capacitive touch, OV2640 camera, SD on the shared LCD SPI bus — **or** **ESP32-2432S028R** (Cheap Yellow Display) — ESP32 with ILI9341 240×320 TFT
- **microSD card** (recommended, for CSV logging)
- **USB power bank** or stable USB power (the ESP32 is sensitive to power fluctuations)

### CrowPanel Pin Map (DLC35020S)

Verified against Elecrow's official demo repo ([Elecrow-RD/esp32-terminal](https://github.com/Elecrow-RD/esp32-terminal), `SPI/` tree):

| Pin | Function |
|-----|----------|
| GPIO 13 | LCD + SD MOSI (shared SPI bus) |
| GPIO 14 | LCD + SD MISO |
| GPIO 12 | LCD + SD SCLK |
| GPIO 3 | LCD CS |
| GPIO 42 | LCD D/C |
| GPIO 46 | LCD backlight (active HIGH) |
| GPIO 10 | SD CS (shares the LCD bus) |
| GPIO 2 / 1 | FT6236 touch I2C SDA / SCL (addr 0x38) |
| GPIO 45 | Piezo buzzer |

Camera (OV2640) and audio pins are recorded in the Scout report for future use but are **not configured in v1** — deferred per the port design (ADR-3).

### CYD Pin Map

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

## Installation

### Option A: Web Flasher (easiest — no software install)

This is the fastest path if you just want to flash a pre-built firmware binary without installing any toolchain.

#### 1. Download the firmware

Grab `flockyou-crowpanel-full-0x0.bin` (full image: bootloader + partition table + app) from the [latest release](https://github.com/yetisoldier/CrowPanel-Flock-You/releases). Piecewise files (`bootloader-0x0.bin`, `partitions-0x8000.bin`, `flockyou-crowpanel-app-0x10000.bin`) are attached too if you prefer separate offsets.

#### 2. Connect the board

The CrowPanel ESP32-S3 Terminal uses the S3's **native USB**, so there is no USB-to-UART bridge and no CP210x/CH340 driver to install. Plug it in and a serial port appears (Linux: `/dev/ttyACM0`, Windows: a COM port, macOS: `/dev/cu.usbmodem*`). If no port shows up: hold **BOOT**, tap **RST**, then replug.

#### 3. Flash with a browser-based tool

Open one of these in a Chromium-based browser (Chrome, Edge, Brave):

- **[esptool-js](https://espressif.github.io/esptool-js/)** (Espressif official)
- **[ESP Flasher](https://esp.huhn.me/)** (community tool)

Steps:

1. Click **Connect** and select the board's serial/COM port
2. Add `flockyou-crowpanel-full-0x0.bin` with upload address/offset **`0x0`** (the image already contains the bootloader and partition table at their correct offsets)
3. Click **Flash** / **Program**
4. Wait for the progress bar to finish; the board reboots automatically

Flashing the app-only file instead? Its offset is `0x10000`, and `bootloader-0x0.bin` + `partitions-0x8000.bin` must be flashed first.

#### 4. Verify

Open a serial monitor at 115200 baud (Arduino IDE Serial Monitor, `pio device monitor`, or `screen /dev/ttyACM0 115200`). Type `FYHELLO` and press Enter. You should see:

```json
{"event":"pair_status","device":"CYD-Flock-You","protocol_version":1,"features":["wifi_promisc","phone_gps","sd_csv","tft_status","ble_uart"],"gps":false,"sd":true,"detections":0,"csv_rows":0}
```

If the display lights up with the FlockFree scan screen, you're good to go.

---

### Option B: PlatformIO (build from source)

#### 1. Install PlatformIO

```bash
pip install platformio
```

Or via Homebrew (macOS):

```bash
brew install platformio
```

### 2. Clone and build

```bash
git clone https://github.com/yetisoldier/CrowPanel-Flock-You.git
cd CrowPanel-Flock-You
pio run -e crowpanel   # CrowPanel ESP32-S3 Terminal 3.5"
# or: pio run -e cyd    # original Cheap Yellow Display
```

### 3. Flash the firmware

Connect the board via USB to your computer. The CrowPanel uses the S3's native USB (hold **BOOT**, tap **RST** if it doesn't enter download mode automatically):

```bash
pio run -e crowpanel -t upload   # CrowPanel
# or: pio run -e cyd -t upload    # CYD (helper script: ./scripts/flash-cyd.sh)
```

### 4. Verify

Open the serial monitor:

```bash
pio device monitor -e crowpanel
```

Type `FYHELLO` and press Enter. You should see a JSON response:

```json
{"event":"pair_status","device":"CYD-Flock-You","protocol_version":1,"features":["wifi_promisc","phone_gps","sd_csv","tft_status","ble_uart"],"gps":false,"sd":true,"detections":0,"csv_rows":0}
```

Note the `device` name is `CYD-Flock-You` even on CrowPanel — see the wire-name constraint at the top of this README.

## Display

The CYD TFT shows a FlockFree-styled UI with navy/cyan/blue colors and danger accents:

- **Scan screen** — FF badge, current channel, hit count, GPS/SD/BLE status, local time
- **GPS screen** — Latest GPS fix details (from phone)
- **CSV Log screen** — SD card logging status and row count
- **Last Detection screen** — Most recent detection details (method, MAC, RSSI, channel)

### Display Controls

The touchscreen controls the display on both boards (the CYD additionally has the boot button for rotation):

| Control | Action |
|---------|--------|
| Touchscreen tap | Cycle to the next display screen / dismiss the FLOCK FOUND flash |
| Boot button press (CYD only, GPIO 0) | Rotate the screen orientation |

On the CrowPanel there is no spare button (GPIO0 is the BOOT strap), so rotation is not cycled from a physical control — screens are changed by touch or the `FYSCREEN,next` serial command.

The orientation cycles through landscape, portrait, landscape reversed, and portrait reversed. The firmware uses a portrait-aware layout, so the display content should stay inside the screen in both orientations.

The standard ESP32-2432S028R touch controller is an XPT2046 wired on a separate SPI bus from the TFT. This firmware reads it directly on GPIO 25/32/39/33 with IRQ on GPIO 36. On the CrowPanel, touch is an FT6236 capacitive controller at I2C address 0x38 (SDA=2, SCL=1), polled without an interrupt pin. On both boards the firmware only needs "screen tapped" — coordinates are debug-only.

## Usage

### Pairing with FlockFree Navigation

1. Flash the CYD firmware (above)
2. Insert a microSD card if you want local CSV logging
3. Power the CYD from a stable USB source
4. Install [FlockFree Navigation](https://github.com/yetisoldier/FlockFree-Navigation/releases) on your Android phone
5. Open FlockFree → Menu → Plugins → FlockFree → Settings
6. Enable **CYD BLE hardware**
7. FlockFree scans and connects to `CYD-Flock-You` automatically
8. The CYD display should show `BT OK` and `GPS OK` once the phone connects and sends a fresh GPS fix

### During a Drive

1. Keep the CYD powered and the phone app open (or backgrounded — the BLE foreground service keeps the connection alive)
2. The phone streams GPS to the CYD once per second via `FYGPS`
3. When the CYD detects a likely ALPR camera:
   - The display changes from `SCAN` to `HIT`
   - The buzzer chirps and the LED flashes
   - A detection event is sent to the phone
   - FlockFree shows a toast and places a cyan diamond marker on the map
4. After the drive, review each CYD marker in FlockFree:
   - Tap the marker → **Review as ALPR camera**
   - Adjust the position from the vehicle path to the actual camera location
   - Set the camera direction manually
   - Submit through the OSM POI editor

### Bench Testing Without RF

1. Connect FlockFree to the CYD over BLE
2. Tap **Simulate CYD detection** in FlockFree settings, or send `FYSIM` over serial
3. A test detection appears in the app
4. Cancel it after verifying the flow

## Serial Commands

| Command | Description |
|---------|-------------|
| `FYHELLO` | Returns pairing/status JSON |
| `FYSTATUS` | Returns full telemetry JSON |
| `FYGPS,lat,lon,acc,speed,course,sats,hdop,unix_time,offset` | Phone GPS input |
| `FYSIM` | Simulate a detection for testing |
| `FYSCREEN,next` | Cycle to next display screen |
| `FYTOUCH` | Report raw touch diagnostic values |

Screen rotation is handled by the physical boot button. There is no serial rotation command yet.

## Bluetooth Protocol

| Parameter | Value |
|-----------|-------|
| Peripheral name | `CYD-Flock-You` |
| Service UUID | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (write) UUID | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (notify) UUID | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |
| Line format | Newline-delimited text |
| Protocol version | 1 |

### Detection Event Format

```json
{
  "event": "detection",
  "detection_method": "wifi_wildcard_probe_ie_sig",
  "protocol": "wifi_2_4ghz",
  "mac_address": "70:c9:4e:aa:bb:cc",
  "oui": "70:c9:4e",
  "rssi": -63,
  "channel": 6,
  "frequency": 2437,
  "ssid": "",
  "confidence": "high",
  "gps": {
    "latitude": 45.171234,
    "longitude": -93.225678,
    "accuracy": 6.5,
    "age_ms": 250,
    "source": "phone"
  }
}
```

See [docs/deflock-pairing-protocol.md](docs/deflock-pairing-protocol.md) for full protocol details.

## CSV Log

When SD is available, detections append to `/flock.csv`:

```
millis,mac,oui,method,rssi,channel,frequency_mhz,lat,lon,accuracy_m,gps_age_ms,speed_kmph,course_deg,count
```

## Detection Methods

| Method | Confidence | Description |
|--------|------------|-------------|
| `wifi_wildcard_probe_ie_sig` | High | Probe request with wildcard SSID + full IE fingerprint match (ordered TLVs + vendor tags) |
| `wifi_wildcard_probe` | High | Probe request with wildcard SSID from a known OUI |
| `wifi_ssid` | Medium | SSID keyword match (flock, flck, test_flck) |
| `wifi_oui_addr2` | Medium | Transmitter address OUI match |
| `wifi_oui_addr1` | Low | Receiver-side OUI match (quiet/sleeping infrastructure) |
| `wifi_hidden_ssid` | Low | Hidden beacon/probe-response from a known OUI |
| `wifi_oui_addr3` | Low | BSSID OUI match (disabled by default) |

Target OUI list: 33 prefixes — see [`datasets/NitekryDPaul_wifi_ouis.md`](datasets/NitekryDPaul_wifi_ouis.md)

### Channel Scanning

- **Flock channel-priority hop:** {11, 6, 1, 10, 5, 2, 9, 4, 3, 8, 7, 12, 13} — hits the three non-overlapping 2.4 GHz channels (1, 6, 11) first, then fills in the rest
- 750ms dwell per channel — balanced between the Marauder's proven 1s dwell and cycle completion during a drive-by (full cycle: 9.75s)
- Covers all legal 2.4 GHz channels (1–13; channel 14 is restricted in most regions)
- RSSI threshold: -100 dBm (catches weak/distant cameras that would be missed at -95)
- Optimized WiFi init config disables AMPDU, CSI, and NVS for leaner promiscuous mode operation (~48KB RAM freed vs defaults)

### BLE Scanning

- BLE Flock scanning runs simultaneously with phone BLE UART — the ESP32 Bluedroid stack supports both peripheral and central roles at the same time
- Near-continuous scanning: 15s scan duration / 16s interval (94% duty cycle) — optimized for USB power
- Active scan with 99% radio duty cycle (`setInterval(100)`, `setWindow(99)`)
- Detection signatures: Penguin-NNNNNNNNNN names, FS Ext Battery, 10-digit numeric names (with OUI cross-check), XUNTONG manufacturer ID 0x09C8
- See [docs/marauder-comparison.md](docs/marauder-comparison.md) for the full analysis of what we adopted from the ESP32 Marauder and why

## Forks and Credits

- Firmware fork: [`colonelpanichacks/flock-you`](https://github.com/colonelpanichacks/flock-you)
- Android app fork: [`FoggedLens/deflock-app`](https://github.com/FoggedLens/deflock-app)
- Wildcard probe signature and OUI research: [`DeflockJoplin/flock-you`](https://github.com/DeflockJoplin/flock-you)
- BLE manufacturer ID work: [`wgreenberg/flock-you`](https://github.com/wgreenberg/flock-you)
- DeFlock project: [deflock.me](https://deflock.me)
- Wi-Fi OUI research: ØяĐöØцяöЪöяцฐ / @NitekryDPaul

## Disclaimer

This is a passive research and privacy-auditing tool. It does not transmit Wi-Fi, authenticate to networks, or bypass access controls. Wireless reception and public infrastructure mapping laws vary by jurisdiction. Use responsibly, follow local law, and avoid submitting unverified data to OpenStreetMap.
