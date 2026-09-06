// ============================================================
// board_config.h — canonical per-board hardware configuration
// ============================================================
// ADR-2 (docs/port/bob-port-design.md): every board pin, dimension, and
// touch-controller type lives here — the ONLY place board hardware is
// defined for UI builds. main.cpp code stays board-agnostic behind the
// FY_UI_BUILD / FY_TOUCH_* macros.
//
//   FY_UI_BUILD              1 when this build has display/touch/SD/GPS/BLE-UART
//                            (the full CYD feature set) — gates shared feature
//                            code that used to be CYD-only.
//   FY_TOUCH_XPT2046_BITBANG CYD's software-SPI resistive touch
//   FY_TOUCH_FT6236_I2C      CrowPanel's capacitive touch (Wire poll, no INT pin)
//
// Protocol constants (BLE name/UUIDs/protocol_version) intentionally do NOT
// live here: they are a wire contract with the shipped deflock-app and are
// identical across boards. See main.cpp and README "wire name" note.

#pragma once

#if defined(CYD_BUILD)

  // ---- Cheap Yellow Display (ESP32-2432S028R) ----
  #define FY_UI_BUILD 1
  #define FY_TOUCH_XPT2046_BITBANG 1

  #define BUZZER_PIN            26
  #define USE_BUZZER            1

  #define CYD_TFT_BACKLIGHT_PIN 21
  #define CYD_BOOT_BUTTON_PIN   0
  #define CYD_SD_CS_PIN         5
  #define CYD_LOG_FILE          "/flock.csv"
  #define CYD_TFT_ROTATION      1
  #define CYD_TFT_W             320
  #define CYD_TFT_H             240
  #define CYD_ROTATION_DEBOUNCE_MS 300
  #define CYD_TOUCH_IRQ_PIN     36
  #define CYD_TOUCH_MISO_PIN    39
  #define CYD_TOUCH_MOSI_PIN    32
  #define CYD_TOUCH_CLK_PIN     25
  #define CYD_TOUCH_CS_PIN      33

  #define LED_PIN               4
  #define USE_LED               1
  #define LED_ACTIVE_HIGH       0
  #define LED_FLASH_MS          120

  #define MIRROR_SERIAL         0

  #define CHANNEL_MODE          CHANNEL_MODE_FULL_HOP
  #define CHANNEL_DWELL_MS      750

  // On-screen brand. This is UI text only — it is NOT the BLE advertisement
  // name (CYD_PAIR_NAME in main.cpp), which is a wire contract and stays
  // "CYD-Flock-You" on every board.
  #define FY_UI_BRAND           "FlockFree CYD"

#elif defined(CROWPANEL_BUILD)

  // ---- Elecrow CrowPanel ESP32-S3 Terminal 3.5" (DLC35020S) ----
  // Pins verified against Elecrow's official demo repo
  // (Elecrow-RD/esp32-terminal, SPI/ tree) — see
  // docs/port-research/scout-hardware-report.md.
  #define FY_UI_BUILD 1
  #define FY_TOUCH_FT6236_I2C 1

  // Buzzer: Elecrow demo drives it directly (A-BUZZER), GPIO45.
  #define BUZZER_PIN            45
  #define USE_BUZZER            1

  // Backlight driven by TFT_eSPI via TFT_BL build flag (GPIO46, active HIGH).
  // No dedicated boot button on this board; GPIO0 is the strap/BOOT pin —
  // left alone (button code is compiled out via #if CYD_BUILD). Rotation
  // cycling is via touch or the FYSCREEN command instead.
  #define CYD_SD_CS_PIN         10   // shares the LCD SPI bus (CS-gated)
  #define CYD_LOG_FILE          "/flock.csv"
  #define CYD_TFT_ROTATION      0    // portrait default (ADR-4)
  #define CYD_TFT_W             320
  #define CYD_TFT_H             480
  #define CYD_ROTATION_DEBOUNCE_MS 300

  // FT6236 capacitive touch controller, I2C addr 0x38.
  // No INT pin is wired to a usable GPIO; poll mode only (A-TOUCH demo).
  #define FY_TOUCH_I2C_SDA      2
  #define FY_TOUCH_I2C_SCL      1
  #define FY_TOUCH_I2C_ADDR     0x38
  #define FY_TOUCH_I2C_FREQ_HZ  400000

  // No user LED on this board.
  #define USE_LED               0

  #define MIRROR_SERIAL         0

  #define CHANNEL_MODE          CHANNEL_MODE_FULL_HOP
  #define CHANNEL_DWELL_MS      750

  #define FY_UI_BRAND           "FlockFree CP"

#else

  // ---- Headless builds (xiao_esp32s3 & similar) ----
  #define FY_UI_BUILD 0
  #define BUZZER_PIN            3
  #define USE_BUZZER            1
  #define LED_PIN               21
  #define USE_LED               1
  #define LED_ACTIVE_HIGH       0
  #define LED_FLASH_MS          120
  #define MIRROR_SERIAL         1
  #define MIRROR_TX_PIN         43
  #define MIRROR_BAUD           115200
  #define CHANNEL_MODE          CHANNEL_MODE_CUSTOM
  #define CHANNEL_DWELL_MS      250

#endif