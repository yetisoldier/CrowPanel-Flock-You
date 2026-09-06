# Karen — Validation Report (re-run)

- **Branch:** `crowpanel-port` @ `f5a4cae` (clean tree)
- **Date:** 2026-09-06
- **Scope:** Fast, evidence-only re-validation of the CrowPanel port. Builds were
  already fresh (`.pio/build/crowpanel/firmware.bin` 09:41, `.pio/build/cyd/firmware.bin`
  09:42, both envs clean). No rebuilds executed.

---

## Fast Checks

### Check 1 — Wire contract preserved — **PASS**
- `CYD_PAIR_NAME` = `"CYD-Flock-You"` (`main.cpp:38`).
- `CYD_PROTOCOL_VERSION` = `1` (preserved by guard in `main.cpp`).
- BLE UUIDs unchanged: `6E400001/2/3-B5A3-F393-E0A9-E50E24DCCA9E` (`main.cpp:44-46`).
- `git diff cyd-upstream/main -- main.cpp`: zero `-` lines for any of the five
  wire-contract identifiers; zero `-` lines touching detection/pairing internals
  (`onWrite`, `createService`, `advertising->addServiceUUID`, `protocol_version`
  serialization, `cydBleTx`). The 154/69 insertions are pure structural
  reorganization (move defines behind `FY_UI_BUILD`, add `board_config.h` include
  and `Wire.h` for FT6236).
- `git diff --stat` for `main.cpp`: `154 insertions(+), 69 deletions(-)` —
  size delta explained entirely by the `#if defined(CYD_BUILD)` / `#elif
  defined(CROWPANEL_BUILD)` split.

### Check 2 — CYD regression — **PASS**
- `[env:cyd]` block in `platformio.ini` is **byte-identical** to
  `cyd-upstream/main:platformio.ini` (verified via process-substitution `diff`).
- No additions or modifications inside `[env:cyd]`. Only an `[env:crowpanel]`
  block is appended (confirmed by `git diff cyd-upstream/main -- platformio.ini`).
- Commit `f5a4cae "fix: CYD touch pressed-test must keep exact original semantics"`
  preserves the exact pressed-test used by the original `cydTouchReadPoint()`:
  IRQ-low gate only, no `z>0` threshold; x/y/z read sequence matches the
  upstream code so the debug log line is unchanged. Verified via
  `git show f5a4cae -- main.cpp` — the patch only removes the spurious
  `if (z == 0) return false;` line that was added during the port and restores
  the comment to explicitly note the original semantics.

### Check 3 — Pin ground truth — **PASS**

Cross-check between `board_config.h` (CROWPANEL_BUILD branch) and
`[env:crowpanel]` build_flags in `platformio.ini`:

| Item | Spec | env:crowpanel build_flag | board_config.h | Verdict |
|---|---|---|---|---|
| SPI MOSI | 13 | `-DTFT_MOSI=13` | — (consumed by TFT_eSPI) | ✅ |
| SPI MISO | 14 | `-DTFT_MISO=14` | — | ✅ |
| SPI SCK | 12 | `-DTFT_SCLK=12` | — | ✅ |
| LCD CS | 3 | `-DTFT_CS=3` | — | ✅ |
| LCD DC | 42 | `-DTFT_DC=42` | — | ✅ |
| LCD RST | -1 | `-DTFT_RST=-1` | — | ✅ |
| LCD BL | 46 | `-DTFT_BL=46` | — | ✅ |
| Driver | ILI9488 | `-DILI9488_DRIVER=1` | — | ✅ |
| Resolution | 320x480 | `-DTFT_WIDTH=320 -DTFT_HEIGHT=480` | `CYD_TFT_W=320 CYD_TFT_H=480` | ✅ |
| Inversion | none | (no `-DTFT_INVERSIONON`) | — | ✅ |
| SPI clock | 40 MHz | `-DSPI_FREQUENCY=40000000` | — | ✅ |
| Touch IC | FT6236 | (no XPT build_flag; FT6236 via Wire) | `FY_TOUCH_FT6236_I2C=1` | ✅ |
| Touch I2C addr | 0x38 | (no build_flag) | `FY_TOUCH_I2C_ADDR=0x38` | ✅ |
| Touch SDA | 2 | (no build_flag) | `FY_TOUCH_I2C_SDA=2` (passed to `Wire.begin()` at `main.cpp:1240`) | ✅ |
| Touch SCL | 1 | (no build_flag) | `FY_TOUCH_I2C_SCL=1` (passed to `Wire.begin()` at `main.cpp:1240`) | ✅ |
| SD CS | 10 | (no SD build_flag; main.cpp reads `CYD_SD_CS_PIN`) | `CYD_SD_CS_PIN=10` | ✅ |

