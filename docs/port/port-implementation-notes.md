# CrowPanel Port — Developer Notes (v1)

Implementation record for the port of CYD-Flock-You to the Elecrow CrowPanel
ESP32-S3 Terminal 3.5" (DLC35020S). Design: `docs/port/bob-port-design.md`.
Hardware facts: `docs/port-research/scout-hardware-report.md`.

## Commits / structure

- `e734f18` — board abstraction refactor (ADR-2), zero behavior change on CYD
- `b76f18c` — `env:crowpanel` + `partitions_crowpanel.csv` + docs
- (docs commit) — README/DEVELOPER updates

## Implementation decisions and evidence

1. **Shared LCD/SD SPI bus is safe with stock libraries.** On the S3,
   TFT_eSPI's 18-bit (ILI9488) path drives the FSPI/SPI2 peripheral *through
   the Arduino global `SPI` object* (`SPIClass& spi = SPI` in
   `TFT_eSPI_ESP32_S3.c`) and wraps every write in
   `spi.beginTransaction(...)`; the SD library's `AcquireSPI` does the same on
   the same object. Both serialize on the per-bus mutex inside
   `spiTransaction()`, and neither nests transactions, so the shared bus
   (display CS=3, SD CS=10) needs no extra locking in our code. This mirrors
   Elecrow's own `bus_shared=true` pattern (`A-TF-PIC` demo, which even runs
   the bus at 40MHz for SD).
2. **Touch is poll-only.** The FT6236's TD_STATUS register (0x02) is polled
   once per `cydTouchTick()` (loop-rate, ≥1ms cadence); no INT pin is wired.
   A read failure (0xFF) is treated as "not pressed" so a missing controller
   cannot fake a stuck touch. `FYTOUCH` emits
   `{"event":"touch_status","points":n,"x":n,"y":n}` (debug-only; the app
   ignores unknown events).
3. **`setTxTimeoutMs(0)` kept under literal `!CYD_BUILD`.** Bob §7.9: every
   S3 build (xiao *and* CrowPanel) needs the USB-CDC no-host guard; CYD
   (classic ESP32, UART bridge) never does. After the `FY_UI_BUILD` rename
   this guard is the one place that still says `CYD_BUILD` on purpose.
4. **Rotation default 0 (portrait)** per ADR-4; the 320-wide landscape layout
   branch renders without clipping at 320×480 (Bob §3.5 verdict), just
   under-filled. Native 320×480 layout is v1.1 work.
5. **Buzzer on GPIO45** (Elecrow A-BUZZER demo). Note GPIO45 is an S3
   strapping pin for VDD_SPI voltage; driving it as a normal output after
   boot is what Elecrow's own demo does — verified safe by their demo code.
6. **No LED on CrowPanel** — `USE_LED 0`; `LED_FLASH_MS` still defined
   (call sites pass it as an argument even when `ledFlash` is compiled out).

## Deviations from Bob's design (smallest reasonable choices)

