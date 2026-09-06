# Scout Hardware Report — Elecrow ESP32-S3 Terminal 3.5" (product id 195316)

**Target:** Port of CYD-Flock-You (branch `crowpanel-port`) to the Elecrow "ESP32-S3 Terminal: 3.5" SPI Touch, Camera" — https://m.elecrow.com/pages/shop/product/details?id=195316
**Researched:** 2026-09-05 · Scout (research) + Jarvis (primary-source verification against Elecrow's official demo repo).

> Persisted by Jarvis: Scout's runtime lacked file-write capability, so this file is his delivered report, completed with facts Jarvis verified directly from Elecrow's official demo repository. Items marked **[J-verified]** were confirmed by Jarvis against the cited demo source; **UNCONFIRMED** items remain open.

## 1. Product identity (CRITICAL DISCREPANCY — RESOLVED)

**Product 195316 is the SPI-bus 3.5" terminal, official model DLC35020S.**

- Exact name: **CrowPanel-ESP32 Terminal 3.5inch SPI Capacitive Touch Display with OV2640 Camera** (Elecrow also lists it as "ESP32-S3 Terminal 3.5" SPI Touch, Camera"). SKU/model: **DLC35020S**.
- Sources: OpenELAB (official reseller) product data https://openelab.com/products/elecrow-dlc35020s (SKU `ELECROW-DLC35020S`, "Official Model: DLC35020S", "Product Name: CrowPanel-ESP32 Terminal 3.5inch SPI Capacitive Touch Display with OV2640 Camera", Display Driver: ILI9488, 320×480, ESP32-S3, OV2640); Elecrow's own download URL for this board: https://www.elecrow.com/download/product/DLC35020S/PlatformIO_SPI.zip (model code embedded, "SPI" variant).
- **Distinguished from siblings:**
  - **ESP Terminal 3.5" RGB Capacitive** — different SKU, RGB parallel-bus panel (schematic "ESP32- 3.5 TFT Display(RGB)-V1.0", ILI9488 over RGB bus; community repo paulhamsh/Elecrow-Terminal-RGB-LVGL). Elecrow ships both variants from the same demo repo under separate `RGB/` and `SPI/` trees — do **not** use RGB tree code. **[J-verified:** both trees exist in the demo repo; the `SPI/` tree contains the ILI9488 SPI schematic `ESP32- 3.5 TFT Display(SPI)V1.0_20230116.sch` and Eagle board files.**]**
  - **CrowPanel ESP32 HMI 3.5" (C309/C309B)** — older plain-ESP32 HMI board line, no camera; different product entirely.
- Official documentation artifact: Elecrow-RD GitHub demo repo **https://github.com/Elecrow-RD/esp32-terminal** (covers both variants; use the `SPI/` tree for this board). Wiki slug for the SPI terminal UNCONFIRMED (Elecrow wiki is JS-rendered).

## 2. MCU / module

- **ESP32-S3**, 2.4 GHz Wi-Fi + Bluetooth 5 LE (per OpenELAB spec table). Flash/PSRAM size **UNCONFIRMED** (Elecrow page JS-rendered; likely N16R8-class since the demo streams UXGA-JPEG + LVGL, but unverified — check silkscreen or the PlatformIO zip). **Port guidance:** use an 8 MB partition table + `board_upload.flash_size = 8MB` (safe whether physical flash is 8 or 16 MB), `qio_opi` + `BOARD_HAS_PSRAM`; adjust if the on-device smoke test shows a different module variant.

## 3. Display — **[J-verified against `SPI/3、arduino/ESP Terminal 3.5inch SPI sample code/A-LCD/A-LCD.ino`]**

- Controller **ILI9488**, native **320×480**, **SPI bus** (not QSPI, not RGB).
- Elecrow demo config (LovyanGFX v1, `Panel_ILI9488` + `Bus_SPI`): `SPI3_HOST`, SPI mode 0, **write 60 MHz**, read 16 MHz, `spi_3wire=true` with **`pin_dc = 42`** (3-wire is a LovyanGFX optimization; the board wires a real D/C line to GPIO42, so a standard 4-wire SPI driver with a GPIO DC pin — e.g. TFT_eSPI — is fully compatible), `dma_channel = SPI_DMA_CH_AUTO`, `bus_shared = true`, `use_lock = true`.
- **Pins:** MOSI=**13**, MISO=**14**, SCK=**12**, CS=**3**, DC=**42**, RST=**-1** (unused), BL=**46**.
- Panel geometry: `memory_width/height = 320/480`, `offset_x/y = 0`, `offset_rotation = 0`, `dummy_read_pixel = 8`, `readable = true`, **`invert = false`**, `rgb_order = false`, `dlen_16bit = false`.
- TFT_eSPI equivalent: `ILI9488_DRIVER`, `TFT_WIDTH=320`, `TFT_HEIGHT=480`, inversion OFF, RGB order normal, SPI frequency 40 MHz (conservative start; demo proves 60 MHz is stable).

## 4. Touch — **[J-verified against `.../A-TOUCH/FT6236.h`, `.../A-TOUCH/A-TOUCH.ino`]**

- Controller **FT6236** (capacitive), **I2C addr 0x38**, **SDA=2, SCL=1**.
- Demo reads touch coordinates from registers XH=0x03, XL=0x04, YH=0x05, YL=0x06 (no INT pin used; interrupt pin UNCONFIRMED). Number-of-touch-points register 0x02 (TD_STATUS) per the FT6x36 datasheet in `SPI/1、product-data/Capacitive_Touch_Data/`.
- Port need is minimal: the app uses touch only as a boolean "screen tapped" (screen cycler / alert dismiss); coordinates are debug-only. A ~40-line poll driver suffices.

## 5. Camera (DEFERRED for v1 port — pins recorded for future use)

- **OV2640**, DVP parallel, XCLK 20 MHz in demo (QVGA RGB565 streamed to LCD).
- Pins (`CAMERA_MODEL_ELECROW` in `.../A-Camera/camera_pins.h`): XCLK=**7**, SCCB SDA(SIOD)=**2**, SCL(SIOC)=**1**, D0–D7 = **8, 47, 48, 21, 18, 16, 15, 6**, VSYNC=**4**, HREF=**5**, PCLK=**17**, PWDN/RST = -1.
- **Bus conflict analysis:** camera SCCB shares I2C bus (pins 1/2) with the FT6236 touch — same bus, different addresses (0x30 vs 0x38), coexists fine. Camera DVP pins don't overlap display SPI (12/13/14). No display/camera bus conflict. SD shares the LCD SPI bus (bus_shared, CS-gated).

## 6. SD card — **[J-verified against `.../A-TF-PIC/A-TF-PIC.ino`]**

- SPI, **shares the LCD bus**: MOSI=13, MISO=14, SCK=12, **CS=10**. Elecrow demo runs LCD and SD simultaneously on `SPI3_HOST` with `bus_shared = true` and `use_lock = true`. Port must use a shared-bus SPI pattern (single SPI host, CS gating, transaction locking); start SD clock conservative (4 MHz).

## 7. Audio (DEFERRED for v1 port — pins recorded for future use) — **[J-verified against `.../A-AUDIO/A-AUDIO.ino`, `.../A-BUZZER/A-BUZZER.ino`]**

- **Mic:** I2S MEMS mic — BCK=**39**, WS=**38**, DATA=**41**, channel-select unused (-1). Demo uses AudioInI2S @ 44.1 kHz.
- **Buzzer:** GPIO **45**.

## 8. Flashing / USB

- USB-C via the S3's native USB (GPIO19/20 D+/D-, standard for ESP32-S3; board has BOOT/RST buttons — UNCONFIRMED silkscreen labels). Standard S3 download mode: hold BOOT, tap RST. `ARDUINO_USB_CDC_ON_BOOT=1` per the existing repo S3 env pattern.

## 9. PlatformIO / community references

- Official demo repo: **https://github.com/Elecrow-RD/esp32-terminal** (master branch; `SPI/` tree for this board). Key files:
  - `SPI/3、arduino/ESP Terminal 3.5inch SPI sample code/A-LCD/A-LCD.ino` — display config/pins
  - `SPI/3、arduino/ESP Terminal 3.5inch SPI sample code/A-TOUCH/FT6236.h` (+ `.cpp`, `A-TOUCH.ino`) — touch driver
  - `SPI/3、arduino/ESP Terminal 3.5inch SPI sample code/A-TF-PIC/A-TF-PIC.ino` — SD + shared-bus pattern
  - `SPI/3、arduino/ESP Terminal 3.5inch SPI sample code/A-Camera/camera_pins.h` — OV2640 pin map
  - `SPI/1、product-data/` — ILI9488 + FT6x36 datasheets, touch schematic
  - `SPI/2、ESP32-SPI-SCH+PCB-Eagle/` — board schematic `ESP32- 3.5 TFT Display(SPI)V1.0_20230116`
- Elecrow PlatformIO package: `https://www.elecrow.com/download/product/DLC35020S/PlatformIO_SPI.zip` (download did not yield a usable archive at research time; treat as unverified).
- No known community wardriving/detection firmware ports to this exact board at research time.

## 10. Open risks

1. **Flash/PSRAM size unconfirmed** — N16R8 assumed; 8 MB partition + qio_opi is the safe default either way.
2. **FT6236 INT pin unknown** — irrelevant for poll-mode touch.
3. **Wiki page for the SPI variant unconfirmed** — demo repo is the authoritative source; schematic PDF available in-repo.
4. **PSRAM memory_type** (qio_opi vs qio_qpi) may need adjustment on the physical board if it's not an R8 (OPI) module.