I2C SDA/SCL are explicitly remapped at runtime via `Wire.begin(FY_TOUCH_I2C_SDA,
FY_TOUCH_I2C_SCL, FY_TOUCH_I2C_FREQ_HZ)` (`main.cpp:1240`) — no build-flag
needed because Wire defaults (GPIO 8/9 on esp32s3) are overridden per-call.

**Partition table (`partitions_crowpanel.csv`) — 8MB fit:**

| Name | Offset | Size | End | Adjacent end | Verdict |
|---|---|---|---|---|---|
| nvs | 0x9000 | 0x5000 | 0xe000 | otadata starts 0xe000 | ✅ contiguous |
| otadata | 0xe000 | 0x2000 | 0x10000 | app0 starts 0x10000 | ✅ contiguous |
| app0 | 0x10000 | 0x300000 | 0x310000 | app1 starts 0x310000 | ✅ contiguous |
| app1 | 0x310000 | 0x300000 | 0x610000 | spiffs starts 0x610000 | ✅ contiguous |
| spiffs | 0x610000 | 0x1E0000 | 0x7F0000 | coredump starts 0x7F0000 | ✅ contiguous |
| coredump | 0x7F0000 | 0x10000 | 0x800000 | (end of 8MB flash) | ✅ exact, no overlap |

Layout ends exactly at 0x800000 (8 MiB). No overlaps, no gaps. Fits
`board_build.flash_size = 8MB`. CrowPanel firmware (1.37 MB) fits well inside
either 3 MB app slot.

### Check 4 — No leftover ILI9341 / 320-as-width values in crowpanel env — **PASS**

- No `-DILI9341_DRIVER=` and no `-DILI9340_DRIVER=` in `[env:crowpanel]`
  (grep returns empty).
- The only `TFT_WIDTH=320` hit is the legitimate CrowPanel width in portrait.
- No `TFT_HEIGHT=320` (which would indicate a leftover CYD dimension).
- `USER_SETUP_LOADED` flag-driven pattern is intact:
  `-DUSER_SETUP_LOADED=1` set in `[env:crowpanel]` so TFT_eSPI's
  `User_Setup_Select.h` does not pull in a default panel config.
- Board-abstraction flag usage in source:
  `main.cpp` references `FY_UI_BUILD` / `FY_TOUCH_FT6236_I2C` / `FY_TOUCH_XPT2046_BITBANG`
  23 times; `board_config.h` references them 12 times plus the top-level
  `#if defined(CYD_BUILD) / #elif defined(CROWPANEL_BUILD) / #else` ladder.
  Guard ladder is correct and consistent.

---

## Defect Summary

No defects discovered in the static / source-level scope of this re-run.

| # | Severity | Area | Description | Recommendation |
|---|---|---|---|---|
| — | — | — | None | — |

---

## HARDWARE-PENDING (only verifiable on the physical board)

These checks cannot be made from source/build artifacts alone and must be
executed against the live DLC35020S hardware before declaring the port fully
shipped. They are **not** blockers for the source-side merge to `main`, but
**are** blockers for the first hardware bring-up.

1. **Backlight polarity.** `-DTFT_BACKLIGHT_ON=HIGH` is asserted from the
   Elecrow demo's `LGFX` config, but `TFT_eSPI` semantics for the
   `TFT_BL`/`TFT_BACKLIGHT_ON` pair vary by minor version. Verify the panel
   backlight is on after boot; if it's stuck off, either swap to `LOW` or
   confirm the user_setup override.
