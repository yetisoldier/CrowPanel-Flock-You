# CrowPanel-Flock-You — Port Design

**Author:** Bob (Principal Architect)
**Date:** 2026-09-05
**Target:** Elecrow CrowPanel ESP32-S3 Terminal 3.5" (ILI9488-class 320×480, capacitive/resistive touch, camera)
**Source:** CYD-Flock-You firmware for ESP32-2432S028R (ESP32, ILI9341 240×320)
**Working copy:** `/home/yetisoldier/projects/CrowPanel-Flock-You`, branch `crowpanel-port`
**Status of hardware facts:** Scout report (`docs/port-research/scout-hardware-report.md`) **was not present** when this doc was written. Every board-specific pin / interface value below is marked **`TBD-Scout`**. Forge must not start the platformio env or pin map until those are filled in. Everything else (code structure, scope, risk, protocol constraints) is final.

---

## 1. Executive Summary

CYD-Flock-You is a passive Flock/ALPR detector: Wi-Fi promiscuous sniffing + BLE Flock-battery scanning on an ESP32, with a TFT status UI, SD CSV logging, and a **BLE Nordic-UART pairing protocol** consumed by the `deflock-app` Android companion. The detection engine and the BLE UART protocol are the crown jewels and **must not change**. Everything that must change is a thin hardware-adaptation layer: display driver config, touch driver, GPIO pin map, partition table, and UI layout geometry.

The codebase is already well-positioned for this port:

- **The detection/BLE/SD/GPS/UI feature block is already isolated behind `#if CYD_BUILD`** (main.cpp:7–16, 22–420, and ~50 other guards). The port adds a *second* board, not a rewrite.
- **An ESP32-S3 build path already exists** (`env:xiao_esp32s3`). It is a headless, display-less, BLE-less Wi-Fi-only build, but it **proves the core detection engine + SPIFFS + ESP-IDF promiscuous Wi-Fi compile and link cleanly on the ESP32-S3 Arduino core** (`framework-arduinoespressif32`, `platform espressif32@^6.3.0`). See ADR-1.
- **Touch is trivial to re-target.** The XPT2046 driver is bit-banged (main.cpp:1154–1200) but its output is used only as a *"screen was tapped"* boolean to cycle screens and dismiss the alert overlay (main.cpp:1530–1554). **Touch coordinates are never used for hit-testing** — there are no on-screen buttons. Any touch controller that yields a "pressed" edge is sufficient.

**Recommended v1 scope:** display + touch + Wi-Fi/BLE detection + pairing protocol on the new panel. **Camera and audio (speaker/mic) are explicitly deferred** (ADR-3). **Recommended structure:** add a third PlatformIO env `crowpanel` with a new `CROWPANEL_BUILD=1` flag, introduce an umbrella `FY_UI_BUILD` macro so the large shared blocks stop being CYD-exclusive, and extract board pins/geometry/touch into a single `board_config.h` plus a 3-function touch abstraction (ADR-2). This keeps CYD byte-for-byte behavior-identical while adding the new board.

---

## 2. Requirements

### 2.1 Functional (must preserve)
- Passive Wi-Fi promiscuous sniffing, full channel hop 1–11, all six detection methods (`wildcard_probe`, `wildcard_probe_ie_sig`, `oui_addr2`, `oui_addr1`, `oui_addr3`, `ssid`, `hidden_ssid`).
- BLE Flock-battery scanning (Penguin / FS Ext Battery / XUNTONG 0x09C8) concurrent with BLE UART peripheral role.
- BLE Nordic-UART pairing protocol, **protocol_version = 1**, device name **`CYD-Flock-You`** (see §3.3 — name is a wire constant, not a branding string).
- Phone GPS ingestion (`FYGPS`, both 7- and 10-field forms, plus raw `$` NMEA), `FYSIM`, `FYSTATUS`, `FYSCREEN,next`, `FYTOUCH`, `FYHELLO`.
- SD CSV append to `/flock.csv` with the exact 14-column schema.
- SPIFFS session persistence.
- TFT status UI: SCAN / GPS / LOG / LAST screens, full-screen red FLOCK FOUND flash, buzzer + LED feedback.

### 2.2 Functional (new / changed for CrowPanel)
- Drive a 320×480 ILI9488-class panel via TFT_eSPI.
- Read the CrowPanel touch controller and produce the same "tap = next screen / dismiss flash" behavior.
- Fit the UI to 320×480 without clipping (see §5, ADR-4).
- Use S3-appropriate flash size and partition layout.

### 2.3 Non-functional
- **Zero behavioral change** to detection, scoring, dedupe, queueing, JSON emission, and BLE protocol on either board.
- CYD env continues to build and behave identically (regression guard).
- New board builds clean with `pio run -e crowpanel`.
- Minimal, reviewable diff; existing coding conventions preserved (build-flag-driven TFT_eSPI, no `User_Setup.h`).