1. Bob suggested `SPI_FREQUENCY=27000000` conservative start; we ship
   **40MHz** — the verified-facts header explicitly blesses 40MHz as
   proven-safe for TFT_eSPI (Elecrow's LovyanGFX demo runs 60MHz). If any
   artifact shows up on hardware, drop the flag to 27MHz and re-flash; it's
   a one-line change.
2. Bob's `board_config.h` sketch put protocol constants near the board
   blocks; we hoisted `CYD_PAIR_NAME`/`CYD_PROTOCOL_VERSION`/UUIDs into a
   board-independent section of `main.cpp`/config so they can never diverge
   per-board (same spirit, stronger guard).
3. Bob's touch sketch was `GT911`-shaped; Scout verified the panel actually
   uses **FT6236** (I2C 0x38) — implemented per Scout (design defers to
   Scout on hardware values).
4. `CYD_TFT_BACKLIGHT_PIN` handling: CYD drives BL manually (unchanged);
   CrowPanel's BL is driven by TFT_eSPI's `init()` via the `TFT_BL`/
   `TFT_BACKLIGHT_ON` build flags (per Elecrow: GPIO46, active HIGH) — no
   manual pin touch in `cydInitDisplay` for CrowPanel.
5. On-screen brand reads `FlockFree CP` on CrowPanel (`FY_UI_BRAND`); the
   CYD keeps `FlockFree CYD`. UI text only — BLE name unchanged everywhere.

## Known gaps / future work (not blockers)

- Camera (OV2640) and audio (mic I2S) pins recorded in Scout's report but
  intentionally unconfigured (ADR-3). Add behind `FY_CAMERA_BUILD` /
  `FY_AUDIO_BUILD` flags when a feature needs them.
- Native 320×480 portrait layout (v1.1) — current layout renders correctly
  but under-fills the panel.
- Bluedroid dual-role (peripheral + central scan) on S3 is compiled but not
  yet hardware-smoke-tested; existing envs never exercised BLE on S3
  (risk 7.1). Karen's validation list #1 and #4 cover this.
- Flash/PSRAM module size is *assumed* N16R8-class (8MB partitions safe
  either way). If the physical board boots into a loop, check
  `memory_type` (qio_opi vs qio_qspi) per risk 7.7.

## Hardware test checklist (first flash on the physical board)

1. Backlight comes on, SCAN screen renders full-bleed (portrait, 320×480).
2. Tap cycles SCAN→GPS→LOG→LAST; `FYTOUCH` returns JSON on serial.
3. `FYSIM` triggers the red FLOCK FOUND flash; tap dismisses it.
4. SD card present: `/flock.csv` created, `sd:true` in pair_status;
   absent: `sd:false`, firmware continues.
5. BLE: phone sees `CYD-Flock-You`, pairs, `FYHELLO` works, GPS flows.
6. 30-min soak: concurrent Wi-Fi sniff + BLE scan + display refresh; watch
   for brownouts (shared-bus current) and heap/PSRAM stability.
7. If display is blank: verify BL (46) and SPI pins before suspecting code
   (wrong BL pin = dead backlight, per Bob §10).
8. If boot loops: likely PSRAM type mismatch — try `qio_qspi`.
## Diagnostic build

`env:crowpanel-diag` (`pio run -e crowpanel-diag`) is an audible-milestone
variant of `env:crowpanel` for remote bring-up where no serial tooling is
available — the buzzer (GPIO45) is the only confirmed-working output
channel. It adds `-DFY_DIAG_BUILD=1` on top of the exact `env:crowpanel`
flags; all normal code paths (`env:crowpanel`, `env:cyd`) are untouched.
Not a release artifact — flash it to localize a boot-loop, then return to
the normal build.

Boot progress is announced in beeps: **N beeps = stage N was reached and
returned; silence after N beeps (then the sequence restarting from M0) =
crash in stage N+1.** A continuous 60 ms B4 metronome beep every 5 s in
`loop()` means the firmware is fully up (steady state). The color-fill
self-test after M1 additionally proves the TFT bus/backlight by eye.

| Beeps | Milestone | Just-completed stage |
|-------|-----------|----------------------|
| 1 (M0) | Immediately at `setup()` start, before `Serial` init | CPU/Arduino core alive |
| 2 (M1) | After `cydInit()` returns, then RGB + black full-screen fills (300 ms each) | Display + touch init |
| 3 (M2) | After `SPIFFS.begin(true)` — beeps on pass **and** fail | Filesystem init |
| 4 (M3) | After `esp_wifi_start()` returns | WiFi driver up (Marauder-minimal cfg) |
| 5 (M4) | After `esp_wifi_set_promiscuous(true)` | Promiscuous sniffer armed |
| Victory tune (M5) | 6 ascending notes (D5→D6) after "merged WiFi detector started" | Steady state reached |
| 60 ms beep every 5 s | In `loop()` — still-alive metronome | Fully running |

Stage beeps are 80 ms at 1046 Hz (C6) with 140 ms gaps — distinct pitches
from the detection chirps (2000/2800 Hz), target heartbeat beeps (1500 Hz),
and the SMB startup chime, so milestone counts can't be confused with
detection audio. If the M5 victory tune never plays, the crash is between
M4 and the `dualPrintln("merged WiFi detector started")` line.

Build artifacts: `.pio/build/crowpanel-diag/bootloader.bin`,
`partitions.bin`, `firmware.bin`.