2. **SD card on shared SPI bus.** `CYD_SD_CS_PIN=10` relies on CS-gated sharing
   with the LCD. Verify SD read/write works at default SPI clock when the LCD
   is actively drawing (no MISO contention / bus-turnaround glitches).
3. **FT6236 touch polling latency.** With no INT pin, polling must be tight
   enough to feel responsive at 320x480 (drag during rotation cycle). Watch
   for missed taps and for the very first touch after sleep taking longer than
   the user expects.
4. **40 MHz SPI stability on this panel.** Code path assumes 40 MHz is safe
   with TFT_eSPI register writes (vs. the Elecrow demo's 60 MHz with
   LovyanGFX). Watch for `TFT_eSPI` debug spam about SPI timeouts / display
   corruption at boot or during screen redraw.
5. **Buzzer GPIO45 drive.** `-DBUZZER_PIN=45` is asserted from the Elecrow
   demo. Confirm the buzzer is actually wired to GPIO45 on the specific SKU
   you have (some CrowPanel 3.5" Terminal revisions use GPIO3 for the buzzer).
6. **Rotation 0 (portrait) → 1 (landscape) layout sanity.** The portrait-first
   default (`CYD_TFT_ROTATION=0`) and the rotation-debounce path (300 ms) need
   on-device verification — the CYD path was tested at rotation=1, 320x240.
7. **Partition OTA behavior.** Dual-OTA layout (`app0`/`app1`) is new for the
   CrowPanel. Confirm a self-induced reboot during OTA does not brick the
   device, and that `otadata` flip cleanly switches slots.
8. **`-DARDUINO_USB_CDC_ON_BOOT=1`.** Required for Serial over USB on the
   native USB port (no UART bridge). Confirm `Serial` works over the USB-C
   connector at 115200 baud.
9. **PSRAM availability at runtime.** `BOARD_HAS_PSRAM` is declared, but on
   first boot verify `psramFound()` returns true and that any heap-heavy paths
   (Wi-Fi scan buffers, JSON serialization for the BLE notify payload) prefer
   `ps_malloc` rather than `malloc`.

---

## Overall Verdict

# **SHIP** ✅

The CrowPanel port at `f5a4cae` on `crowpanel-port` is source-side correct and
buildable for both environments:

- Wire contract with the shipped deflock-app companion is byte-identical
  (`CYD_PAIR_NAME`, `protocol_version`, BLE service/RX/TX UUIDs all unchanged).
- CYD env is byte-identical to upstream; no detection/pairing logic hunks.
- Commit `f5a4cae` restores the exact original CYD touch pressed-test
  semantics.
- Pin ground truth cross-checks cleanly between `board_config.h` and the
  `[env:crowpanel]` build flags; I2C SDA/SCL are correctly remapped at runtime
  via `Wire.begin(SDA, SCL, freq)`.
- Partition layout fits exactly 8 MiB flash with no overlaps.
- No leftover ILI9341/240-as-height values in the CrowPanel env; the
  flag-driven pattern (`FY_UI_BUILD` / `FY_TOUCH_*` / `USER_SETUP_LOADED`) is
  intact.
- Both firmware artifacts are fresh and present (1.37 MB CrowPanel, 1.60 MB
  CYD, both well within their 3 MB app slots).

Merge-to-main is unblocked. Hardware bring-up list above is required before
declaring the port production-ready for end users, but does **not** gate the
source-side merge.

---

## Artifacts referenced

- `main.cpp`, `board_config.h`, `platformio.ini`, `partitions_crowpanel.csv`
- `.pio/build/crowpanel/firmware.bin` (1,366,384 B, mtime 2026-09-06 09:41)
- `.pio/build/cyd/firmware.bin` (1,597,472 B, mtime 2026-09-06 09:42)
- `git log` of `crowpanel-port`: `f5a4cae`, `0780b35`, `b76f18c`, `e734f18`,
  `6d68f74` (and upstream-merged history).
- `git diff cyd-upstream/main -- main.cpp` — structural only, 154/69.
- `git diff cyd-upstream/main -- platformio.ini` — additions only, `[env:cyd]`
  byte-identical.