---

## 3. Hardware-Coupling Map (Part A.1)

All references are `main.cpp` unless noted. This is the exhaustive list of CYD hardware touchpoints Forge must adapt.

### 3.1 Build configuration (`platformio.ini`)
| Concern | CYD value | Notes for port |
|---|---|---|
| Board | `esp32dev` (ESP32) | → S3 board, **TBD-Scout** |
| TFT driver | `-DILI9341_2_DRIVER` | → `-DILI9488_DRIVER` (or panel-specific, **TBD-Scout**) |
| Dimensions | `-DTFT_WIDTH=240 -DTFT_HEIGHT=320` | → `320` / `480` |
| Display pins | `MISO=12 MOSI=13 SCLK=14 CS=15 DC=2 RST=-1 BL=21` | → **TBD-Scout** |
| SPI | `-DUSE_HSPI_PORT=1`, `SPI_FREQUENCY=40000000`, `SPI_READ_FREQUENCY=16000000` | S3 has no HSPI/VSPI naming; see ADR-5 + §7 risk |
| Color/inversion | `-DTFT_INVERSION_ON=1 -DTFT_RGB_ORDER=0` | ILI9488 differs; **TBD-Scout** verify colors |
| Partitions | `partitions_cyd.csv` (4 MB) | → `partitions_crowpanel.csv` (§5.3) |
| Flash | `board_*.flash_size = 4MB` | → 8/16 MB, **TBD-Scout** |

### 3.2 Pin / peripheral #defines
| Symbol | Line | CYD value | Port action |
|---|---|---|---|
| `BUZZER_PIN` | 23 (CYD), 80 (else) | 26 | → board pin or `USE_BUZZER 0`, **TBD-Scout** |
| `CYD_TFT_BACKLIGHT_PIN` | 26 | 21 | → board BL pin, **TBD-Scout** |
| `CYD_BOOT_BUTTON_PIN` | 27 | 0 | S3 GPIO0 is also strapping/boot; usable as button, verify **TBD-Scout** |
| `CYD_SD_CS_PIN` | 28 | 5 | → board TF-card CS, **TBD-Scout** |
| Touch pins | 38–42 | IRQ36 MISO39 MOSI32 CLK25 CS33 | Replaced by touch abstraction (ADR-2) |
| `LED_PIN` / `LED_ACTIVE_HIGH` | 86–96 | 4 / active-low | → board LED or `USE_LED 0`, **TBD-Scout** |
| `TFT_W/H`, `cydScreenW/H` | 34–36, 45–46 | 320×240 | → 320×480 (via new board block) |
| `CYD_TFT_ROTATION` | 34 | 1 | Choose portrait default for 3.5" (§5) |
| `MIRROR_TX_PIN` | 103 | 43 | Non-CYD only; irrelevant to CrowPanel UI build |

### 3.3 Protocol constants (**MUST NOT CHANGE — wire contract**)
| Symbol | Line | Value |
|---|---|---|
| `CYD_PAIR_NAME` | 31 | `"CYD-Flock-You"` |
| `CYD_PROTOCOL_VERSION` | 30 | `1` |
| Service UUID | 47 | `6E400001-…CA9E` (Nordic UART) |
| RX / TX UUID | 48–49 | `…0002` / `…0003` |
| `features[]` | 1117 | `wifi_promisc, phone_gps, sd_csv, tft_status, ble_uart` |

> **Critical:** `deflock-app` pairs on `device == "CYD-Flock-You" && protocol_version == 1` (`docs/deflock-pairing-protocol.md`). The BLE peripheral name and version are a **compatibility contract with the shipped companion app**, not branding. They stay `"CYD-Flock-You"` / `1` on CrowPanel. Repo/product name can be "CrowPanel-Flock-You"; the on-wire identity cannot. If a distinct advertised name is ever wanted, it requires a coordinated `deflock-app` change and a protocol bump — out of scope for this port. Document this prominently in the README.

### 3.4 Display / touch / SD driver code
| Function(s) | Lines | Coupling |
|---|---|---|
| `TFT_eSPI tft` instance | 333 | Driver selected entirely by build flags (good — no `User_Setup.h`) |
| Bit-banged XPT2046 touch | 1154–1210 | **Board-specific; replace via touch abstraction.** Uses `CYD_TOUCH_*` pins + software SPI. `cydTouchReadPoint()` returns x/y/z but callers ignore x/y. |
| `cydInitDisplay` | 1212–1226 | Backlight pin, boot button pinMode, `tft.init()`, `tft.setRotation()`, reads `tft.width()/height()` into `cydScreenW/H` (adaptive — good) |
| `cydSetDisplayRotation` | 1508–1517 | Rotation cycling; re-reads width/height (adaptive) |
| `cydButtonTick` | 1519–1528 | Boot button GPIO0 → rotate screen |
| `cydTouchTick` | 1530–1554 | **Only consumer of touch: any press → next screen / dismiss flash.** Coords used only in a debug `printf`. |
| `cydInitSd` | 1556–1571 | `SD.begin(CYD_SD_CS_PIN)` on the shared SPI bus |
| `cydEmitTouchStatus` (`FYTOUCH`) | 1202–1210 | Debug JSON of raw touch registers; XPT2046-specific — see §5.4 |

### 3.5 UI geometry — hardcoded coordinates (Part A.1 layout flags)
The renderer has two layout branches selected by **`if (cydScreenW >= 320)`** (1308, 1348, 1423, 1444): a "landscape" branch tuned for exactly **320 wide** and a "portrait" branch for **240 wide**. Consequences on a 320×480 panel:

- **Portrait mode (rot 0/2): width = 320 → the `>=320` "landscape" branch is taken**, but the panel is 480 tall. Content occupies only the top ~198 px; the lower ~280 px is empty. The footer floats correctly because it is anchored to `cydScreenH - 39` (1086).
- **Landscape mode (rot 1/3): width = 480 → "landscape" branch**, designed for 320 wide, leaves a ~160 px empty right margin. Nothing clips or overflows (all boxes have absolute x within 0–312), but it looks unbalanced.

**Every hardcoded coordinate that assumes 320-wide content:** the SCAN landscape block (1308–1324), GPS landscape (1348–1381), LOG landscape (1423–1429), LAST landscape (1444–1469), header (1228–1241, 36 px tall, `BTN` badge at `cydScreenW-42`), footer (1082–1109, 39 px, `LAST LOC` text at fixed x=84/138/148). The 240-wide portrait branch (1325–1342, 1382–1418, 1430–1439, 1470–1499) will **not** be used on the 3.5" panel because its min dimension is 320.

**Adaptive / safe elements (no change needed):** full-screen flash overlay centers on `cydScreenW/2, cydScreenH/2` (1254–1264); header/footer bars span `cydScreenW`; footer anchored to `cydScreenH-39`.

**Verdict:** the port **renders correctly (no clipping) with zero layout changes**, but wastes 40–60% of the panel. Layout polish is a **v1.1 nicety, not a blocker** (ADR-4). Recommend a follow-up 320×480-native layout, not gating the port on it.

### 3.6 Libraries in use (all under `CYD_BUILD`, main.cpp:8–15)
| Lib | Source | S3 status |
|---|---|---|
| `TFT_eSPI` | `bodmer/TFT_eSPI@^2.5.43` (lib_deps) | Supports ILI9488 + ESP32-S3. Config via build flags. |
| `SD` / `SPI` / `SPIFFS` | Arduino core | S3-native |
| `TinyGPSPlus` | `mikalhart/TinyGPSPlus@^1.1.0` | MCU-agnostic |
| `BLEDevice` (Bluedroid) | Arduino core `BLE*` | S3-native (see ADR-1 / risk §7.1) |
| `esp_wifi` promiscuous | ESP-IDF | S3-native (proven by `xiao_esp32s3` env) |

---

## 4. Architecture (Part A.3)

### ADR-1 — ESP32-S3 core compatibility of the detection/BLE engine
- **Problem:** Will the WiFi/BLE/detection code run on ESP32-S3 at all?
- **Evidence:** `env:xiao_esp32s3` (platformio.ini:5–21) already targets `seeed_xiao_esp32s3` with the same `main.cpp` and `platform espressif32@^6.3.0`. It compiles the entire non-UI path: `esp_wifi` promiscuous sniffer, ESP-IDF `wifi_init_config_t` (main.cpp:2593–2621), SPIFFS persistence, alert queue, OUI/IE matchers. The `WIFI_TASK_CORE_ID` / `WIFI_DYNAMIC_TX_BUFFER_NUM` symbols come from the framework sdkconfig, which adapts per-target. **ESP32-S3 is dual-core Xtensa LX7**, so core-pinning of the Wi-Fi task is valid exactly as on ESP32 (unlike single-core C3/C6).
- **Decision:** Treat the detection engine as **S3-ready**. The only *unproven-by-existing-env* piece is the **Bluedroid BLE stack**, which `xiao_esp32s3` does not exercise (BLE is under `CYD_BUILD`). Bluedroid is fully supported on S3 in the Arduino core; risk is low but must be smoke-tested (§7.1).
- **Consequence:** The port is a hardware-adaptation exercise, not a chip bring-up.

### ADR-2 — Board abstraction structure (the central decision)
- **Problem:** How to add CrowPanel without forking `main.cpp` and without a risky wide rename?
- **Options:**
  1. **`#ifdef` sprinkle** — add `|| CROWPANEL_BUILD` to every one of ~50 `#if CYD_BUILD` guards. *Rejected:* noisy, error-prone, and conflates "is this the CYD board" with "does this build have a UI."
  2. **Umbrella feature macro + board_config header (recommended).** Introduce two ideas: (a) a **feature macro `FY_UI_BUILD`** meaning "this build has display/touch/SD/GPS/BLE-UART," set to 1 for both CYD and CrowPanel; (b) a **`board_config.h`** that, based on `-DCYD_BUILD` or `-DCROWPANEL_BUILD`, defines the canonical pin/dimension/touch-type macros. Replace the large shared blocks' `#if CYD_BUILD` with `#if FY_UI_BUILD`; keep `#if CYD_BUILD` / `#if CROWPANEL_BUILD` only where board pins/geometry/touch genuinely differ.
  3. **Full driver-class refactor** (C++ `IBoard`/`IDisplay`/`ITouch` interfaces). *Rejected for v1:* over-engineered for a 2-board embedded project; larger diff, higher regression risk, contrary to the minimal-change mandate. Revisit only if a 3rd/4th board appears.
- **Decision:** **Option 2.** It is the smallest change that keeps CYD identical, isolates board deltas in one header, and reads naturally in the existing build-flag style.
- **Scope of the umbrella rename:** mechanical. The blocks at main.cpp:328–420, 987, 1976–1979, 2134–2165, 2488–2491, 2575–2578, 2648–2660 that are *feature* code (not CYD-pin code) flip `CYD_BUILD → FY_UI_BUILD`. The genuinely CYD-specific pin/`else` blocks (22–105) move into `board_config.h`.
- **Touch abstraction:** define a 3-function contract used by `cydTouchTick`/`cydInit`:
  ```c
  void   boardTouchInit();          // was cydInitTouch()
  bool   boardTouchPressed();       // edge-agnostic "is a finger down now"
  void   boardTouchDebug();         // was cydEmitTouchStatus() — FYTOUCH
  ```
  CYD implements these with the existing bit-banged XPT2046 (rename in place). CrowPanel implements them with its controller (GT911 I2C **or** XPT2046 SPI — **TBD-Scout**, see §5.4). `cydTouchTick` (1530–1554) collapses to `bool down = boardTouchPressed();` and keeps its debounce/cycle logic verbatim. **Because callers never use coordinates, no coordinate calibration is required for v1.**
- **Consequence:** Adding a 3rd board later = new `board_config.h` branch + new touch impl. No detection-code edits ever.

### ADR-3 — Camera & audio scope
- **Problem:** CrowPanel Terminal has a camera and likely I2S speaker/mic. In scope for v1?
- **Decision:** **Explicitly deferred.** Out of scope for v1.
- **Rationale:** The product's core value is Wi-Fi/BLE detection + display + the DeFlock pairing protocol. Camera (OV-series via `esp_camera`) and audio are large, independent subsystems with their own pins, PSRAM budgets, and DMA/timing concerns. They share **nothing** with the detection path and can be added later behind their own `FY_CAMERA_BUILD` / `FY_AUDIO_BUILD` flags without touching v1 code. Pulling them into v1 multiplies risk and PSRAM contention (the camera framebuffer competes with Wi-Fi RX buffers) for zero benefit to the detector.
- **Consequence:** v1 leaves camera/audio pins unconfigured. Document as "planned." Revisit criterion: a concrete feature request (e.g. capture a photo on detection).

### ADR-4 — UI layout for 320×480
- **Problem:** The 320-wide layout under-fills the 3.5" panel.
- **Decision:** **Ship v1 with the existing adaptive layout** (renders correctly, no clipping — §3.5). Track a **native 320×480 portrait layout as v1.1**.
- **Rationale:** Correct-but-sparse beats risky-but-pretty for the first working build. A native layout is pure additive UI work with no protocol/detection impact.
- **Consequence:** Choose **portrait (rotation 0)** as the CrowPanel default so the extra height reads as a natural vertical dashboard rather than a wide empty margin.

### ADR-5 — SPI bus & display interface
- **Problem:** CYD uses a dual-SPI-bus quirk (display on HSPI, touch bit-banged on GPIO, SD on VSPI). What does CrowPanel use?
- **Decision:** **TBD-Scout**, but design for the common CrowPanel-3.5" case: display + SD share one hardware SPI bus; touch is either I2C (GT911) or a separate SPI/shared-SPI (XPT2046). Drop `USE_HSPI_PORT` unless Scout confirms a specific bus mapping; on S3 let TFT_eSPI pick an SPI host from the pins.
- **Risk flag:** If Scout finds the panel is a **parallel/8080** interface rather than SPI, this becomes a materially larger change (parallel TFT_eSPI setup, different flags, more pins). The task header says "SPI touch + camera," implying SPI — but **Forge must confirm before building** (§7.2).

### 4.1 Data flow (unchanged by port)
```
Wi-Fi promiscuous cb (core-pinned) ─┐
                                     ├─► lock-free alert ring ─► loop()/drainAlertQueue ─┬─► fyAddDetection (SPIFFS table)
BLE scan cb (Bluedroid central) ─────┘                                                   ├─► emitDetectionJSON ─► dualPrintf ─► USB serial + BLE UART notify ─► deflock-app
                                                                                         ├─► cydRecordDetection ─► TFT flash + LED + buzzer
                                                                                         └─► cydLogDetectionCsv ─► SD /flock.csv
phone ─FYGPS─► BLE RX cb ─► cydBleRxQueue ─► cydBleDrainCommands ─► cydParseGpsCsv ─► cydGps
```
The port swaps only the **TFT + touch + SD-CS + pins** boxes. Every arrow stays.

---

## 5. Concrete Change List (Part B.5)

> Gate: fill all **TBD-Scout** values from `docs/port-research/scout-hardware-report.md` before building. If a value is still missing at build time, keep it `TBD-Scout` and treat the env as non-buildable (do not guess pins — wrong SPI/BL pins can hang the panel).

### 5.1 Files to CREATE
1. **`board_config.h`** (repo root, next to `main.cpp`) — canonical per-board macros:
   ```c
   #pragma once
   #if defined(CYD_BUILD)
     #define FY_UI_BUILD 1
     #define FY_TOUCH_XPT2046_BITBANG 1
     // pins exactly as today: BUZZER 26, BL 21, BOOT 0, SD_CS 5,
     // touch IRQ36/MISO39/MOSI32/CLK25/CS33, LED 4 active-low,
     // TFT_W 320, TFT_H 240, ROTATION 1
   #elif defined(CROWPANEL_BUILD)
     #define FY_UI_BUILD 1
     #define FY_TOUCH_GT911_I2C 1        // or FY_TOUCH_XPT2046_SPI — TBD-Scout
     #define BUZZER_PIN            /* TBD-Scout, else USE_BUZZER 0 */
     #define CROWPANEL_BL_PIN      /* TBD-Scout */
     #define CROWPANEL_BOOT_BTN    /* TBD-Scout (or 0) */
     #define CROWPANEL_SD_CS       /* TBD-Scout */
     #define CROWPANEL_LED_PIN     /* TBD-Scout, else USE_LED 0 */
     // GT911: I2C SDA/SCL/INT/RST — TBD-Scout
     #define CROWPANEL_TFT_W 320
     #define CROWPANEL_TFT_H 480
     #define CROWPANEL_TFT_ROTATION 0    // portrait default (ADR-4)
   #else
     #define FY_UI_BUILD 0               // headless xiao_esp32s3 path
   #endif
   ```
2. **`partitions_crowpanel.csv`** — see §5.3.
3. **Touch impl for CrowPanel** — either inline in `main.cpp` under `#if FY_TOUCH_GT911_I2C` or a small `touch_crowpanel.cpp`. If GT911: add lib (§5.5).
4. **`docs/port/` companion notes** already created (this doc). Add a README section documenting the wire-name constraint (§3.3).

### 5.2 Files to MODIFY
1. **`platformio.ini`** — add:
   ```ini
   [env:crowpanel]
   platform = espressif32@^6.3.0
   board = esp32-s3-devkitc-1        ; or Elecrow-specific board — TBD-Scout
   framework = arduino
   monitor_speed = 115200
   upload_speed = 921600
   lib_deps =
       bodmer/TFT_eSPI@^2.5.43
       mikalhart/TinyGPSPlus@^1.1.0
       ; GT911 touch lib if applicable — TBD-Scout (§5.5)
   build_flags =
       -DCORE_DEBUG_LEVEL=0
       -DCROWPANEL_BUILD=1
       -DARDUINO_USB_CDC_ON_BOOT=1     ; S3 native USB CDC
       -DBOARD_HAS_PSRAM
       -DUSER_SETUP_LOADED=1
       -DILI9488_DRIVER=1              ; TBD-Scout confirm panel driver
       -DTFT_WIDTH=320
       -DTFT_HEIGHT=480
       -DTFT_MISO=/*TBD-Scout*/
       -DTFT_MOSI=/*TBD-Scout*/
       -DTFT_SCLK=/*TBD-Scout*/
       -DTFT_CS=/*TBD-Scout*/
       -DTFT_DC=/*TBD-Scout*/
       -DTFT_RST=/*TBD-Scout*/
       -DTFT_BL=/*TBD-Scout*/
       -DTFT_BACKLIGHT_ON=HIGH
       -DLOAD_GLCD=1 -DLOAD_FONT2=1 -DLOAD_FONT4=1 -DSMOOTH_FONT=1
       -DSPI_FREQUENCY=27000000        ; ILI9488 tolerates ~27–40MHz; start conservative
       -DSPI_READ_FREQUENCY=16000000
   build_src_filter = +<main.cpp>
   board_build.arduino.memory_type = qio_opi   ; R8 octal PSRAM — TBD-Scout (qio_qspi if quad)
   board_build.partitions = partitions_crowpanel.csv
   board_build.filesystem = spiffs
   board_upload.flash_size = 8MB       ; TBD-Scout (8 or 16)
   board_build.flash_size = 8MB
   ```
   Do **not** set `USE_HSPI_PORT` unless Scout maps a specific bus. Do **not** set `TFT_INVERSION_ON` blindly — verify color inversion on ILI9488 (**TBD-Scout**; likely off).
2. **`main.cpp`** — three surgical categories, no logic changes:
   - Add `#include "board_config.h"` after the existing includes; move the include guard at line 7 (`#if CYD_BUILD`) to `#if FY_UI_BUILD`.
   - Flip **feature** guards `#if CYD_BUILD → #if FY_UI_BUILD` at: 328, 987, 1976, 2134, 2156, 2163, 2488, 2575, 2648 (and their matching `#endif`). Leave protocol constants and CYD pin `else`-blocks (22–105) to be sourced from `board_config.h`.
   - Rename touch calls to the abstraction (ADR-2): `cydInitTouch→boardTouchInit`, wrap `cydTouchReadPoint` usage in `cydTouchTick` behind `boardTouchPressed()`, route `cydEmitTouchStatus→boardTouchDebug`. CYD keeps the bit-banged impl verbatim under `#if FY_TOUCH_XPT2046_BITBANG`.
3. **`README.md`** — new title/badges, the **wire-name constraint** note (§3.3), CrowPanel build/flash instructions, camera/audio "planned" note.
4. **`DEVELOPER.md`** — add `pio run -e crowpanel` build/upload/monitor recipes alongside the CYD ones.

### 5.3 Partition table (`partitions_crowpanel.csv`)
CYD is 4 MB single-OTA (`partitions_cyd.csv`: app0 3 MB, spiffs ~960 KB). The S3 Terminal has 8/16 MB. Recommended 8 MB layout with room to grow (and toward future OTA/camera):
```
# Name,   Type, SubType,  Offset,   Size,     Flags
nvs,      data, nvs,      0x9000,   0x5000,
otadata,  data, ota,      0xe000,   0x2000,
app0,     app,  ota_0,    0x10000,  0x300000,
app1,     app,  ota_1,    0x310000, 0x300000,
spiffs,   data, spiffs,   0x610000, 0x1E0000,
coredump, data, coredump, 0x7F0000, 0x10000,
```
- Dual 3 MB OTA slots (enables future BLE/serial OTA; the firmware is single-image today but the headroom is free on 8 MB).
- ~1.9 MB SPIFFS (plenty for `session.json`; `oui.txt` is compiled in, not on FS).
- If Scout confirms **16 MB**, widen `spiffs` and add a `factory` reserve — Forge can scale offsets. If a simpler single-OTA scheme is preferred for v1, mirror `partitions.csv` (the xiao 3 MB app) scaled up. **Decision: use the dual-OTA 8 MB table above unless Scout says 4 MB.**

### 5.4 Touch strategy detail
- **Callers need only a debounced "pressed" edge** (main.cpp:1536 `down && !cydLastTouchDown && …`). No calibration, no rotation-aware coordinate mapping for v1.
- **If GT911 (I2C, capacitive)** — most likely for a "Terminal": add `TAMC_GT911` (§5.5). `boardTouchPressed()` = `touch.read(); return touch.isTouched;` (or `getTouches()>0`). `boardTouchInit()` sets SDA/SCL/INT/RST + `touch.begin()`. GT911 needs INT/RST pins and the panel's native resolution passed for internal scaling — coords unused so exact mapping is non-critical.
- **If XPT2046 (SPI, resistive)** — even simpler: reuse the existing bit-bang or switch to hardware SPI with the CrowPanel touch pins; `boardTouchPressed()` mirrors `cydTouchReadPoint()`'s IRQ check. **This is the lower-risk outcome; hope Scout confirms it.**
- **`FYTOUCH` debug (`boardTouchDebug`)** — CYD prints XPT2046 registers. For GT911, emit `{"event":"touch_status","touched":<bool>,"points":<n>}` or a no-op `{"event":"touch_status","supported":false}`. Non-protocol; deflock-app ignores non-JSON/unknown events. Keep it valid JSON.

### 5.5 Library choices
| Purpose | Choice | Version | License | Notes |
|---|---|---|---|---|
| Display | `bodmer/TFT_eSPI` | `^2.5.43` (same as CYD) | MIT/FreeBSD | ILI9488 supported; flag-driven, no `User_Setup.h` |
| GPS parse | `mikalhart/TinyGPSPlus` | `^1.1.0` | LGPL | unchanged |
| GT911 touch *(if capacitive)* | `tamctec/TAMC_GT911` | `^1.0.2` | MIT | **TBD-Scout** whether needed; small, maintained, widely used on CrowPanel |
| BLE / WiFi / SD / SPIFFS | Arduino-ESP32 core | platform `@^6.3.0` | — | S3-native |

Prefer **not** adding a heavier board-support library (e.g. LovyanGFX or an Elecrow BSP) unless Scout shows the panel needs init quirks TFT_eSPI can't express via flags. Keep parity with the CYD stack to minimize divergence.

---

## 6. Acceptance Criteria (Part B.6)

### 6.1 For Forge — build/structure gates
1. `pio run -e crowpanel` **compiles and links clean** (0 errors) once TBD-Scout pins are filled.
2. `pio run -e cyd` **still builds and is behaviorally unchanged** — diff the resulting behavior-relevant flags; the CYD binary must be functionally identical (regression guard). `pio run -e xiao_esp32s3` still builds.
3. **No edits** to: the promiscuous callback (`wifiSniffer`, 2293+), IE/OUI matchers (2195–2274), alert queue/dedupe, `fyAddDetection`, `emitDetectionJSON` field set/order, `cydEmitPairStatus` fields, all BLE UUIDs, `CYD_PAIR_NAME`, `CYD_PROTOCOL_VERSION`. Verify by `git diff` scope review.
4. `board_config.h` is the **only** place board pins/dimensions live for the UI builds; no raw CYD pin numbers remain in `#if CROWPANEL_BUILD` paths.
5. Touch is reached only through `boardTouchInit/Pressed/Debug`; `cydTouchTick` logic unchanged except the read call.
6. Partition CSV present and consistent with `board_*.flash_size`.

### 6.2 For Karen — validation, in priority order
1. **Protocol compatibility (highest).** With `deflock-app` (branch `cyd-flock-you-integration`): BLE scan finds `CYD-Flock-You`, connects on Nordic UART, `FYHELLO` → `pair_status` with `device=="CYD-Flock-You"`, `protocol_version==1`, features list intact. **The shipped app must pair with zero app-side changes.**
2. **Detection parity.** `FYSIM` produces a synthetic `event:"detection"` with identical JSON shape (fields, order, `gps` block when a fix is fresh) to CYD. If a real Flock RF source is available, a live Wi-Fi hit fires with correct `detection_method`/`confidence`.
3. **GPS ingestion.** `FYGPS` (both 7- and 10-field) and raw `$GxRMC` update `cydGps`; detections embed fresh GPS ≤10 s old; stale after.
4. **BLE Flock scan.** Concurrent peripheral+central: a Penguin/FS-Ext/XUNTONG advertiser is detected and triggers the red flash + JSON `detection_method:"ble_flock_battery"` while a phone stays paired (validates Bluedroid dual-role on S3 — §7.1).
5. **SD logging.** `/flock.csv` created with the exact 14-column header; rows append with correct schema; `csv_rows` increments; `sd:false` degrades gracefully when no card.
6. **Display.** Boots to SCAN; SCAN/GPS/LOG/LAST all render **without clipping** at 320×480; footer visible; **red FLOCK FOUND flash** shows and is dismissible by touch; clock/last-loc render.
7. **Touch.** A tap cycles screens and dismisses the flash; debounce feels right; no phantom/stuck touches. `FYTOUCH` returns valid JSON.
8. **Button/rotation.** If a usable button exists, it rotates; all 4 rotations render without clipping (portrait is default). If no button, screen stays in portrait — acceptable.
9. **Stability.** ≥30 min soak: no crash/brownout during concurrent Wi-Fi sniff + BLE scan + BLE UART + display refresh; watch for PSRAM/heap issues.
10. **Feedback.** Buzzer chirp + LED flash on new detections (or gracefully absent if `USE_*` disabled for missing hardware).

---

## 7. Risks (Part A.4)

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| 7.1 | **Bluedroid BLE on S3 unproven by existing env** (BLE is CYD-only; xiao env is BLE-less). Dual-role peripheral+central + concurrent Wi-Fi promiscuous is heavy on the shared 2.4 GHz radio. | Med | Bluedroid is officially S3-supported. Smoke-test dual-role early (Karen #1+#4). Falls back gracefully — worst case reduce BLE scan duty cycle (`CYD_BLE_FLOCK_SCAN_*`). No protocol impact. |
| 7.2 | **Display interface unknown: SPI vs parallel/8080.** Header says "SPI touch + camera" (implies SPI) but some CrowPanel 3.5" panels are 8080-parallel. Parallel = much bigger TFT_eSPI change. | **High until Scout confirms** | Hard gate: Forge must not build until Scout confirms interface + driver IC. Design assumes SPI (ADR-5); if parallel, re-scope display config (not detection). |
| 7.3 | **Touch controller unknown: GT911 (I2C) vs XPT2046 (SPI).** Different libs/pins. | Med | Touch abstraction (ADR-2) absorbs either; coords unused so no calibration. `board_config.h` selects impl. |
| 7.4 | **ILI9488 color/inversion/SPI-speed quirks.** 18-bit color, slower reads; wrong `TFT_INVERSION`/`RGB_ORDER` → wrong colors; too-fast SPI → artifacts. | Low | Start `SPI_FREQUENCY=27MHz`, inversion off, verify on hardware (Karen #6). Tune up after first good image. |
| 7.5 | **UI under-fills 320×480** (§3.5). | Low (cosmetic) | Ships correct; native layout is v1.1 (ADR-4). Not a blocker. |
| 7.6 | **Pin conflicts / strapping.** S3 GPIO0/45/46 are strapping; camera/SD/BL may collide; some pins input-only. | Med | Take every pin from Scout's verified map; never reuse CYD numbers on S3. `board_config.h` central review. |
| 7.7 | **Flash/PSRAM memory_type mismatch** (`qio_opi` vs `qio_qspi`) → boot loop. | Med | Confirm PSRAM type from Scout (R8=octal→`qio_opi`; R2=quad→`qio_qspi`) + flash size. |
| 7.8 | **CYD regression** from the umbrella-macro rename. | Low | Gate: `pio run -e cyd` unchanged (Karen/Forge #2); rename is mechanical; keep diff reviewable. |
| 7.9 | **`ARDUINO_USB_CDC_ON_BOOT` serial behavior** differs on S3 native USB; CYD path skips `setTxTimeoutMs(0)` (main.cpp:2546–2550, under `!CYD_BUILD`). | Low | CrowPanel is neither pure-CYD nor xiao; ensure the S3-CDC tx-timeout guard applies. Change guard to fire for `!CYD_BUILD` (already does) — confirm CrowPanel gets `setTxTimeoutMs(0)`. **Forge note:** line 2546 `#if !CYD_BUILD` already covers CrowPanel correctly; leave as-is. |

---

## 8. Open Questions Blocking Forge

**All blockers are Scout-dependent** (hardware facts). None are design-ambiguity.

1. **[BLOCKER] Display interface + driver IC** — SPI or 8080-parallel? ILI9488 / ST7796 / other? (Risk 7.2)
2. **[BLOCKER] Display pin map** — MISO/MOSI/SCLK/CS/DC/RST/BL, SPI host, frequency ceiling.
3. **[BLOCKER] Touch controller + pins** — GT911(I2C: SDA/SCL/INT/RST) or XPT2046(SPI)? (Risk 7.3)
4. **[BLOCKER] Flash size + PSRAM type** — 8 vs 16 MB; octal(R8) vs quad(R2) → `memory_type` + partitions. (Risk 7.7)
5. **[BLOCKER] SD card CS + bus** — shared with display SPI?
6. **[non-blocking] Buzzer / LED / button availability + pins** — if absent, set `USE_BUZZER 0` / `USE_LED 0` / skip button; software already guards these.
7. **[non-blocking] Exact PlatformIO board id** — `esp32-s3-devkitc-1` works generically; an Elecrow-specific board profile is nicer if it exists.

**Recommendation:** Forge implements the board-abstraction refactor (ADR-2: `board_config.h`, `FY_UI_BUILD` rename, touch functions) **now** — it is Scout-independent and unblocks the whole structure — then fills the `TBD-Scout` pin/env values and builds once Scout's report lands.

---

## 9. Implementation Order (for Forge)

1. **Refactor (Scout-independent, zero behavior change):** create `board_config.h` with the CYD branch = today's values; add `FY_UI_BUILD`; flip the feature guards; introduce `boardTouchInit/Pressed/Debug` with CYD's bit-bang impl. **Verify `pio run -e cyd` and `-e xiao_esp32s3` unchanged.** ← independently testable milestone.
2. **Add `env:crowpanel` + `partitions_crowpanel.csv`** with TBD-Scout placeholders; add the CrowPanel branch to `board_config.h`.
3. **Fill TBD-Scout values** from the report; implement CrowPanel touch (GT911 or XPT2046).
4. **First light:** `pio run -e crowpanel` → flash → confirm display boots, SCAN renders, touch cycles.
5. **Bring-up parity:** GPS, `FYSIM`, SD, BLE pairing with deflock-app, BLE Flock scan, soak.
6. **(v1.1, separate PR)** native 320×480 portrait layout.

---

## 10. Final Notes
- **Do not touch** the detection engine, JSON schema, or BLE protocol constants. The entire value of this port is that the *same* firmware brain runs on a nicer screen and still talks to the *unmodified* Android app.
- Keep the diff small and reviewable; the umbrella-macro rename should be mechanical and greppable.
- When Scout's report lands, **re-read §3.1/§5 and replace every `TBD-Scout`** before building. Wrong BL/SPI pins can appear as a dead backlight or a hung bus — not a code bug, so verify pins first when the first flash shows a blank screen.
