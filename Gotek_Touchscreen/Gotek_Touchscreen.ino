/*
  Gotek-Touchscreen-Interface
  Multi-Display Support: JC3248 and Waveshare

  Board settings:
    Board: ESP32S3 Dev Module
    USB CDC On Boot → Enabled
    PSRAM → OPI PSRAM
    Flash Size → 16MB
    Partition → Huge APP (3MB No OTA/1MB SPIFFS)

  Supported Hardware:
    - JC3248W535C (AXS15231B, 320x480, QSPI)
    - Waveshare ESP32-S3-Touch-LCD-2.8 (ST7789, 320x240, SPI+I2C touch)
*/

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <FS.h>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <vector>
#include <algorithm>
#include <ctype.h>

#include <USB.h>
#include <USBMSC.h>

// ============================================================================
// DISPLAY SELECTOR
// ============================================================================

#define DISPLAY_JC3248    1
#define DISPLAY_WAVESHARE 2

// SELECT YOUR DISPLAY HERE:
#define ACTIVE_DISPLAY DISPLAY_JC3248

// ============================================================================
// CONDITIONAL INCLUDES
// ============================================================================

#if ACTIVE_DISPLAY == DISPLAY_JC3248
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_axs15231b.h"
#elif ACTIVE_DISPLAY == DISPLAY_WAVESHARE
#include <LovyanGFX.hpp>
#endif

extern "C" {
  extern bool tud_mounted(void);
  extern void tud_disconnect(void);
  extern void tud_connect(void);
  extern void* ps_malloc(size_t size);
}

#define FW_VERSION "v0.7.0-Touchscreen"

using std::vector;
using std::sort;
using std::swap;

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================

#if ACTIVE_DISPLAY == DISPLAY_JC3248
#define LCD_WIDTH   320
#define LCD_HEIGHT  480
#define gW 480
#define gH 320

// SPI Pin Configuration for QSPI
#define LCD_PIN_CS    45
#define LCD_PIN_CLK   47
#define LCD_PIN_MOSI  21  // QSPI_D0
#define LCD_PIN_MISO  48  // QSPI_D1
#define LCD_PIN_D2    40  // QSPI_D2
#define LCD_PIN_D3    39  // QSPI_D3
#define LCD_PIN_BL    1

// Touch I2C Configuration
#define TOUCH_ADDR  0x3B
#define TOUCH_SDA   4
#define TOUCH_SCL   8

// SD Card Configuration
#define SD_CLK  12
#define SD_CMD  11
#define SD_D0   13

#elif ACTIVE_DISPLAY == DISPLAY_WAVESHARE
#define LCD_WIDTH   320
#define LCD_HEIGHT  240
#define gW 320
#define gH 240

// Waveshare ESP32-S3-Touch-LCD-2.8 SD pins
#define SD_CLK  14
#define SD_CMD  17
#define SD_D0   16

#endif

#define ROWS_PER_STRIP 10

// Colors (RGB565)
#define TFT_BLACK      0x0000
#define TFT_WHITE      0xFFFF
#define TFT_RED        0xF800
#define TFT_GREEN      0x07E0
#define TFT_BLUE       0x001F
#define TFT_CYAN       0x07FF
#define TFT_DARKGREY   0x7BEF
#define TFT_YELLOW     0xFFE0
#define TFT_ORANGE     0xFD20
#define TFT_GREY       0x8410

// ============================================================================
// LCD INIT COMMANDS (JC3248 - AXS15231B, 320x480)
// ============================================================================

#if ACTIVE_DISPLAY == DISPLAY_JC3248

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
  { 0xBB, (uint8_t[]){ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5 }, 8, 0 },
  { 0xA0, (uint8_t[]){ 0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00 }, 17, 0 },
  { 0xA2, (uint8_t[]){ 0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xf9, 0x10, 0x02, 0xff, 0xff, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A }, 31, 0 },
  { 0xD0, (uint8_t[]){ 0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12 }, 30, 0 },
  { 0xA3, (uint8_t[]){ 0xA0, 0x06, 0xAa, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55 }, 22, 0 },
  { 0xC1, (uint8_t[]){ 0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0d, 0x00, 0xFF, 0x40 }, 30, 0 },
  { 0xC3, (uint8_t[]){ 0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01 }, 11, 0 },
  { 0xC4, (uint8_t[]){ 0x00, 0x24, 0x33, 0x80, 0x00, 0xea, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50 }, 29, 0 },
  { 0xC5, (uint8_t[]){ 0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00 }, 23, 0 },
  { 0xC6, (uint8_t[]){ 0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22 }, 20, 0 },
  { 0xC7, (uint8_t[]){ 0x50, 0x32, 0x28, 0x00, 0xa2, 0x80, 0x8f, 0x00, 0x80, 0xff, 0x07, 0x11, 0x9c, 0x67, 0xff, 0x24, 0x0c, 0x0d, 0x0e, 0x0f }, 20, 0 },
  { 0xC9, (uint8_t[]){ 0x33, 0x44, 0x44, 0x01 }, 4, 0 },
  { 0xCF, (uint8_t[]){ 0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08 }, 27, 0 },
  { 0xD5, (uint8_t[]){ 0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00 }, 30, 0 },
  { 0xD6, (uint8_t[]){ 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00 }, 30, 0 },
  { 0xD7, (uint8_t[]){ 0x03, 0x01, 0x0b, 0x09, 0x0f, 0x0d, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F }, 19, 0 },
  { 0xD8, (uint8_t[]){ 0x02, 0x00, 0x0a, 0x08, 0x0e, 0x0c, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19 }, 12, 0 },
  { 0xD9, (uint8_t[]){ 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F }, 12, 0 },
  { 0xDD, (uint8_t[]){ 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F }, 12, 0 },
  { 0xDF, (uint8_t[]){ 0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90 }, 8, 0 },
  { 0xE0, (uint8_t[]){ 0x3B, 0x28, 0x10, 0x16, 0x0c, 0x06, 0x11, 0x28, 0x5c, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D }, 17, 0 },
  { 0xE1, (uint8_t[]){ 0x37, 0x28, 0x10, 0x16, 0x0b, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F }, 17, 0 },
  { 0xE2, (uint8_t[]){ 0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D }, 17, 0 },
  { 0xE3, (uint8_t[]){ 0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F }, 17, 0 },
  { 0xE4, (uint8_t[]){ 0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D }, 17, 0 },
  { 0xE5, (uint8_t[]){ 0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F }, 17, 0 },
  { 0xA4, (uint8_t[]){ 0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30 }, 16, 0 },
  { 0xA4, (uint8_t[]){ 0x85, 0x85, 0x95, 0x85 }, 4, 0 },
  { 0xBB, (uint8_t[]){ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 8, 0 },
  { 0x13, (uint8_t[]){ 0x00 }, 0, 0 },
  { 0x11, (uint8_t[]){ 0x00 }, 0, 120 },
  { 0x29, (uint8_t[]){ 0x00 }, 0, 20 },
  { 0x2C, (uint8_t[]){ 0x00, 0x00, 0x00, 0x00 }, 4, 0 },
};

#endif

// ============================================================================
// FONT DATA - 6x8 bitmap font (ASCII 32-126) - SHARED
// ============================================================================

static const uint8_t font6x8[95][6] PROGMEM = {
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // 32 space
  {0x00, 0x00, 0x4F, 0x00, 0x00, 0x00}, // 33 !
  {0x00, 0x07, 0x00, 0x07, 0x00, 0x00}, // 34 "
  {0x14, 0x7F, 0x14, 0x7F, 0x14, 0x00}, // 35 #
  {0x24, 0x2A, 0x7F, 0x2A, 0x12, 0x00}, // 36 $
  {0x62, 0x64, 0x08, 0x13, 0x23, 0x00}, // 37 %
  {0x36, 0x49, 0x49, 0x36, 0x50, 0x00}, // 38 &
  {0x00, 0x04, 0x03, 0x00, 0x00, 0x00}, // 39 '
  {0x00, 0x1C, 0x22, 0x41, 0x00, 0x00}, // 40 (
  {0x00, 0x41, 0x22, 0x1C, 0x00, 0x00}, // 41 )
  {0x14, 0x08, 0x3E, 0x08, 0x14, 0x00}, // 42 *
  {0x08, 0x08, 0x3E, 0x08, 0x08, 0x00}, // 43 +
  {0x00, 0x50, 0x30, 0x00, 0x00, 0x00}, // 44 ,
  {0x08, 0x08, 0x08, 0x08, 0x08, 0x00}, // 45 -
  {0x00, 0x60, 0x60, 0x00, 0x00, 0x00}, // 46 .
  {0x20, 0x10, 0x08, 0x04, 0x02, 0x00}, // 47 /
  {0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00}, // 48 0
  {0x00, 0x42, 0x7F, 0x40, 0x00, 0x00}, // 49 1
  {0x42, 0x61, 0x51, 0x49, 0x46, 0x00}, // 50 2
  {0x21, 0x41, 0x45, 0x4B, 0x31, 0x00}, // 51 3
  {0x18, 0x14, 0x12, 0x7F, 0x10, 0x00}, // 52 4
  {0x27, 0x45, 0x45, 0x45, 0x39, 0x00}, // 53 5
  {0x3C, 0x4A, 0x49, 0x49, 0x30, 0x00}, // 54 6
  {0x01, 0x71, 0x09, 0x05, 0x03, 0x00}, // 55 7
  {0x36, 0x49, 0x49, 0x49, 0x36, 0x00}, // 56 8
  {0x06, 0x49, 0x49, 0x29, 0x1E, 0x00}, // 57 9
  {0x00, 0x36, 0x36, 0x00, 0x00, 0x00}, // 58 :
  {0x00, 0x56, 0x36, 0x00, 0x00, 0x00}, // 59 ;
  {0x08, 0x14, 0x22, 0x41, 0x00, 0x00}, // 60 <
  {0x14, 0x14, 0x14, 0x14, 0x14, 0x00}, // 61 =
  {0x00, 0x41, 0x22, 0x14, 0x08, 0x00}, // 62 >
  {0x02, 0x01, 0x51, 0x09, 0x06, 0x00}, // 63 ?
  {0x32, 0x49, 0x79, 0x41, 0x3E, 0x00}, // 64 @
  {0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00}, // 65 A
  {0x7F, 0x49, 0x49, 0x49, 0x36, 0x00}, // 66 B
  {0x3E, 0x41, 0x41, 0x41, 0x22, 0x00}, // 67 C
  {0x7F, 0x41, 0x41, 0x41, 0x3E, 0x00}, // 68 D
  {0x7F, 0x49, 0x49, 0x49, 0x41, 0x00}, // 69 E
  {0x7F, 0x09, 0x09, 0x09, 0x01, 0x00}, // 70 F
  {0x3E, 0x41, 0x49, 0x49, 0x3A, 0x00}, // 71 G
  {0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00}, // 72 H
  {0x00, 0x41, 0x7F, 0x41, 0x00, 0x00}, // 73 I
  {0x20, 0x40, 0x41, 0x3F, 0x01, 0x00}, // 74 J
  {0x7F, 0x08, 0x14, 0x22, 0x41, 0x00}, // 75 K
  {0x7F, 0x40, 0x40, 0x40, 0x40, 0x00}, // 76 L
  {0x7F, 0x02, 0x0C, 0x02, 0x7F, 0x00}, // 77 M
  {0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00}, // 78 N
  {0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00}, // 79 O
  {0x7F, 0x09, 0x09, 0x09, 0x06, 0x00}, // 80 P
  {0x3E, 0x41, 0x41, 0x21, 0x5E, 0x00}, // 81 Q
  {0x7F, 0x09, 0x19, 0x29, 0x46, 0x00}, // 82 R
  {0x46, 0x49, 0x49, 0x49, 0x31, 0x00}, // 83 S
  {0x01, 0x01, 0x7F, 0x01, 0x01, 0x00}, // 84 T
  {0x3F, 0x40, 0x40, 0x40, 0x3F, 0x00}, // 85 U
  {0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00}, // 86 V
  {0x3F, 0x40, 0x38, 0x40, 0x3F, 0x00}, // 87 W
  {0x63, 0x14, 0x08, 0x14, 0x63, 0x00}, // 88 X
  {0x07, 0x08, 0x70, 0x08, 0x07, 0x00}, // 89 Y
  {0x61, 0x51, 0x49, 0x45, 0x43, 0x00}, // 90 Z
  {0x00, 0x7F, 0x41, 0x00, 0x00, 0x00}, // 91 [
  {0x02, 0x04, 0x08, 0x10, 0x20, 0x00}, // 92 
  {0x00, 0x41, 0x7F, 0x00, 0x00, 0x00}, // 93 ]
  {0x04, 0x02, 0x01, 0x02, 0x04, 0x00}, // 94 ^
  {0x40, 0x40, 0x40, 0x40, 0x40, 0x00}, // 95 _
  {0x00, 0x01, 0x02, 0x04, 0x00, 0x00}, // 96 `
  {0x20, 0x54, 0x54, 0x54, 0x78, 0x00}, // 97 a
  {0x7F, 0x48, 0x44, 0x44, 0x38, 0x00}, // 98 b
  {0x38, 0x44, 0x44, 0x44, 0x20, 0x00}, // 99 c
  {0x38, 0x44, 0x44, 0x48, 0x7F, 0x00}, // 100 d
  {0x38, 0x54, 0x54, 0x54, 0x18, 0x00}, // 101 e
  {0x08, 0x7E, 0x09, 0x01, 0x02, 0x00}, // 102 f
  {0x18, 0xA4, 0xA4, 0x9C, 0x78, 0x00}, // 103 g
  {0x7F, 0x08, 0x04, 0x04, 0x78, 0x00}, // 104 h
  {0x00, 0x44, 0x7D, 0x40, 0x00, 0x00}, // 105 i
  {0x20, 0x40, 0x44, 0x3D, 0x00, 0x00}, // 106 j
  {0x7F, 0x10, 0x28, 0x44, 0x00, 0x00}, // 107 k
  {0x00, 0x41, 0x7F, 0x40, 0x00, 0x00}, // 108 l
  {0x7C, 0x04, 0x78, 0x04, 0x78, 0x00}, // 109 m
  {0x7C, 0x08, 0x04, 0x04, 0x78, 0x00}, // 110 n
  {0x38, 0x44, 0x44, 0x44, 0x38, 0x00}, // 111 o
  {0x7C, 0x14, 0x14, 0x14, 0x08, 0x00}, // 112 p
  {0x08, 0x14, 0x14, 0x18, 0x7C, 0x00}, // 113 q
  {0x7C, 0x08, 0x04, 0x04, 0x08, 0x00}, // 114 r
  {0x48, 0x54, 0x54, 0x54, 0x20, 0x00}, // 115 s
  {0x04, 0x3F, 0x44, 0x40, 0x20, 0x00}, // 116 t
  {0x3C, 0x40, 0x40, 0x20, 0x7C, 0x00}, // 117 u
  {0x1C, 0x20, 0x40, 0x20, 0x1C, 0x00}, // 118 v
  {0x3C, 0x40, 0x30, 0x40, 0x3C, 0x00}, // 119 w
  {0x44, 0x28, 0x10, 0x28, 0x44, 0x00}, // 120 x
  {0x1C, 0xA0, 0xA0, 0x9C, 0x0C, 0x00}, // 121 y
  {0x44, 0x64, 0x54, 0x4C, 0x44, 0x00}, // 122 z
  {0x00, 0x08, 0x36, 0x41, 0x00, 0x00}, // 123 {
  {0x00, 0x00, 0x7F, 0x00, 0x00, 0x00}, // 124 |
  {0x00, 0x41, 0x36, 0x08, 0x00, 0x00}, // 125 }
  {0x08, 0x04, 0x08, 0x10, 0x08, 0x00}, // 126 ~
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

#if ACTIVE_DISPLAY == DISPLAY_JC3248
esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;
#elif ACTIVE_DISPLAY == DISPLAY_WAVESHARE
// LovyanGFX display class definition
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Touch_CST328 _touch_instance;
  lgfx::Light_PWM _light_instance;
public:
  LGFX(void) {
    // Bus config
    auto cfg = _bus_instance.config();
    cfg.spi_host = SPI2_HOST;
    cfg.spi_mode = 0;
    cfg.freq_write = 80000000;
    cfg.freq_read = 16000000;
    cfg.pin_sclk = 41;
    cfg.pin_mosi = 40;
    cfg.pin_miso = -1;
    cfg.pin_dc = 39;
    _bus_instance.config(cfg);
    _panel_instance.setBus(&_bus_instance);

    // Panel config
    auto pcfg = _panel_instance.config();
    pcfg.pin_cs = 42;
    pcfg.pin_rst = -1;
    pcfg.pin_busy = -1;
    pcfg.memory_width = 240;
    pcfg.memory_height = 320;
    pcfg.panel_width = 240;
    pcfg.panel_height = 320;
    pcfg.offset_x = 0;
    pcfg.offset_y = 0;
    pcfg.offset_rotation = 0;
    pcfg.readable = true;
    pcfg.invert = true;
    pcfg.rgb_order = false;
    pcfg.dlen_16bit = false;
    pcfg.bus_shared = true;
    _panel_instance.config(pcfg);

    // Backlight
    auto bl = _light_instance.config();
    bl.pin_bl = 2;
    bl.invert = false;
    bl.freq = 44100;
    bl.pwm_channel = 7;
    _light_instance.config(bl);
    _panel_instance.setLight(&_light_instance);

    // Touch
    auto tcfg = _touch_instance.config();
    tcfg.i2c_port = 1;
    tcfg.i2c_addr = 0x1A;
    tcfg.pin_sda = 1;
    tcfg.pin_scl = 3;
    tcfg.freq = 400000;
    tcfg.x_min = 0;
    tcfg.x_max = 240;
    tcfg.y_min = 0;
    tcfg.y_max = 320;
    _touch_instance.config(tcfg);
    _panel_instance.setTouch(&_touch_instance);

    setPanel(&_panel_instance);
  }
};
LGFX lcd;
#endif

JPEGDEC jpegdec;

#if ACTIVE_DISPLAY == DISPLAY_JC3248
uint16_t *framebuffer = NULL;
uint16_t *dma_buffer = NULL;
#endif

uint16_t text_fg = TFT_WHITE;
uint16_t text_bg = TFT_BLACK;
bool text_transparent = false;
int text_size = 1;
int text_x = 0, text_y = 0;

// CONFIG system
String cfg_display = "";
String cfg_lastfile = "";
String cfg_lastmode = "";
String cfg_theme = "DEFAULT";   // active theme folder name

// Theme system
String theme_path = "/THEMES/DEFAULT";  // resolved path to active theme
vector<String> theme_list;              // available theme names

// RAM disk variables
#define RAM_DISK_SIZE (2880 * 512)
uint8_t *ram_disk = NULL;
const char *ram_mount_point = "/ramdisk";

// Disk mode
enum DiskMode { MODE_ADF = 0, MODE_DSK = 1 };
DiskMode g_mode = MODE_ADF;

// USB MSC
USBMSC msc;
uint32_t msc_block_count;

// UI state
enum Screen { SCR_SELECTION = 0, SCR_DETAILS = 1, SCR_INFO = 2 };
Screen current_screen = SCR_SELECTION;

// File list
vector<String> file_list;
vector<String> display_names;
int selected_index = 0;

// Game list (merged multi-disk entries)
struct GameEntry {
  String name;              // display name (game base name)
  String jpg_path;          // cover art path (empty if none)
  int first_file_index;     // index into file_list for disk 1 (or only disk)
  int disk_count;           // number of disks (1 = single disk game)
};
vector<GameEntry> game_list;
int game_selected = 0;      // selected index in game_list
int scroll_offset = 0;      // scroll offset for list view

// Details screen
String detail_filename = "";
String detail_nfo_text = "";
String detail_jpg_path = "";

// Multi-disk support
vector<int> disk_set;         // file_list indices for all disks of current game
int loaded_disk_index = -1;   // file_list index of currently loaded disk (-1 = none)

// Touch state
bool touch_available = false;
uint16_t last_touch_x = 0, last_touch_y = 0;
unsigned long last_touch_time = 0;

// Swipe tracking
bool touch_active = false;
uint16_t touch_start_x = 0, touch_start_y = 0;
uint16_t touch_last_x = 0, touch_last_y = 0;
unsigned long touch_start_time = 0;

// ============================================================================
// GRAPHICS LAYER - JC3248 Display
// ============================================================================

#if ACTIVE_DISPLAY == DISPLAY_JC3248

// RGB565 byte-swap: ESP32 is little-endian, display expects big-endian
static inline uint16_t swap16(uint16_t c) {
  return (c >> 8) | (c << 8);
}

// Software rotation: virtual landscape (480x320) → physical portrait (320x480)
// 90° CCW: physical_x = virtual_y, physical_y = (LCD_HEIGHT-1) - virtual_x
static inline void fb_setPixel(int vx, int vy, uint16_t color) {
  int px = vy;
  int py = (LCD_HEIGHT - 1) - vx;
  framebuffer[py * LCD_WIDTH + px] = swap16(color);
}

static inline uint16_t fb_getPixel(int vx, int vy) {
  int px = vy;
  int py = (LCD_HEIGHT - 1) - vx;
  return swap16(framebuffer[py * LCD_WIDTH + px]);
}

void gfx_fillScreen(uint16_t color) {
  if (!framebuffer) return;
  uint16_t sc = swap16(color);
  uint16_t *ptr = framebuffer;
  for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
    *ptr++ = sc;
  }
}

void gfx_drawPixel(int x, int y, uint16_t color) {
  if (!framebuffer || x < 0 || x >= gW || y < 0 || y >= gH) return;
  fb_setPixel(x, y, color);
}

void gfx_fillRect(int x, int y, int w, int h, uint16_t color) {
  if (!framebuffer) return;
  // Clip to virtual bounds
  int vx0 = (x < 0) ? 0 : x;
  int vy0 = (y < 0) ? 0 : y;
  int vx1 = (x + w > gW) ? gW : x + w;
  int vy1 = (y + h > gH) ? gH : y + h;
  if (vx0 >= vx1 || vy0 >= vy1) return;

  // Iterate in physical row order for cache efficiency
  int px0 = vy0;
  int px1 = vy1 - 1;
  int py0 = LCD_HEIGHT - vx1;
  int py1 = LCD_HEIGHT - 1 - vx0;

  uint16_t sc = swap16(color);
  for (int py = py0; py <= py1; py++) {
    uint16_t *row = &framebuffer[py * LCD_WIDTH];
    for (int px = px0; px <= px1; px++) {
      row[px] = sc;
    }
  }
}

void gfx_drawRect(int x, int y, int w, int h, uint16_t color) {
  gfx_fillRect(x, y, w, 1, color);
  gfx_fillRect(x, y + h - 1, w, 1, color);
  gfx_fillRect(x, y, 1, h, color);
  gfx_fillRect(x + w - 1, y, 1, h, color);
}

void gfx_fillCircle(int cx, int cy, int r, uint16_t color) {
  for (int y = -r; y <= r; y++) {
    int w = (int)sqrt(r * r - y * y);
    gfx_fillRect(cx - w, cy + y, 2 * w + 1, 1, color);
  }
}

void gfx_drawCircle(int cx, int cy, int r, uint16_t color) {
  int x = 0, y = r, d = 3 - 2 * r;
  while (x <= y) {
    gfx_drawPixel(cx + x, cy + y, color);
    gfx_drawPixel(cx - x, cy + y, color);
    gfx_drawPixel(cx + x, cy - y, color);
    gfx_drawPixel(cx - x, cy - y, color);
    gfx_drawPixel(cx + y, cy + x, color);
    gfx_drawPixel(cx - y, cy + x, color);
    gfx_drawPixel(cx + y, cy - x, color);
    gfx_drawPixel(cx - y, cy - x, color);
    if (d < 0) {
      d = d + 4 * x + 6;
    } else {
      d = d + 4 * (x - y) + 10;
      y--;
    }
    x++;
  }
}

void gfx_setTextColor(uint16_t fg, uint16_t bg) {
  text_fg = fg;
  text_bg = bg;
}

void gfx_setTextSize(int s) {
  text_size = s;
}

void gfx_setCursor(int x, int y) {
  text_x = x;
  text_y = y;
}

int gfx_fontHeight() {
  return 8 * text_size;
}

int gfx_textWidth(const String& text) {
  return text.length() * 6 * text_size;
}

void gfx_print(const String& text) {
  for (char c : text) {
    if (c < 32 || c > 126) continue;
    const uint8_t *data = font6x8[c - 32];
    for (int col = 0; col < 6; col++) {
      uint8_t bits = pgm_read_byte(&data[col]);
      for (int row = 0; row < 8; row++) {
        bool fg = (bits & (1 << row));
        if (!fg && text_transparent) continue;  // skip background in transparent mode
        uint16_t color = fg ? text_fg : text_bg;
        for (int dy = 0; dy < text_size; dy++) {
          for (int dx = 0; dx < text_size; dx++) {
            gfx_drawPixel(text_x + col * text_size + dx, text_y + row * text_size + dy, color);
          }
        }
      }
    }
    text_x += 6 * text_size;
  }
}

void gfx_flush() {
  if (!framebuffer || !panel_handle) return;

  // Flush physical framebuffer (320x480 portrait) in strips via DMA bounce buffer
  for (int strip_y = 0; strip_y < LCD_HEIGHT; strip_y += ROWS_PER_STRIP) {
    int rows = (strip_y + ROWS_PER_STRIP > LCD_HEIGHT) ? (LCD_HEIGHT - strip_y) : ROWS_PER_STRIP;
    int bytes_to_copy = LCD_WIDTH * rows * 2;

    memcpy(dma_buffer, &framebuffer[strip_y * LCD_WIDTH], bytes_to_copy);

    esp_lcd_panel_draw_bitmap(panel_handle, 0, strip_y, LCD_WIDTH, strip_y + rows,
                              (const void *)dma_buffer);
    delayMicroseconds(500);
  }
}

// --- JPEG scaling support ---
// Decode JPEG to a temporary pixel buffer, then scale to fit target area.

static uint16_t *jpeg_tmp_buf = NULL;
static int jpeg_tmp_w = 0;
static int jpeg_tmp_h = 0;

// Callback: write decoded pixels into temp buffer with bounds checking.
// JPEG MCU blocks can extend beyond image dimensions, so we must clip.
int jpeg_buf_cb(JPEGDRAW *pDraw) {
  if (!jpeg_tmp_buf) return 0;
  uint16_t *src = pDraw->pPixels;
  int dx = pDraw->x;
  int dy = pDraw->y;
  int w  = pDraw->iWidth;
  int h  = pDraw->iHeight;
  for (int yy = 0; yy < h; yy++) {
    int row = dy + yy;
    if (row < 0 || row >= jpeg_tmp_h) continue;   // vertical bounds
    int copyW = w;
    if (dx < 0) continue;
    if (dx + copyW > jpeg_tmp_w) copyW = jpeg_tmp_w - dx;  // clip right edge
    if (copyW <= 0) continue;
    memcpy(&jpeg_tmp_buf[row * jpeg_tmp_w + dx], &src[yy * w], copyW * 2);
  }
  return 1;
}

void gfx_drawJpgFile(fs::FS &fs, const char* path, int x, int y, int maxW, int maxH) {
  File f = fs.open(path, "r");
  if (!f) return;

  size_t sz = f.size();
  if (sz == 0) { f.close(); return; }

  uint8_t *buf = (uint8_t *)malloc(sz);
  if (!buf) { f.close(); return; }

  f.read(buf, sz);
  f.close();

  // Open to get dimensions
  if (!jpegdec.openRAM(buf, sz, jpeg_buf_cb)) {
    free(buf);
    return;
  }

  int jpgW = jpegdec.getWidth();
  int jpgH = jpegdec.getHeight();

  // Sanity check dimensions
  if (jpgW <= 0 || jpgH <= 0 || jpgW > 2000 || jpgH > 2000) {
    jpegdec.close();
    free(buf);
    return;
  }

  // Allocate temp buffer for full decoded image (PSRAM)
  jpeg_tmp_buf = (uint16_t *)ps_malloc(jpgW * jpgH * 2);
  if (!jpeg_tmp_buf) {
    jpegdec.close();
    free(buf);
    return;
  }
  memset(jpeg_tmp_buf, 0, jpgW * jpgH * 2);  // clear to black
  jpeg_tmp_w = jpgW;
  jpeg_tmp_h = jpgH;

  // Decode entire image into temp buffer
  jpegdec.decode(0, 0, 0);
  jpegdec.close();
  free(buf);  // compressed data no longer needed

  // Calculate scale to fit maxW x maxH, maintaining aspect ratio
  float scaleX = (float)maxW / (float)jpgW;
  float scaleY = (float)maxH / (float)jpgH;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;

  // Sanity check scale
  if (scale <= 0.0f || scale > 20.0f) {
    free(jpeg_tmp_buf);
    jpeg_tmp_buf = NULL;
    return;
  }

  int drawW = (int)(jpgW * scale);
  int drawH = (int)(jpgH * scale);
  if (drawW <= 0 || drawH <= 0) {
    free(jpeg_tmp_buf);
    jpeg_tmp_buf = NULL;
    return;
  }

  // Center image within the target area
  int offX = x + (maxW - drawW) / 2;
  int offY = y + (maxH - drawH) / 2;

  // Nearest-neighbor scaling blit to framebuffer
  for (int row = 0; row < drawH; row++) {
    int srcY = (int)(row / scale);
    if (srcY >= jpgH) srcY = jpgH - 1;
    for (int col = 0; col < drawW; col++) {
      int srcX = (int)(col / scale);
      if (srcX >= jpgW) srcX = jpgW - 1;
      int vx = offX + col;
      int vy = offY + row;
      if (vx >= 0 && vx < gW && vy >= 0 && vy < gH) {
        fb_setPixel(vx, vy, jpeg_tmp_buf[srcY * jpgW + srcX]);
      }
    }
    if (row % 20 == 0) yield();  // prevent watchdog timeout on large images
  }

  free(jpeg_tmp_buf);
  jpeg_tmp_buf = NULL;
}

#elif ACTIVE_DISPLAY == DISPLAY_WAVESHARE

// Waveshare LovyanGFX wrapper functions
void gfx_fillScreen(uint16_t color) {
  lcd.fillScreen(color);
}

void gfx_drawPixel(int x, int y, uint16_t color) {
  lcd.drawPixel(x, y, color);
}

void gfx_fillRect(int x, int y, int w, int h, uint16_t color) {
  lcd.fillRect(x, y, w, h, color);
}

void gfx_drawRect(int x, int y, int w, int h, uint16_t color) {
  lcd.drawRect(x, y, w, h, color);
}

void gfx_fillCircle(int cx, int cy, int r, uint16_t color) {
  lcd.fillCircle(cx, cy, r, color);
}

void gfx_drawCircle(int cx, int cy, int r, uint16_t color) {
  lcd.drawCircle(cx, cy, r, color);
}

void gfx_setTextColor(uint16_t fg, uint16_t bg) {
  text_fg = fg;
  text_bg = bg;
  lcd.setTextColor(fg, bg);
}

void gfx_setTextSize(int s) {
  text_size = s;
  lcd.setTextSize(s);
}

void gfx_setCursor(int x, int y) {
  text_x = x;
  text_y = y;
  lcd.setCursor(x, y);
}

int gfx_fontHeight() {
  return 8 * text_size;
}

int gfx_textWidth(const String& text) {
  return text.length() * 6 * text_size;
}

void gfx_print(const String& text) {
  lcd.print(text);
}

void gfx_flush() {
  // LovyanGFX writes directly, no-op
}

void gfx_drawJpgFile(fs::FS &fs, const char* path, int x, int y, int maxW, int maxH) {
  // Read file into RAM to get dimensions, then draw scaled
  File f = fs.open(path, "r");
  if (!f) return;
  size_t sz = f.size();
  uint8_t *buf = (uint8_t *)malloc(sz);
  if (!buf) { f.close(); return; }
  f.read(buf, sz);
  f.close();

  JPEGDEC jpg;
  if (!jpg.openRAM(buf, sz, NULL)) {
    free(buf);
    return;
  }
  int jpgW = jpg.getWidth();
  int jpgH = jpg.getHeight();
  jpg.close();

  // Calculate scale to fit maxW x maxH, maintaining aspect ratio
  float scaleX = (float)maxW / (float)jpgW;
  float scaleY = (float)maxH / (float)jpgH;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;

  int drawW = (int)(jpgW * scale);
  int drawH = (int)(jpgH * scale);
  int offX = x + (maxW - drawW) / 2;
  int offY = y + (maxH - drawH) / 2;

  // LovyanGFX drawJpg with scale factors
  lcd.drawJpg(buf, sz, offX, offY, drawW, drawH, 0, 0, scale, scale);
  free(buf);
}

#endif

// ============================================================================
// DISPLAY INITIALIZATION (Backend-specific)
// ============================================================================

#if ACTIVE_DISPLAY == DISPLAY_JC3248

void displayInit() {
  // Allocate physical-sized buffers (320x480 portrait)
  framebuffer = (uint16_t *)ps_malloc(LCD_WIDTH * LCD_HEIGHT * 2);
  dma_buffer = (uint16_t *)heap_caps_malloc(LCD_WIDTH * ROWS_PER_STRIP * 2,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

  if (!framebuffer || !dma_buffer) {
    Serial.println("Failed to allocate framebuffers!");
    while (1);
  }

  spi_bus_config_t buscfg = {};
  buscfg.data0_io_num = LCD_PIN_MOSI;
  buscfg.data1_io_num = LCD_PIN_MISO;
  buscfg.sclk_io_num  = LCD_PIN_CLK;
  buscfg.data2_io_num = LCD_PIN_D2;
  buscfg.data3_io_num = LCD_PIN_D3;
  buscfg.max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2;

  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = LCD_PIN_CS;
  io_config.dc_gpio_num = -1;
  io_config.spi_mode = 3;
  io_config.pclk_hz = 50 * 1000 * 1000;
  io_config.trans_queue_depth = 1;
  io_config.on_color_trans_done = NULL;
  io_config.user_ctx = NULL;
  io_config.lcd_cmd_bits = 32;
  io_config.lcd_param_bits = 8;
  io_config.flags.quad_mode = true;

  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

  axs15231b_vendor_config_t vendor_config = {};
  vendor_config.init_cmds = lcd_init_cmds;
  vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
  vendor_config.flags.use_qspi_interface = 1;

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = -1;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = 16;
  panel_config.vendor_config = &vendor_config;

  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io_handle, &panel_config, &panel_handle));
  esp_lcd_panel_reset(panel_handle);
  delay(100);
  esp_lcd_panel_init(panel_handle);
  delay(200);

  ledcAttach(LCD_PIN_BL, 5000, 8);
  ledcWrite(LCD_PIN_BL, 200);
}

#elif ACTIVE_DISPLAY == DISPLAY_WAVESHARE

void displayInit() {
  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(200);
}

#endif

// ============================================================================
// TOUCH INTERFACE (Backend-specific)
// ============================================================================

#if ACTIVE_DISPLAY == DISPLAY_JC3248

void touchInit() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
  touch_available = true;
}

bool touchRead(uint16_t *x, uint16_t *y) {
  uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, 11);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 8) != 8) return false;
  uint8_t buf[8];
  for (int i = 0; i < 8; i++) buf[i] = Wire.read();
  if (buf[1] == 0) return false;
  uint16_t raw_x = ((buf[2] & 0x0F) << 8) | buf[3];
  uint16_t raw_y = ((buf[4] & 0x0F) << 8) | buf[5];
  // Rotate touch: physical portrait → virtual landscape
  *x = (LCD_HEIGHT - 1) - raw_y;
  *y = raw_x;
  return true;
}

#elif ACTIVE_DISPLAY == DISPLAY_WAVESHARE

void touchInit() {
  // LovyanGFX handles touch init in LGFX constructor
  touch_available = true;
}

bool touchRead(uint16_t *x, uint16_t *y) {
  lgfx::touch_point_t tp;
  if (lcd.getTouch(&tp)) {
    *x = tp.x;
    *y = tp.y;
    return true;
  }
  return false;
}

#endif

// ============================================================================
// CONFIG SYSTEM
// ============================================================================

void loadConfig() {
  cfg_display = "";
  cfg_lastfile = "";
  cfg_lastmode = "";

  File f = SD_MMC.open("/CONFIG.TXT", "r");
  if (!f) return;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (line.startsWith("#")) continue;  // skip comments

    int eqIdx = line.indexOf('=');
    if (eqIdx < 0) continue;

    String key = line.substring(0, eqIdx);
    String val = line.substring(eqIdx + 1);
    key.trim();
    val.trim();

    if (key == "DISPLAY") {
      cfg_display = val;
    } else if (key == "LASTFILE") {
      cfg_lastfile = val;
    } else if (key == "LASTMODE") {
      cfg_lastmode = val;
    } else if (key == "THEME") {
      cfg_theme = val;
    }
  }
  f.close();

  // Resolve theme path
  if (cfg_theme.length() == 0) cfg_theme = "DEFAULT";
  theme_path = "/THEMES/" + cfg_theme;
}

void saveConfig() {
  File f = SD_MMC.open("/CONFIG.TXT", "w");
  if (!f) return;

  if (cfg_display.length() > 0) {
    f.println("DISPLAY=" + cfg_display);
  }
  if (cfg_lastfile.length() > 0) {
    f.println("LASTFILE=" + cfg_lastfile);
  }
  if (cfg_lastmode.length() > 0) {
    f.println("LASTMODE=" + cfg_lastmode);
  }
  f.println("THEME=" + cfg_theme);

  f.close();
}

// Scan /THEMES/ folder for available theme subfolders
void scanThemes() {
  theme_list.clear();
  File root = SD_MMC.open("/THEMES");
  if (!root || !root.isDirectory()) {
    theme_list.push_back("DEFAULT");
    return;
  }
  File entry;
  while ((entry = root.openNextFile())) {
    if (entry.isDirectory()) {
      String name = entry.name();
      // entry.name() returns full path on some ESP32 cores
      int lastSlash = name.lastIndexOf('/');
      if (lastSlash >= 0) name = name.substring(lastSlash + 1);
      if (name.length() > 0 && !name.startsWith(".")) {
        theme_list.push_back(name);
      }
    }
    entry.close();
  }
  root.close();
  // Sort alphabetically
  for (int i = 0; i < (int)theme_list.size() - 1; i++) {
    for (int j = i + 1; j < (int)theme_list.size(); j++) {
      if (theme_list[j] < theme_list[i]) {
        String tmp = theme_list[i];
        theme_list[i] = theme_list[j];
        theme_list[j] = tmp;
      }
    }
  }
  if (theme_list.empty()) {
    theme_list.push_back("DEFAULT");
  }
}

// Switch to the next available theme
void cycleTheme() {
  if (theme_list.empty()) scanThemes();
  int idx = 0;
  for (int i = 0; i < (int)theme_list.size(); i++) {
    if (theme_list[i] == cfg_theme) { idx = i; break; }
  }
  idx = (idx + 1) % theme_list.size();
  cfg_theme = theme_list[idx];
  theme_path = "/THEMES/" + cfg_theme;
  saveConfig();
}

// ============================================================================
// SD CARD INTERFACE
// ============================================================================

void init_sd_card() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD card mount failed!");
  }
}

// ============================================================================
// File system scanning
// ============================================================================
// Folder structure:
//   /ADF/<GameName>/<GameName>.adf   (+ optional .nfo, .jpg)
//   /DSK/<GameName>/<GameName>.dsk   (+ optional .nfo, .jpg)
//
// file_list stores full paths like "/ADF/Giganoid/Giganoid.adf"
// Also supports legacy flat layout: /*.adf / *.dsk in root
// ============================================================================

// Find a file by name (case-insensitive) inside a given directory.
// Returns the full path if found, empty string otherwise.
String findFileInDir(const String &dirPath, const String &targetName) {
  File dir = SD_MMC.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) return "";

  String targetUpper = targetName;
  targetUpper.toUpperCase();

  File entry;
  while ((entry = dir.openNextFile())) {
    String fname = entry.name();
    // entry.name() may return full path on some ESP32 cores
    int slash = fname.lastIndexOf('/');
    if (slash >= 0) fname = fname.substring(slash + 1);

    String upper = fname;
    upper.toUpperCase();
    if (upper == targetUpper) {
      entry.close();
      dir.close();
      return dirPath + "/" + fname;
    }
    entry.close();
  }
  dir.close();
  return "";
}

// List disk images by scanning /<MODE>/ subfolders.
// Each subfolder represents one game/program.
// Falls back to flat root scanning for legacy layout.
vector<String> listImages() {
  vector<String> images;
  String modeDir = (g_mode == MODE_ADF) ? "/ADF" : "/DSK";
  String ext1 = (g_mode == MODE_ADF) ? ".ADF" : ".DSK";

  File root = SD_MMC.open(modeDir.c_str());
  if (root && root.isDirectory()) {
    // New layout: /<MODE>/<GameFolder>/<file>.adf|dsk
    File gameDir;
    while ((gameDir = root.openNextFile())) {
      if (!gameDir.isDirectory()) { gameDir.close(); continue; }

      String gameDirPath = gameDir.name();
      // Ensure full path
      if (!gameDirPath.startsWith("/")) gameDirPath = modeDir + "/" + gameDirPath;

      // Scan this game folder for disk images
      File entry;
      while ((entry = gameDir.openNextFile())) {
        String fname = entry.name();
        int slash = fname.lastIndexOf('/');
        if (slash >= 0) fname = fname.substring(slash + 1);

        String upper = fname;
        upper.toUpperCase();
        if (upper.endsWith(ext1) || upper.endsWith(".IMG")) {
          // Store full path
          String fullPath = gameDirPath + "/" + fname;
          if (!fullPath.startsWith("/")) fullPath = "/" + fullPath;
          images.push_back(fullPath);
        }
        entry.close();
      }
      gameDir.close();
    }
    root.close();
  }

  // Fallback: also scan root for flat layout (legacy compatibility)
  File rootDir = SD_MMC.open("/");
  if (rootDir) {
    File entry;
    while ((entry = rootDir.openNextFile())) {
      if (entry.isDirectory()) { entry.close(); continue; }
      String fname = entry.name();
      int slash = fname.lastIndexOf('/');
      if (slash >= 0) fname = fname.substring(slash + 1);
      String upper = fname;
      upper.toUpperCase();
      if (upper.endsWith(ext1) || upper.endsWith(".IMG")) {
        String fullPath = "/" + fname;
        // Avoid duplicates
        bool dup = false;
        for (const auto &existing : images) {
          if (existing == fullPath) { dup = true; break; }
        }
        if (!dup) images.push_back(fullPath);
      }
      entry.close();
    }
    rootDir.close();
  }

  sort(images.begin(), images.end());
  return images;
}

// Forward declaration (used by findNFOFor/findJPGFor before definition)
String getGameBaseName(const String &fullPath);
void handleTap(uint16_t px, uint16_t py);
void handleSwipe(int16_t dx, int16_t dy, uint16_t startX, uint16_t startY);

// Get the parent directory of a file path
String parentDir(const String &path) {
  int slash = path.lastIndexOf('/');
  if (slash <= 0) return "/";
  return path.substring(0, slash);
}

// Get just the filename from a full path
String filenameOnly(const String &path) {
  int slash = path.lastIndexOf('/');
  if (slash < 0) return path;
  return path.substring(slash + 1);
}

// Find the .nfo file for a given disk image (full path).
// Looks in the same directory as the image file.
// For multi-disk games also tries the game base name.
String findNFOFor(const String &imagePath) {
  String dir = parentDir(imagePath);
  String base = filenameOnly(imagePath);
  base = base.substring(0, base.lastIndexOf('.'));

  // Try <base>.nfo in the same folder
  String result = findFileInDir(dir, base + ".nfo");
  if (result.length() > 0) return result;

  // For multi-disk files, try the game base name
  String gameName = getGameBaseName(imagePath);
  if (gameName != base) {
    result = findFileInDir(dir, gameName + ".nfo");
    if (result.length() > 0) return result;
  }

  // Also try just "info.nfo" or "readme.nfo"
  result = findFileInDir(dir, "info.nfo");
  if (result.length() > 0) return result;
  return "";
}

// Find the cover image for a given disk image (full path).
// Looks in the same directory as the image file.
// For multi-disk games (GameName-1.adf) also tries the base game name (GameName.jpg).
String findJPGFor(const String &imagePath) {
  String dir = parentDir(imagePath);
  String base = filenameOnly(imagePath);
  base = base.substring(0, base.lastIndexOf('.'));

  // Try <base>.jpg, .jpeg, .png in the same folder
  for (const char *ext : {".jpg", ".jpeg", ".png"}) {
    String result = findFileInDir(dir, base + ext);
    if (result.length() > 0) return result;
  }

  // For multi-disk files, try the game base name (without -N suffix)
  String gameName = getGameBaseName(imagePath);
  if (gameName != base) {
    for (const char *ext : {".jpg", ".jpeg", ".png"}) {
      String result = findFileInDir(dir, gameName + ext);
      if (result.length() > 0) return result;
    }
  }

  // Also try generic names
  for (const char *name : {"cover.jpg", "cover.png", "art.jpg"}) {
    String result = findFileInDir(dir, name);
    if (result.length() > 0) return result;
  }
  return "";
}

// Find banner image (in root or mode folder)
String findBannerImage() {
  // First try theme-specific banner
  String result = findFileInDir(theme_path, "BANNER.JPG");
  if (result.length() > 0) return result;
  result = findFileInDir(theme_path, "BANNER_ADF.JPG");
  if (result.length() > 0) return result;
  // Fallback to global banner
  result = findFileInDir("/", "BANNER.JPG");
  if (result.length() > 0) return result;
  result = findFileInDir("/ADF", "BANNER.JPG");
  return result;
}

String findDSKBanner() {
  // First try theme-specific DSK banner
  String result = findFileInDir(theme_path, "DSKBANNER.JPG");
  if (result.length() > 0) return result;
  result = findFileInDir(theme_path, "BANNER_DSK.JPG");
  if (result.length() > 0) return result;
  result = findFileInDir(theme_path, "BANNER.JPG");
  if (result.length() > 0) return result;
  // Fallback to global banner
  result = findFileInDir("/", "DSKBANNER.JPG");
  if (result.length() > 0) return result;
  result = findFileInDir("/DSK", "BANNER.JPG");
  return result;
}

String readSmallTextFile(const char *path, int maxSize = 2048) {
  File f = SD_MMC.open(path, "r");
  if (!f) return "";

  String result = "";
  while (f.available() && result.length() < maxSize) {
    result += (char)f.read();
  }
  f.close();
  return result;
}

// ============================================================================
// NFO PARSING
// ============================================================================

void parseNFO(const String &nfoText, String &title, String &blurb) {
  title = "";
  blurb = "";

  int lines = 0;
  int pos = 0;
  while (pos < (int)nfoText.length() && lines < 2) {
    int eol = nfoText.indexOf('\n', pos);
    if (eol < 0) eol = nfoText.length();
    String line = nfoText.substring(pos, eol);
    line.trim();
    if (line.length() > 0) {
      if (lines == 0) {
        title = line;
      } else {
        blurb = line;
      }
      lines++;
    }
    pos = eol + 1;
  }
}

String basenameNoExt(const String &path) {
  int lastSlash = path.lastIndexOf('/');
  int lastDot = path.lastIndexOf('.');
  if (lastSlash < 0) lastSlash = -1;
  if (lastDot < 0) lastDot = path.length();
  return path.substring(lastSlash + 1, lastDot);
}

// Multi-disk helpers: detect "GameName-1.adf", "GameName-2.adf" series
String getGameBaseName(const String &fullPath) {
  String base = basenameNoExt(filenameOnly(fullPath));
  // Strip trailing "-N" if present (e.g., "MonkeyIsland-1" -> "MonkeyIsland")
  int dash = base.lastIndexOf('-');
  if (dash > 0 && dash < (int)base.length() - 1) {
    String suffix = base.substring(dash + 1);
    bool isNum = true;
    for (int i = 0; i < (int)suffix.length(); i++) {
      if (!isDigit(suffix[i])) { isNum = false; break; }
    }
    if (isNum) return base.substring(0, dash);
  }
  return base;
}

int getDiskNumber(const String &fullPath) {
  String base = basenameNoExt(filenameOnly(fullPath));
  int dash = base.lastIndexOf('-');
  if (dash > 0 && dash < (int)base.length() - 1) {
    String suffix = base.substring(dash + 1);
    int num = suffix.toInt();
    if (num > 0) return num;
  }
  return 0;  // not part of a numbered set
}

// Find all disks belonging to the same game, in the same folder
void findRelatedDisks(int currentIndex) {
  disk_set.clear();
  if (currentIndex < 0 || currentIndex >= (int)file_list.size()) return;

  String baseName = getGameBaseName(file_list[currentIndex]);
  String dir = parentDir(file_list[currentIndex]);

  for (int i = 0; i < (int)file_list.size(); i++) {
    if (parentDir(file_list[i]) == dir &&
        getGameBaseName(file_list[i]) == baseName &&
        getDiskNumber(file_list[i]) > 0) {
      disk_set.push_back(i);
    }
  }
  // Sort by disk number
  for (int i = 0; i < (int)disk_set.size(); i++) {
    for (int j = i + 1; j < (int)disk_set.size(); j++) {
      if (getDiskNumber(file_list[disk_set[j]]) < getDiskNumber(file_list[disk_set[i]])) {
        swap(disk_set[i], disk_set[j]);
      }
    }
  }
}

String getOutputFilename() {
  if (selected_index >= 0 && selected_index < (int)file_list.size()) {
    return filenameOnly(file_list[selected_index]);
  }
  return (g_mode == MODE_ADF) ? "DEFAULT.ADF" : "DEFAULT.DSK";
}

// ============================================================================
// FAT12 FILESYSTEM EMULATION
// ============================================================================

void build_boot_sector(uint8_t *buf) {
  memset(buf, 0, 512);
  buf[0x00] = 0xEB; buf[0x01] = 0x3C; buf[0x02] = 0x90;
  memcpy(&buf[0x03], "MSDOS5.0", 8);
  *(uint16_t *)&buf[0x0B] = 512;
  buf[0x0D] = 1;
  *(uint16_t *)&buf[0x0E] = 1;
  buf[0x10] = 2;
  *(uint16_t *)&buf[0x11] = 224;
  *(uint16_t *)&buf[0x13] = 2880;
  buf[0x15] = 0xF0;
  *(uint16_t *)&buf[0x16] = 9;
  *(uint16_t *)&buf[0x18] = 18;
  *(uint16_t *)&buf[0x1A] = 2;
  *(uint32_t *)&buf[0x20] = 0;
  buf[0x24] = 0x00;  // BS_DrvNum: 0x00 = floppy
  buf[0x25] = 0x00;  // BS_Reserved1
  buf[0x26] = 0x29;  // BS_BootSig: marks volume label & FS type as valid
  buf[0x27] = 0x47;  // BS_VolID (serial number bytes)
  buf[0x28] = 0x4F;
  buf[0x29] = 0x54;
  buf[0x2A] = 0x4B;
  memcpy(&buf[0x2B], "GOTEK      ", 11);
  memcpy(&buf[0x36], "FAT12   ", 8);
  buf[510] = 0x55;
  buf[511] = 0xAA;
}

void fat12_set(uint8_t *fat, int idx, uint16_t val) {
  if (idx % 2 == 0) {
    fat[idx * 3 / 2] = val & 0xFF;
    fat[idx * 3 / 2 + 1] = (fat[idx * 3 / 2 + 1] & 0xF0) | ((val >> 8) & 0x0F);
  } else {
    fat[idx * 3 / 2] = (fat[idx * 3 / 2] & 0x0F) | ((val & 0x0F) << 4);
    fat[idx * 3 / 2 + 1] = (val >> 4) & 0xFF;
  }
}

void build_fat(uint8_t *fat) {
  memset(fat, 0, 4608);
  fat12_set(fat, 0, 0xFF0);
  fat12_set(fat, 1, 0xFFF);
  // Cluster 2+ left as 0x000 (free) until a file is loaded
}

void make_83_name(const char *src, uint8_t *dst) {
  memset(dst, ' ', 11);
  // Find last dot for extension
  const char *dot = strrchr(src, '.');
  int nameLen = dot ? (int)(dot - src) : (int)strlen(src);
  // Copy name part (max 8 chars)
  for (int i = 0, j = 0; i < nameLen && j < 8; i++) {
    dst[j++] = toupper(src[i]);
  }
  // Copy extension (max 3 chars)
  if (dot) {
    dot++;
    for (int j = 8; *dot && j < 11; dot++) {
      dst[j++] = toupper(*dot);
    }
  }
}

void build_root(uint8_t *root) {
  memset(root, 0, 7168); // 14 sectors × 512 = 7168 bytes for 224 entries
  uint8_t fname[11];
  make_83_name(getOutputFilename().c_str(), fname);
  memcpy(&root[0], fname, 11);
  root[11] = 0x20;             // Archive attribute
  *(uint16_t *)&root[26] = 0;  // Start cluster = 0 (no data yet)
  *(uint32_t *)&root[28] = 0;  // File size = 0
}

// FAT12 layout for 1.44MB floppy (2880 sectors):
// Sector 0:      Boot sector          → offset 0
// Sectors 1-9:   FAT1 (9 sectors)     → offset 512
// Sectors 10-18: FAT2 (9 sectors)     → offset 5120
// Sectors 19-32: Root dir (14 sectors) → offset 9728
// Sectors 33+:   Data area            → offset 16896
#define FAT1_OFFSET   512
#define FAT2_OFFSET   5120
#define ROOTDIR_OFFSET 9728
#define DATA_OFFSET   16896

void build_volume_with_file() {
  memset(ram_disk, 0, RAM_DISK_SIZE);
  build_boot_sector(&ram_disk[0]);
  build_fat(&ram_disk[FAT1_OFFSET]);
  build_fat(&ram_disk[FAT2_OFFSET]);
  build_root(&ram_disk[ROOTDIR_OFFSET]);

  msc_block_count = RAM_DISK_SIZE / 512;
}

// ============================================================================
// USB MSC CALLBACKS
// ============================================================================

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  uint32_t addr = lba * 512 + offset;
  if (ram_disk && addr + bufsize <= RAM_DISK_SIZE) {
    memcpy(buffer, &ram_disk[addr], bufsize);
    return bufsize;
  }
  return -1;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  uint32_t addr = lba * 512 + offset;
  if (ram_disk && addr + bufsize <= RAM_DISK_SIZE) {
    memcpy(&ram_disk[addr], buffer, bufsize);
    return bufsize;
  }
  return -1;
}

// ============================================================================
// UI FUNCTIONS
// ============================================================================

void uiInit() {
  displayInit();
}

void uiClr() {
  gfx_fillScreen(TFT_BLACK);
}

// ── Amiga Workbench 2.x theme colors ──
// These are used when no PNG theme images are present
#define WB_GREY      0xAD55   // RGB565 for (170,170,170)
#define WB_BLUE      0x02B5   // RGB565 for (0,85,170)
#define WB_LIGHT     0xFFFF   // white highlight
#define WB_MED_GREY  0x7BEF   // medium grey shadow
#define WB_ORANGE    0xFC40   // RGB565 for (255,136,0)

void uiLine(int y, uint16_t c) {
  gfx_fillRect(0, y, gW, 1, c);
}

// Draw an Amiga WB 2.x style title bar with 3D bevel
void drawThemedHeader(const String &title) {
  // Try header PNG from active theme folder (use stack buffer to avoid String alloc issues)
  char headerPath[64];
  snprintf(headerPath, sizeof(headerPath), "%s/HEADER.png", theme_path.c_str());
  Serial.print("Loading header: ");
  Serial.println(headerPath);

  if (drawPngFile(headerPath, 0, 0)) {
    Serial.println("  -> PNG OK");
    // Draw title text on top of the PNG header
    text_transparent = true;
    gfx_setTextColor(TFT_WHITE, TFT_BLACK);
    gfx_setTextSize(2);
    gfx_setCursor(10, 15);
    gfx_print(title);
    text_transparent = false;
    return;
  }
  Serial.println("  -> PNG FAILED, using fallback");

  // Fallback: Amiga WB 2.x style drawn header
  // Main bar in Amiga blue
  gfx_fillRect(0, 0, gW, 50, WB_BLUE);

  // Top highlight line (white)
  gfx_fillRect(0, 0, gW, 1, WB_LIGHT);
  // Left highlight
  gfx_fillRect(0, 0, 1, 50, WB_LIGHT);
  // Bottom shadow
  gfx_fillRect(0, 49, gW, 1, WB_MED_GREY);
  // Right shadow
  gfx_fillRect(gW - 1, 0, 1, 50, WB_MED_GREY);

  // Amiga-style depth gadget lines (horizontal stripes in title bar)
  for (int sy = 4; sy < 46; sy += 4) {
    gfx_fillRect(8, sy, gW - 16, 1, 0x0196);  // slightly darker blue stripe
  }

  // Title text with dark background block (Amiga style: text in a box)
  gfx_setTextSize(2);
  int tw = gfx_textWidth(title);
  int tx = 10;
  int ty = 15;
  // Draw a filled rect behind the text (classic Amiga title text block)
  gfx_fillRect(tx - 4, ty - 4, tw + 8, 22, WB_BLUE);
  gfx_setTextColor(TFT_WHITE, WB_BLUE);
  gfx_setCursor(tx, ty);
  gfx_print(title);

  // Separator line below header
  gfx_fillRect(0, 50, gW, 2, WB_ORANGE);
  gfx_fillRect(0, 52, gW, 1, WB_MED_GREY);
}

// Draw Amiga-style loading screen with themed progress bar
void drawThemedLoadingScreen(const String &filename) {
  gfx_fillScreen(TFT_BLACK);

  // Title
  gfx_setTextColor(TFT_CYAN, TFT_BLACK);
  gfx_setTextSize(2);
  String lt = "LOADING";
  gfx_setCursor((gW - gfx_textWidth(lt)) / 2, 50);
  gfx_print(lt);

  // Filename
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  gfx_setTextSize(2);
  int fnW = gfx_textWidth(filename);
  gfx_setCursor((gW - fnW) / 2, 80);
  gfx_print(filename);

  // Reading indicator
  gfx_setTextColor(WB_ORANGE, TFT_BLACK);
  gfx_setTextSize(2);
  gfx_setCursor((gW - gfx_textWidth("[ READING DISK ]")) / 2, 110);
  gfx_print("[ READING DISK ]");

  // Progress bar frame (Amiga 3D bevel)
  int barX = 40, barY = 160, barW = gW - 80, barH = 26;
  // Outer shadow (dark bottom-right)
  gfx_fillRect(barX + 1, barY + barH, barW, 1, WB_MED_GREY);
  gfx_fillRect(barX + barW, barY + 1, 1, barH, WB_MED_GREY);
  // Outer highlight (white top-left)
  gfx_fillRect(barX, barY, barW, 1, WB_LIGHT);
  gfx_fillRect(barX, barY, 1, barH, WB_LIGHT);
  // Inner recessed area
  gfx_fillRect(barX + 1, barY + 1, barW - 2, barH - 2, 0x1082); // very dark grey

  gfx_flush();
}

// Update the progress bar fill during loading
void drawThemedProgressBar(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  int barX = 40, barY = 160, barW = gW - 80, barH = 26;
  int innerW = barW - 4;
  int fillW = (innerW * pct) / 100;
  if (fillW > innerW) fillW = innerW;  // clamp to prevent overflow
  if (fillW > 0) {
    // Amiga-style green fill with slight gradient
    gfx_fillRect(barX + 2, barY + 2, fillW, barH - 4, 0x07E0); // bright green
    // Highlight on top of fill
    gfx_fillRect(barX + 2, barY + 2, fillW, 1, 0x47E8);  // lighter green
  }
  // Percentage text
  gfx_setTextColor(TFT_WHITE, 0x1082);
  gfx_setTextSize(2);
  String pctStr = String(pct) + "%";
  int tw = gfx_textWidth(pctStr);
  gfx_setCursor(barX + (barW - tw) / 2, barY + 5);
  gfx_print(pctStr);
  gfx_flush();
}

void uiSection(int x, int y, const String &title, uint16_t color) {
  gfx_setTextColor(color, TFT_BLACK);
  gfx_setTextSize(1);
  gfx_setCursor(x, y);
  gfx_print(title);
}

void uiOK(int x, int y) {
  gfx_setTextColor(TFT_GREEN, TFT_BLACK);
  gfx_setTextSize(1);
  gfx_setCursor(x, y);
  gfx_print("[OK]");
}

void uiERR(int x, int y, const String &msg) {
  gfx_setTextColor(TFT_RED, TFT_BLACK);
  gfx_setTextSize(1);
  gfx_setCursor(x, y);
  gfx_print(msg);
}

void drawWrappedText(const String &text, int x, int y, int maxWidth, uint16_t color) {
  gfx_setTextColor(color, TFT_BLACK);
  gfx_setTextSize(1);

  int lineY = y;
  String line = "";
  for (char c : text) {
    if (c == '\n') {
      gfx_setCursor(x, lineY);
      gfx_print(line);
      line = "";
      lineY += 10;
    } else {
      line += c;
      if (gfx_textWidth(line) >= maxWidth) {
        gfx_setCursor(x, lineY);
        gfx_print(line);
        line = "";
        lineY += 10;
      }
    }
  }
  if (line.length() > 0) {
    gfx_setCursor(x, lineY);
    gfx_print(line);
  }
}

void drawWrappedTextBG(const String &text, int x, int y, int maxWidth,
                       uint16_t fg, uint16_t bg) {
  gfx_setTextColor(fg, bg);
  gfx_setTextSize(1);

  int lineY = y;
  String line = "";
  for (char c : text) {
    if (c == '\n') {
      int w = gfx_textWidth(line);
      gfx_fillRect(x, lineY, w, 8, bg);
      gfx_setCursor(x, lineY);
      gfx_print(line);
      line = "";
      lineY += 10;
    } else {
      line += c;
      if (gfx_textWidth(line) >= maxWidth) {
        int w = gfx_textWidth(line);
        gfx_fillRect(x, lineY, w, 8, bg);
        gfx_setCursor(x, lineY);
        gfx_print(line);
        line = "";
        lineY += 10;
      }
    }
  }
  if (line.length() > 0) {
    int w = gfx_textWidth(line);
    gfx_fillRect(x, lineY, w, 8, bg);
    gfx_setCursor(x, lineY);
    gfx_print(line);
  }
}

// ============================================================================
// THEME SYSTEM — Load button images from /THEMES/<name>/ on SD card
// ============================================================================
// Theme system — PNG buttons with alpha blending.
// Folder structure: /THEMES/<theme_name>/
//   HEADER.png     — title bar background (480x53)
//   BTN_SELECT.png, BTN_OPEN.png, BTN_INFO.png   — list page
//   BTN_BACK.png,   BTN_LOAD.png, BTN_UNLOAD.png — detail page
//   BTN_ADF.png,    BTN_DSK.png                   — mode switch
//   RAMDISK.png                                    — indicator
//
// Active theme is set via THEME= in CONFIG.TXT or via INFO screen.
// PNG files can be 32-bit RGBA (with transparency) or 24-bit RGB.
// If a PNG is missing, a simple drawn fallback button is used.
// ============================================================================

static PNG png;
static int png_draw_x = 0;   // top-left X for current PNG draw
static int png_draw_y = 0;   // top-left Y for current PNG draw

// Alpha-blend a single pixel: fg over bg
static inline uint16_t alphaBlend565(uint16_t bg, uint16_t fg, uint8_t alpha) {
  if (alpha == 255) return fg;
  if (alpha == 0)   return bg;
  uint8_t bgR = (bg >> 11) & 0x1F;
  uint8_t bgG = (bg >> 5)  & 0x3F;
  uint8_t bgB =  bg        & 0x1F;
  uint8_t fgR = (fg >> 11) & 0x1F;
  uint8_t fgG = (fg >> 5)  & 0x3F;
  uint8_t fgB =  fg        & 0x1F;
  uint8_t inv = 255 - alpha;
  uint8_t rr = (fgR * alpha + bgR * inv + 127) / 255;
  uint8_t gg = (fgG * alpha + bgG * inv + 127) / 255;
  uint8_t bb = (fgB * alpha + bgB * inv + 127) / 255;
  return (rr << 11) | (gg << 5) | bb;
}

// Does the current PNG have alpha? (set after png.open)
static bool png_has_alpha = false;

// PNGdec draw callback — called for each scanline of decoded pixels.
// Must return 1 to continue decoding, 0 to abort.
int pngDrawCB(PNGDRAW *pDraw) {
  uint16_t lineBuffer[500];
  int w = pDraw->iWidth;
  if (w > 500) w = 500;

  // Convert to native-endian RGB565 (our gfx functions handle byte-swap)
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);

  int drawY = png_draw_y + pDraw->y;
  if (drawY < 0 || drawY >= gH) return 1;

  if (png_has_alpha) {
    // Use PNGdec's alpha mask (1-bit per pixel, threshold 128)
    uint8_t alphaMask[64];  // ceil(500/8) = 63 bytes
    png.getAlphaMask(pDraw, alphaMask, 128);

    for (int i = 0; i < w; i++) {
      int byteIdx = i >> 3;
      int bitIdx  = 7 - (i & 7);
      if (!(alphaMask[byteIdx] & (1 << bitIdx))) continue;  // transparent pixel
      int drawX = png_draw_x + i;
      if (drawX < 0 || drawX >= gW) continue;
      gfx_drawPixel(drawX, drawY, lineBuffer[i]);
    }
  } else {
    // No alpha: draw all pixels directly
    for (int i = 0; i < w; i++) {
      int drawX = png_draw_x + i;
      if (drawX < 0 || drawX >= gW) continue;
      gfx_drawPixel(drawX, drawY, lineBuffer[i]);
    }
  }
  return 1;
}

// File I/O callbacks for PNGdec (reads from SD via File object)
static File pngFile;

void *pngOpen(const char *filename, int32_t *pFileSize) {
  pngFile.close();  // close any previously open file
  pngFile = SD_MMC.open(filename, "r");
  if (!pngFile) return NULL;
  *pFileSize = pngFile.size();
  return &pngFile;
}
void pngClose(void *pHandle) {
  if (pHandle) ((File *)pHandle)->close();
}
int32_t pngRead(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  return pngFile.read(pBuf, iLen);
}
int32_t pngSeek(PNGFILE *pFile, int32_t iPosition) {
  return pngFile.seek(iPosition);
}

// Draw a PNG from SD at position (x,y) with alpha blending.
// Returns true if drawn, false if file missing or invalid.
bool drawPngFile(const char *path, int x, int y) {
  int rc = png.open(path, pngOpen, pngClose, pngRead, pngSeek, pngDrawCB);
  if (rc != PNG_SUCCESS) {
    Serial.print("PNG open FAIL: ");
    Serial.print(path);
    Serial.print(" rc=");
    Serial.println(rc);
    return false;
  }

  png_draw_x = x;
  png_draw_y = y;
  png_has_alpha = png.hasAlpha();
  rc = png.decode(NULL, 0);
  png.close();
  if (rc != PNG_SUCCESS) {
    Serial.print("PNG decode FAIL: ");
    Serial.print(path);
    Serial.print(" rc=");
    Serial.println(rc);
  }
  return (rc == PNG_SUCCESS);
}

// Get dimensions of a PNG file without drawing it.
bool getPngSize(const char *path, int *w, int *h) {
  int rc = png.open(path, pngOpen, pngClose, pngRead, pngSeek, pngDrawCB);
  if (rc != PNG_SUCCESS) return false;
  *w = png.getWidth();
  *h = png.getHeight();
  png.close();
  return (*w > 0 && *h > 0);
}

// Draw a themed button: try PNG from /THEME/, fallback to simple rect+text.
void drawThemedButton(int x, int y, int w, int h,
                      const char *pngName, const char *label,
                      uint16_t borderColor) {
  String path = theme_path + "/" + String(pngName) + ".png";

  int imgW = 0, imgH = 0;
  if (getPngSize(path.c_str(), &imgW, &imgH)) {
    // Center the PNG within the button area
    int bx = x + (w - imgW) / 2;
    int by = y + (h - imgH) / 2;
    drawPngFile(path.c_str(), bx, by);
  } else {
    // Fallback: simple rectangle with text
    gfx_drawRect(x, y, w, h, borderColor);
    gfx_setTextColor(borderColor, TFT_BLACK);
    gfx_setTextSize(2);
    int tw = gfx_textWidth(String(label));
    int th = gfx_fontHeight();
    gfx_setCursor(x + (w - tw) / 2, y + (h - th) / 2);
    gfx_print(String(label));
  }
}

// Convenience: draw mode switch button (ADF or DSK)
void drawModeSwitchButton() {
  int w = 110;
  int h = 36;
  int x = gW - w - 10;
  int y = gH - 42;
  if (g_mode == MODE_ADF) {
    drawThemedButton(x, y, w, h, "BTN_ADF", "ADF", TFT_CYAN);
  } else {
    drawThemedButton(x, y, w, h, "BTN_DSK", "DSK", TFT_CYAN);
  }
}

void drawRamdiskIndicatorCircle() {
  // Try theme PNG (with possible transparency), fallback to yellow circle
  if (!drawPngFile((theme_path + "/RAMDISK.png").c_str(), gW - 30, 5)) {
    gfx_fillCircle(gW - 15, 15, 8, TFT_YELLOW);
  }
}

void drawButton(int x, int y, int w, int h, const String &label) {
  gfx_drawRect(x, y, w, h, TFT_GREY);
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  gfx_setTextSize(1);
  int textX = x + (w - gfx_textWidth(label)) / 2;
  int textY = y + (h - 8) / 2;
  gfx_setCursor(textX, textY);
  gfx_print(label);
}

// ============================================================================
// Cracktro splash screen (Amiga demoscene style)
// ============================================================================

// Simple starfield: pre-generate star positions
#define NUM_STARS 60
int16_t star_x[NUM_STARS], star_y[NUM_STARS], star_speed[NUM_STARS];

void initStars() {
  for (int i = 0; i < NUM_STARS; i++) {
    star_x[i] = random(0, gW);
    star_y[i] = random(0, gH);
    star_speed[i] = random(1, 4);
  }
}

void drawCracktroSplash() {
  initStars();

  // Scroll text — classic cracktro message
  const char *scrollText =
    "       GOTEK TOUCHSCREEN INTERFACE  ...  "
    "THE ULTIMATE RETRO DISK LOADER FOR AMIGA AND CPC  ...  "
    "ORIGINAL CODE BY DIMMY  ...  "
    "ACTIVE THEME ENGINE - PNG BUTTON SUPPORT - FAT12 RAM DISK  ...  "
    "GREETINGS TO ALL RETRO COMPUTING ENTHUSIASTS!  ...       ";
  int scrollLen = strlen(scrollText);
  int scrollPos = 0;
  int charW = 12;  // textSize 2 = 12px per char

  // Copper bar colors (rainbow gradient in RGB565)
  uint16_t copperColors[] = {
    0xF800, 0xF920, 0xFAA0, 0xFC00, 0xFDE0, 0xEFE0, 0x87E0, 0x07E0,
    0x07F0, 0x07FF, 0x041F, 0x001F, 0x801F, 0xF81F, 0xF810, 0xF800
  };
  int numCopper = 16;

  unsigned long startTime = millis();
  int frame = 0;

  // Run for 4 seconds or until touch
  while (millis() - startTime < 7000) {
    uint16_t tx, ty;
    if (touchRead(&tx, &ty)) break;  // skip on touch

    gfx_fillScreen(TFT_BLACK);

    // ── Starfield ──
    for (int i = 0; i < NUM_STARS; i++) {
      uint16_t col;
      if (star_speed[i] == 3) col = TFT_WHITE;
      else if (star_speed[i] == 2) col = TFT_GREY;
      else col = TFT_DARKGREY;
      gfx_drawPixel(star_x[i], star_y[i], col);
      star_x[i] -= star_speed[i];
      if (star_x[i] < 0) {
        star_x[i] = gW - 1;
        star_y[i] = random(0, gH);
      }
    }

    // ── Copper bars (raster bars) — sinusoidal bounce ──
    int copperY = 80 + (int)(40.0 * sin((float)frame * 0.08));
    for (int i = 0; i < numCopper; i++) {
      int barY = copperY + i * 3;
      if (barY >= 0 && barY < gH) {
        gfx_fillRect(0, barY, gW, 2, copperColors[i]);
      }
    }

    // ── Title text (big, centered, over copper bars) ──
    text_transparent = true;
    gfx_setTextSize(3);
    gfx_setTextColor(TFT_WHITE, TFT_BLACK);
    String title = "GOTEK";
    int tw = gfx_textWidth(title);
    gfx_setCursor((gW - tw) / 2, copperY + 6);
    gfx_print(title);

    gfx_setTextSize(2);
    String sub = "TOUCHSCREEN";
    tw = gfx_textWidth(sub);
    gfx_setCursor((gW - tw) / 2, copperY + 32);
    gfx_print(sub);
    text_transparent = false;

    // ── Scroll text (bottom) ──
    gfx_fillRect(0, gH - 30, gW, 24, 0x0010);  // dark blue bar
    gfx_setTextSize(2);
    gfx_setTextColor(TFT_YELLOW, 0x0010);

    int drawX = gW - (scrollPos % (scrollLen * charW));
    // Draw enough characters to fill screen
    int startChar = scrollPos / charW;
    int pixOffset = scrollPos % charW;
    gfx_setCursor(-pixOffset, gH - 26);
    for (int c = 0; c < (gW / charW) + 2; c++) {
      int idx = (startChar + c) % scrollLen;
      char buf[2] = { scrollText[idx], 0 };
      gfx_print(String(buf));
    }

    scrollPos += 3;  // scroll speed (pixels per frame)
    frame++;

    gfx_flush();
    delay(30);  // ~33 fps
  }

  // Fade out: quick flash
  gfx_fillScreen(TFT_WHITE);
  gfx_flush();
  delay(50);
  gfx_fillScreen(TFT_BLACK);
  gfx_flush();
}

// ============================================================================
// Boot loading screen (shown during SD scan and init)
// ============================================================================
void drawBootProgress(const String &status, int pct) {
  // Don't clear full screen — just update status area
  // Bar area: y=180..210
  int barX = 60, barY = 190, barW = gW - 120, barH = 16;

  // Status text
  gfx_fillRect(0, 160, gW, 20, TFT_BLACK);  // clear old text
  gfx_setTextSize(1);
  gfx_setTextColor(TFT_GREY, TFT_BLACK);
  int sw = gfx_textWidth(status);
  gfx_setCursor((gW - sw) / 2, 165);
  gfx_print(status);

  // Progress bar
  gfx_drawRect(barX, barY, barW, barH, TFT_GREY);
  int fillW = ((barW - 4) * pct) / 100;
  if (fillW > 0) {
    gfx_fillRect(barX + 2, barY + 2, fillW, barH - 4, TFT_CYAN);
  }

  gfx_flush();
}

void drawBootScreen() {
  gfx_fillScreen(TFT_BLACK);

  // Logo text
  gfx_setTextSize(3);
  gfx_setTextColor(TFT_CYAN, TFT_BLACK);
  String t = "GOTEK";
  gfx_setCursor((gW - gfx_textWidth(t)) / 2, 80);
  gfx_print(t);

  gfx_setTextSize(2);
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  t = "Touchscreen Interface";
  gfx_setCursor((gW - gfx_textWidth(t)) / 2, 120);
  gfx_print(t);

  gfx_flush();
}

// ============================================================================
// Button press feedback — spinner/loading overlay
// ============================================================================
bool ui_busy = false;  // when true, touch input is ignored

void showBusyIndicator(const String &msg = "LOADING...") {
  ui_busy = true;

  // Semi-transparent overlay: dark bar across the middle
  int boxW = 200, boxH = 50;
  int bx = (gW - boxW) / 2;
  int by = (gH - boxH) / 2;

  // Dark box with border
  gfx_fillRect(bx, by, boxW, boxH, TFT_BLACK);
  gfx_drawRect(bx, by, boxW, boxH, TFT_CYAN);
  gfx_drawRect(bx + 1, by + 1, boxW - 2, boxH - 2, TFT_DARKGREY);

  // Spinning dots (static frame — just show a loading text with dots)
  gfx_setTextSize(2);
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  int mw = gfx_textWidth(msg);
  gfx_setCursor(bx + (boxW - mw) / 2, by + (boxH - 16) / 2);
  gfx_print(msg);

  gfx_flush();
}

void hideBusyIndicator() {
  ui_busy = false;
}

// ============================================================================
// Info / Status screen
// ============================================================================
void drawInfoScreen() {
  gfx_fillScreen(TFT_BLACK);

  // Title
  gfx_setTextColor(TFT_CYAN, TFT_BLACK);
  gfx_setTextSize(2);
  gfx_setCursor(20, 8);
  gfx_print("SYSTEM INFO");

  int y = 35;
  int lineH = 24;

  gfx_setTextSize(2);

  // --- Free heap ---
  gfx_setTextColor(TFT_GREEN, TFT_BLACK);
  gfx_setCursor(20, y);
  gfx_print("Heap: ");
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  gfx_print(String(ESP.getFreeHeap() / 1024) + " KB free");
  y += lineH;

  // --- Free PSRAM ---
  gfx_setTextColor(TFT_GREEN, TFT_BLACK);
  gfx_setCursor(20, y);
  gfx_print("PSRAM: ");
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  gfx_print(String(ESP.getFreePsram() / 1024) + " KB free");
  y += lineH;

  // --- SD card info ---
  uint64_t totalBytes = SD_MMC.totalBytes();
  uint64_t usedBytes  = SD_MMC.usedBytes();
  gfx_setTextColor(TFT_GREEN, TFT_BLACK);
  gfx_setCursor(20, y);
  gfx_print("SD: ");
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  gfx_print(String((uint32_t)(usedBytes / (1024*1024))) + " / " +
            String((uint32_t)(totalBytes / (1024*1024))) + " MB");
  y += lineH;

  // --- File count ---
  gfx_setTextColor(TFT_GREEN, TFT_BLACK);
  gfx_setCursor(20, y);
  gfx_print("Files: ");
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  gfx_print(String(file_list.size()) + " " +
            String((g_mode == MODE_ADF) ? "ADF" : "DSK"));
  y += lineH;

  // --- Currently loaded file ---
  gfx_setTextColor(TFT_GREEN, TFT_BLACK);
  gfx_setCursor(20, y);
  gfx_print("Loaded: ");
  gfx_setTextColor(TFT_YELLOW, TFT_BLACK);
  if (cfg_lastfile.length() > 0) {
    gfx_print(basenameNoExt(filenameOnly(cfg_lastfile)));
  } else {
    gfx_print("(none)");
  }
  y += lineH;

  // --- Display type ---
  gfx_setTextColor(TFT_GREEN, TFT_BLACK);
  gfx_setCursor(20, y);
  gfx_print("Display: ");
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
#if ACTIVE_DISPLAY == DISPLAY_JC3248
  gfx_print("JC3248W535C 480x320");
#elif ACTIVE_DISPLAY == DISPLAY_WAVESHARE
  gfx_print("Waveshare 320x240");
#endif
  y += lineH;

  // --- Active theme ---
  gfx_setTextColor(TFT_GREEN, TFT_BLACK);
  gfx_setCursor(20, y);
  gfx_print("Theme: ");
  gfx_setTextColor(WB_ORANGE, TFT_BLACK);
  gfx_print(cfg_theme);

  // Bottom buttons: BACK + THEME cycle
  drawThemedButton(20, gH - 42, 90, 36, "BTN_BACK", "BACK", TFT_CYAN);
  drawThemedButton(130, gH - 42, 110, 36, "BTN_THEME", "THEME", WB_ORANGE);

  gfx_flush();
}

// List layout constants (no header bar — full screen list)
#define LIST_START_Y   4
#define LIST_ITEM_H    52
#define LIST_THUMB_W   46
#define LIST_THUMB_H   46
#define LIST_BOTTOM    (gH - 48)

int items_per_page() {
  return (LIST_BOTTOM - LIST_START_Y) / LIST_ITEM_H;
}

void drawList() {
  gfx_fillScreen(TFT_BLACK);

  int perPage = items_per_page();

  // Auto-scroll to keep game_selected visible
  if (game_selected < scroll_offset) scroll_offset = game_selected;
  if (game_selected >= scroll_offset + perPage) scroll_offset = game_selected - perPage + 1;

  // Clamp scroll_offset
  if (scroll_offset > (int)game_list.size() - perPage)
    scroll_offset = (int)game_list.size() - perPage;
  if (scroll_offset < 0) scroll_offset = 0;

  // Draw visible game entries
  for (int vi = 0; vi < perPage && (scroll_offset + vi) < (int)game_list.size(); vi++) {
    int gi = scroll_offset + vi;
    const GameEntry &g = game_list[gi];
    int y = LIST_START_Y + vi * LIST_ITEM_H;
    bool isSel = (gi == game_selected);

    // Selection highlight bar
    if (isSel) {
      gfx_fillRect(0, y, gW, LIST_ITEM_H, 0x1082);  // dark highlight
    }

    // Thumbnail (cover art)
    int thumbX = 6;
    int thumbY = y + (LIST_ITEM_H - LIST_THUMB_H) / 2;
    bool thumbDrawn = false;
    if (g.jpg_path.length() > 0) {
      String lp = g.jpg_path;
      lp.toLowerCase();
      if (lp.endsWith(".jpg") || lp.endsWith(".jpeg")) {
        gfx_drawJpgFile(SD_MMC, g.jpg_path.c_str(), thumbX, thumbY, LIST_THUMB_W, LIST_THUMB_H);
        thumbDrawn = true;
      } else if (lp.endsWith(".png")) {
        // PNG — draw scaled via drawPngFile (no scaling, draw at offset)
        thumbDrawn = drawPngFile(g.jpg_path.c_str(), thumbX, thumbY);
      }
    }
    if (!thumbDrawn) {
      // No cover art — draw a placeholder
      gfx_drawRect(thumbX, thumbY, LIST_THUMB_W, LIST_THUMB_H, 0x4208);  // grey border
      gfx_setTextColor(0x4208, TFT_BLACK);
      gfx_setTextSize(1);
      gfx_setCursor(thumbX + 12, thumbY + 18);
      gfx_print("?");
    }

    // Game name
    int textX = thumbX + LIST_THUMB_W + 8;
    gfx_setTextColor(isSel ? TFT_CYAN : TFT_WHITE, isSel ? 0x1082 : TFT_BLACK);
    gfx_setTextSize(2);
    String dispName = truncateToWidth(g.name, gW - textX - 10);
    gfx_setCursor(textX, y + 8);
    gfx_print(dispName);

    // Disk count indicator (if multi-disk)
    if (g.disk_count > 1) {
      gfx_setTextSize(1);
      gfx_setTextColor(TFT_YELLOW, isSel ? 0x1082 : TFT_BLACK);
      gfx_setCursor(textX, y + 30);
      gfx_print(String(g.disk_count) + " disks");
    }

    // Separator line
    gfx_fillRect(6, y + LIST_ITEM_H - 1, gW - 12, 1, 0x2104);
  }

  // Scroll indicators (right edge)
  if ((int)game_list.size() > perPage) {
    // Scrollbar track
    int trackH = LIST_BOTTOM - LIST_START_Y;
    int thumbH = max(10, trackH * perPage / (int)game_list.size());
    int thumbY = LIST_START_Y + (trackH - thumbH) * scroll_offset / max(1, (int)game_list.size() - perPage);
    gfx_fillRect(gW - 4, thumbY, 3, thumbH, 0x4208);

    // UP arrow (if can scroll up)
    if (scroll_offset > 0) {
      gfx_setTextColor(TFT_GREY, TFT_BLACK);
      gfx_setTextSize(2);
      gfx_setCursor(gW - 24, LIST_START_Y + 4);
      gfx_print("^");
    }
    // DOWN arrow (if can scroll down)
    if (scroll_offset < (int)game_list.size() - perPage) {
      gfx_setTextColor(TFT_GREY, TFT_BLACK);
      gfx_setTextSize(2);
      gfx_setCursor(gW - 24, LIST_BOTTOM - 20);
      gfx_print("v");
    }
  }

  // Bottom action buttons
  drawThemedButton(20, gH - 42, 90, 36, "BTN_SELECT", "SELECT", TFT_CYAN);
  drawThemedButton(130, gH - 42, 90, 36, "BTN_OPEN", "OPEN", TFT_GREEN);
  drawThemedButton(240, gH - 42, 90, 36, "BTN_INFO", "INFO", TFT_YELLOW);
  drawModeSwitchButton();  // ADF/DSK toggle (bottom-right)

  gfx_flush();
}

// Draw the disk selector row for multi-disk games.
// diskY = top Y position of the disk button row.
void drawDiskSelector(int diskY) {
  if (disk_set.size() <= 1) return;

  int btnW = 44;
  int btnH = 28;
  int gap = 4;
  int numDisks = disk_set.size();
  int totalW = numDisks * btnW + (numDisks - 1) * gap;
  int startX = (gW - totalW) / 2;  // center the row

  // "DISK:" label before the buttons
  gfx_setTextColor(TFT_GREY, TFT_BLACK);
  gfx_setTextSize(1);
  gfx_setCursor(startX - 35, diskY + 10);
  gfx_print("DISK:");
  startX += 0;  // buttons right after label

  for (int i = 0; i < numDisks; i++) {
    int bx = startX + i * (btnW + gap);
    int diskNum = getDiskNumber(file_list[disk_set[i]]);
    bool isLoaded = (disk_set[i] == loaded_disk_index);

    if (isLoaded) {
      gfx_fillRect(bx, diskY, btnW, btnH, TFT_GREEN);
      gfx_drawRect(bx, diskY, btnW, btnH, TFT_WHITE);
      gfx_setTextColor(TFT_BLACK, TFT_GREEN);
    } else {
      gfx_fillRect(bx, diskY, btnW, btnH, 0x1082);
      gfx_drawRect(bx, diskY, btnW, btnH, TFT_GREY);
      gfx_setTextColor(TFT_WHITE, 0x1082);
    }

    // Number label centered in button (textSize 2 fits: "1" = 12px in 44px)
    gfx_setTextSize(2);
    String label = String(diskNum);
    int tw = gfx_textWidth(label);
    gfx_setCursor(bx + (btnW - tw) / 2, diskY + (btnH - 16) / 2);
    gfx_print(label);
  }
}

void drawDetailsFromNFO(const String &filename) {
  gfx_fillScreen(TFT_BLACK);

  // Detect multi-disk set
  findRelatedDisks(selected_index);
  bool multiDisk = (disk_set.size() > 1);

  detail_nfo_text = "";
  detail_jpg_path = "";
  String nfoPath = findNFOFor(filename);
  if (nfoPath.length() > 0) {
    detail_nfo_text = readSmallTextFile(nfoPath.c_str(), 500);
  }
  detail_jpg_path = findJPGFor(filename);

  // Layout: no header — full screen for content
  int diskRowH = multiDisk ? 34 : 0;
  int btnTop = gH - 42;
  int diskTop = multiDisk ? (btnTop - diskRowH) : btnTop;
  int contentBottom = diskTop - 3;

  // Game title at top
  String gameTitle = multiDisk ? getGameBaseName(filename) : basenameNoExt(filenameOnly(filename));
  gfx_setTextColor(TFT_CYAN, TFT_BLACK);
  gfx_setTextSize(2);
  String dispTitle = truncateToWidth(gameTitle, gW - 60);
  gfx_setCursor((gW - gfx_textWidth(dispTitle)) / 2, 6);
  gfx_print(dispTitle);

  // Cover art (bigger — from y=28 to contentBottom minus text space)
  int imgTop = 28;
  int imgW = gW - 60;  // leave room for nav arrows

  String title = "", blurb = "";
  parseNFO(detail_nfo_text, title, blurb);
  int textSpace = 0;
  if (title.length() > 0) textSpace += 18;
  if (blurb.length() > 0) textSpace += 14;

  int imgH = contentBottom - imgTop - textSpace - 4;
  if (imgH < 60) imgH = 60;

  if (detail_jpg_path.length() > 0) {
    gfx_drawJpgFile(SD_MMC, detail_jpg_path.c_str(),
                   (gW - imgW) / 2, imgTop, imgW, imgH);
  }

  int textY = imgTop + imgH + 3;
  if (title.length() > 0) {
    gfx_setTextColor(TFT_YELLOW, TFT_BLACK);
    gfx_setTextSize(2);
    gfx_setCursor(20, textY);
    gfx_print(truncateToWidth(title, gW - 40));
    textY += 18;
  }
  if (blurb.length() > 0) {
    gfx_setTextSize(1);
    drawWrappedText(blurb, 20, textY, gW - 40, TFT_WHITE);
  }

  // Disk selector row (if multi-disk)
  if (multiDisk) {
    drawDiskSelector(diskTop);
  }

  // Navigation arrows (left/right edges)
  int arrowY = imgTop + imgH / 2 - 8;
  gfx_setTextSize(2);
  if (game_selected > 0) {
    gfx_setTextColor(TFT_GREY, TFT_BLACK);
    gfx_setCursor(4, arrowY);
    gfx_print("<");
  }
  if (game_selected < (int)game_list.size() - 1) {
    gfx_setTextColor(TFT_GREY, TFT_BLACK);
    gfx_setCursor(gW - 16, arrowY);
    gfx_print(">");
  }

  // Bottom action buttons
  drawThemedButton(20, btnTop, 90, 36, "BTN_BACK", "BACK", TFT_CYAN);
  drawThemedButton(130, btnTop, 90, 36, "BTN_LOAD", "LOAD", TFT_GREEN);
  drawThemedButton(240, btnTop, 90, 36, "BTN_UNLOAD", "UNLOAD", TFT_RED);

  gfx_flush();
}

String truncateToWidth(const String &text, int maxWidth) {
  String result = text;
  gfx_setTextSize(2);
  while (gfx_textWidth(result) > maxWidth && result.length() > 0) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

void buildDisplayNames(const vector<String> &files) {
  display_names.clear();
  for (const auto &f : files) {
    // Show the filename without extension and path
    String name = basenameNoExt(filenameOnly(f));
    display_names.push_back(truncateToWidth(name, gW - 40));
  }
}

void sortByDisplay() {
  for (int i = 0; i < (int)file_list.size(); i++) {
    for (int j = i + 1; j < (int)file_list.size(); j++) {
      if (display_names[i].compareTo(display_names[j]) > 0) {
        swap(file_list[i], file_list[j]);
        swap(display_names[i], display_names[j]);
      }
    }
  }
}

// Find the game_list index for a given file_list index
int findGameIndex(int fileIndex) {
  for (int i = 0; i < (int)game_list.size(); i++) {
    // Check if this game entry contains the file
    String baseName = game_list[i].name;
    String dir = parentDir(file_list[fileIndex]);
    String fileBase = getGameBaseName(file_list[fileIndex]);
    if (game_list[i].first_file_index == fileIndex) return i;
    if (game_list[i].disk_count > 1 && fileBase == baseName) return i;
  }
  return 0;
}

// Build the merged game list from file_list.
// Groups multi-disk games (GameName-1.adf, GameName-2.adf) into one entry.
// Also finds cover art (JPG/PNG) for each game.
void buildGameList() {
  game_list.clear();
  game_selected = 0;
  scroll_offset = 0;

  // Track which file_list indices we've already grouped
  vector<bool> used(file_list.size(), false);

  for (int i = 0; i < (int)file_list.size(); i++) {
    if (used[i]) continue;

    String baseName = getGameBaseName(file_list[i]);
    int diskNum = getDiskNumber(file_list[i]);
    String dir = parentDir(file_list[i]);

    GameEntry entry;
    entry.first_file_index = i;
    entry.disk_count = 1;

    if (diskNum > 0) {
      // This is a multi-disk file — find all related disks
      entry.name = baseName;
      int count = 1;
      for (int j = i + 1; j < (int)file_list.size(); j++) {
        if (used[j]) continue;
        if (parentDir(file_list[j]) == dir &&
            getGameBaseName(file_list[j]) == baseName &&
            getDiskNumber(file_list[j]) > 0) {
          used[j] = true;
          count++;
          // Keep first_file_index pointing to disk 1 (lowest number)
          if (getDiskNumber(file_list[j]) < getDiskNumber(file_list[entry.first_file_index])) {
            entry.first_file_index = j;
          }
        }
      }
      entry.disk_count = count;
    } else {
      // Single disk game
      entry.name = basenameNoExt(filenameOnly(file_list[i]));
    }

    used[i] = true;

    // Find cover art — try the game folder
    entry.jpg_path = findJPGFor(file_list[entry.first_file_index]);

    // If multi-disk and no cover found with disk-1 name, try base name
    if (entry.jpg_path.length() == 0 && diskNum > 0) {
      String tryBase = dir + "/" + baseName;
      for (const char *ext : {".jpg", ".jpeg", ".png"}) {
        String tryPath = tryBase + ext;
        if (SD_MMC.exists(tryPath.c_str())) {
          entry.jpg_path = tryPath;
          break;
        }
      }
    }

    game_list.push_back(entry);
  }

  // Sort alphabetically by name
  for (int i = 0; i < (int)game_list.size(); i++) {
    for (int j = i + 1; j < (int)game_list.size(); j++) {
      if (game_list[i].name.compareTo(game_list[j].name) > 0) {
        swap(game_list[i], game_list[j]);
      }
    }
  }
}

// Core file loading: reads SD file into RAM disk, builds FAT chain.
// Returns bytes loaded, or 0 on error. Does NOT touch USB.
size_t loadFileToRam(int index) {
  if (index < 0 || index >= (int)file_list.size()) return 0;

  // file_list contains full paths like "/ADF/Giganoid/Giganoid.adf"
  String filepath = file_list[index];

  // Show themed loading UI
  drawThemedLoadingScreen(basenameNoExt(filenameOnly(file_list[index])));

  // Rebuild FAT volume (empty)
  build_volume_with_file();

  // Read disk image from SD directly into RAM disk data area
  File f = SD_MMC.open(filepath.c_str(), "r");
  if (!f) {
    gfx_setTextColor(TFT_RED, TFT_BLACK);
    gfx_setTextSize(2);
    String errMsg = "Error opening file!";
    gfx_setCursor((gW - gfx_textWidth(errMsg)) / 2, 160);
    gfx_print(errMsg);
    gfx_flush();
    delay(1000);
    return 0;
  }

  size_t fileSize = f.size();
  size_t maxData = RAM_DISK_SIZE - DATA_OFFSET;
  size_t toRead = (fileSize < maxData) ? fileSize : maxData;
  size_t totalRead = 0;
  int lastPctDrawn = -1;

  while (totalRead < toRead) {
    size_t chunk = toRead - totalRead;
    if (chunk > 32768) chunk = 32768;
    size_t got = f.read(&ram_disk[DATA_OFFSET + totalRead], chunk);
    if (got == 0) break;
    totalRead += got;

    int pct = (totalRead * 100) / toRead;
    if (pct / 5 != lastPctDrawn / 5) {
      lastPctDrawn = pct;
      drawThemedProgressBar(pct);
    }
  }
  f.close();

  // Build FAT chain for loaded file
  uint16_t clusters_needed = (totalRead + 511) / 512;
  for (int c = 2; c < 2 + clusters_needed; c++) {
    if (c < 2 + clusters_needed - 1) {
      fat12_set(&ram_disk[FAT1_OFFSET], c, c + 1);
      fat12_set(&ram_disk[FAT2_OFFSET], c, c + 1);
    } else {
      fat12_set(&ram_disk[FAT1_OFFSET], c, 0xFFF);
      fat12_set(&ram_disk[FAT2_OFFSET], c, 0xFFF);
    }
  }
  *(uint16_t *)&ram_disk[ROOTDIR_OFFSET + 26] = 2;
  *(uint32_t *)&ram_disk[ROOTDIR_OFFSET + 28] = totalRead;

  // Show 100% filled progress bar (no extra bar outside frame)
  drawThemedProgressBar(100);

  gfx_setTextColor(TFT_GREEN, TFT_BLACK);
  gfx_setTextSize(2);
  String okMsg = "OK! " + String(totalRead / 1024) + " KB";
  gfx_setCursor((gW - gfx_textWidth(okMsg)) / 2, 200);
  gfx_print(okMsg);
  gfx_flush();
  delay(500);

  return totalRead;
}

// Unload: remove media from USB drive so host sees no disk.
void doUnload() {
  tud_disconnect();
  delay(50);

  // Clear RAM disk and rebuild empty volume
  build_volume_with_file();
  msc.mediaPresent(false);

  tud_connect();

  // Clear last file from config so it won't auto-load next boot
  cfg_lastfile = "";
  loaded_disk_index = -1;
  saveConfig();

  Serial.println("Drive unloaded");
}

// Full load: disconnects USB, loads file, reconnects USB, saves config.
// Use this from loop() when USB is active.
void doLoadSelected() {
  if (selected_index < 0 || selected_index >= (int)file_list.size()) return;

  tud_disconnect();
  delay(50);

  size_t loaded = loadFileToRam(selected_index);

  msc.mediaPresent(loaded > 0);
  tud_connect();

  if (loaded > 0) {
    loaded_disk_index = selected_index;
    cfg_lastfile = file_list[selected_index];
    cfg_lastmode = (g_mode == MODE_ADF) ? "ADF" : "DSK";
    saveConfig();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Gotek Touchscreen Interface starting...");

  uiInit();
  gfx_fillScreen(TFT_BLACK);
  gfx_flush();
  Serial.println("Display initialized");

  touchInit();
  Serial.println("Touch initialized");

  // ── Cracktro splash screen ──
  drawCracktroSplash();

  // ── Boot loading screen ──
  drawBootScreen();
  drawBootProgress("Initializing SD card...", 5);

  init_sd_card();
  Serial.println("SD card initialized");
  drawBootProgress("Loading configuration...", 15);

  loadConfig();
  scanThemes();
  drawBootProgress("Scanning themes...", 25);

  if (cfg_lastmode == "DSK") {
    g_mode = MODE_DSK;
  } else {
    g_mode = MODE_ADF;
  }

  drawBootProgress("Scanning disk images...", 35);
  file_list = listImages();
  buildDisplayNames(file_list);
  sortByDisplay();
  buildGameList();

  Serial.println("Found " + String(file_list.size()) + " images (" + String(game_list.size()) + " games)");
  drawBootProgress("Found " + String(game_list.size()) + " games", 50);

  drawBootProgress("Allocating RAM disk...", 60);
  ram_disk = (uint8_t *)ps_malloc(RAM_DISK_SIZE);
  if (!ram_disk) {
    Serial.println("Failed to allocate RAM disk!");
    drawBootProgress("ERROR: RAM alloc failed!", 60);
    while (1);
  }

  build_volume_with_file();
  Serial.println("RAM disk initialized");
  drawBootProgress("RAM disk ready", 70);

  // Auto-load last file BEFORE USB starts (no tud_disconnect needed)
  bool autoloaded = false;
  if (cfg_lastfile.length() > 0) {
    drawBootProgress("Auto-loading last disk...", 80);
    for (int i = 0; i < (int)file_list.size(); i++) {
      if (file_list[i] == cfg_lastfile) {
        selected_index = i;
        break;
      }
    }
    size_t loaded = loadFileToRam(selected_index);
    autoloaded = (loaded > 0);
    if (autoloaded) {
      loaded_disk_index = selected_index;
      game_selected = findGameIndex(selected_index);
    }
    Serial.println("Auto-loaded: " + file_list[selected_index] + " (" + String(loaded) + " bytes)");
  }

  drawBootProgress("Starting USB...", 90);

  // TEMPORARILY DISABLED for touch debug — USB MSC takes over serial
  #if 0
  msc.vendorID("Gotek");
  msc.productID("Disk");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.mediaPresent(autoloaded);
  msc.begin(msc_block_count, 512);
  USB.begin();
  #endif
  Serial.println("USB MSC SKIPPED (debug mode)");

  drawBootProgress("Ready!", 100);
  delay(300);

  // Show appropriate screen
  if (autoloaded) {
    detail_filename = file_list[selected_index];
    current_screen = SCR_DETAILS;
    drawDetailsFromNFO(detail_filename);
  } else {
    current_screen = SCR_SELECTION;
    drawList();
  }

  Serial.println("Setup complete!");
}


// ============================================================================
// Touch handling — tap & swipe detection
// ============================================================================
// Swipe: finger moved > threshold pixels between touch-down and touch-up.
// Tap: finger stayed within threshold. Processed on release for reliability.
// Buttons are always checked first (priority over swipe/edge gestures).

// Navigate to previous/next game from detail page
void detailGoToPrev() {
  if (game_selected <= 0) return;
  game_selected--;
  selected_index = game_list[game_selected].first_file_index;
  detail_filename = file_list[selected_index];
  drawDetailsFromNFO(detail_filename);
}

void detailGoToNext() {
  if (game_selected >= (int)game_list.size() - 1) return;
  game_selected++;
  selected_index = game_list[game_selected].first_file_index;
  detail_filename = file_list[selected_index];
  drawDetailsFromNFO(detail_filename);
}

// Check if touch (px,py) hits a button rectangle
bool hitBtn(uint16_t px, uint16_t py, int bx, int by, int bw, int bh) {
  return (px >= bx && px <= bx + bw && py >= by && py <= by + bh);
}

// ============================================================================
// Touch helpers
// ============================================================================

void waitForRelease(unsigned long timeout_ms = 2000) {
  uint16_t dummy_x, dummy_y;
  unsigned long start = millis();
  while (touchRead(&dummy_x, &dummy_y)) {
    if (millis() - start > timeout_ms) break;
    delay(20);
  }
  last_touch_time = millis();
}

// ============================================================================
// Main loop — ORIGINAL WORKING touch model (release-based)
// ============================================================================
// This is the exact approach that worked before the UI changes:
//   - Track touch_active for press/release detection
//   - On release: determine tap vs swipe, then process
//   - waitForRelease() after heavy operations to prevent phantom taps
//   - Simple 200ms debounce

#define SWIPE_THRESHOLD 30

void loop() {
  uint16_t px = 0, py = 0;
  bool haveTouch = touchRead(&px, &py);

  // DEBUG: print touch data every 500ms
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 500) {
    lastDebug = millis();
    if (haveTouch) {
      Serial.printf("TOUCH: x=%d y=%d active=%d busy=%d\n", px, py, touch_active, ui_busy);
    }
  }

  if (ui_busy) {
    delay(10);
    return;
  }

  if (haveTouch) {
    if (!touch_active) {
      // Touch DOWN — start tracking
      touch_active = true;
      touch_start_x = px;
      touch_start_y = py;
      touch_start_time = millis();
    }
    // Update last known position while dragging
    touch_last_x = px;
    touch_last_y = py;
  } else if (touch_active) {
    // Touch UP — determine if it was a tap or swipe
    touch_active = false;
    unsigned long now = millis();

    Serial.printf("RELEASE: start=(%d,%d) last=(%d,%d) dt=%lu\n",
      touch_start_x, touch_start_y, touch_last_x, touch_last_y,
      now - last_touch_time);

    // Debounce: ignore very quick phantom touches
    if (now - last_touch_time < 200) {
      delay(10);
      return;
    }

    int16_t dx = (int16_t)touch_last_x - (int16_t)touch_start_x;
    int16_t dy = (int16_t)touch_last_y - (int16_t)touch_start_y;

    if (abs(dx) > SWIPE_THRESHOLD || abs(dy) > SWIPE_THRESHOLD) {
      handleSwipe(dx, dy, touch_start_x, touch_start_y);
    } else {
      handleTap(touch_start_x, touch_start_y);
    }
    last_touch_time = millis();
  }

  delay(10);
}

// ============================================================================
// Handle swipe gestures
// ============================================================================
void handleSwipe(int16_t dx, int16_t dy, uint16_t startX, uint16_t startY) {
  int16_t absDx = abs(dx);
  int16_t absDy = abs(dy);

  if (current_screen == SCR_SELECTION) {
    // Vertical swipe in list area → scroll
    if (startY >= LIST_START_Y && startY < LIST_BOTTOM && absDy > absDx) {
      int scrollItems = absDy / LIST_ITEM_H;
      if (scrollItems < 1) scrollItems = 1;
      if (dy > 0) scroll_offset -= scrollItems;
      else        scroll_offset += scrollItems;
      int maxOff = (int)game_list.size() - items_per_page();
      if (maxOff < 0) maxOff = 0;
      if (scroll_offset > maxOff) scroll_offset = maxOff;
      if (scroll_offset < 0) scroll_offset = 0;
      drawList();
    }
  } else if (current_screen == SCR_DETAILS) {
    // Horizontal swipe → previous/next game
    if (absDx > absDy) {
      if (dx > 0) detailGoToPrev();
      else        detailGoToNext();
    }
  }
}

// ============================================================================
// Handle tap gestures
// ============================================================================
void handleTap(uint16_t px, uint16_t py) {
  Serial.printf("TAP: x=%d y=%d screen=%d\n", px, py, current_screen);

  // ══════════════════════════════════════
  // SELECTION SCREEN
  // ══════════════════════════════════════
  if (current_screen == SCR_SELECTION) {

    // Mode switch button (bottom-right area)
    if (px >= gW - 120 && py >= gH - 42) {
      showBusyIndicator("SCANNING...");
      g_mode = (g_mode == MODE_ADF) ? MODE_DSK : MODE_ADF;
      file_list = listImages();
      buildDisplayNames(file_list);
      sortByDisplay();
      buildGameList();
      selected_index = 0;
      game_selected = 0;
      scroll_offset = 0;
      hideBusyIndicator();
      drawList();
      return;
    }

    // SELECT button (x: 20-110)
    if (px >= 20 && px <= 110 && py >= gH - 42) {
      if (game_selected < (int)game_list.size()) {
        showBusyIndicator("LOADING...");
        selected_index = game_list[game_selected].first_file_index;
        detail_filename = file_list[selected_index];
        current_screen = SCR_DETAILS;
        hideBusyIndicator();
        drawDetailsFromNFO(detail_filename);
      }
      return;
    }

    // OPEN button (x: 130-220)
    if (px >= 130 && px <= 220 && py >= gH - 42) {
      if (game_selected < (int)game_list.size()) {
        selected_index = game_list[game_selected].first_file_index;
        detail_filename = file_list[selected_index];
        showBusyIndicator("LOADING DISK...");
        waitForRelease();
        doLoadSelected();
        current_screen = SCR_DETAILS;
        hideBusyIndicator();
        drawDetailsFromNFO(detail_filename);
      }
      return;
    }

    // INFO button (x: 240-330)
    if (px >= 240 && px <= 330 && py >= gH - 42) {
      current_screen = SCR_INFO;
      drawInfoScreen();
      return;
    }

    // Game list area — tap item to select
    if (py >= LIST_START_Y && py < LIST_BOTTOM) {
      int idx = (py - LIST_START_Y) / LIST_ITEM_H + scroll_offset;
      if (idx >= 0 && idx < (int)game_list.size()) {
        game_selected = idx;
        drawList();
      }
    }
  }

  // ══════════════════════════════════════
  // DETAIL SCREEN
  // ══════════════════════════════════════
  else if (current_screen == SCR_DETAILS) {

    // BACK button (x: 20-110)
    if (px >= 20 && px <= 110 && py >= gH - 42) {
      current_screen = SCR_SELECTION;
      drawList();
      return;
    }

    // LOAD button (x: 130-220)
    if (px >= 130 && px <= 220 && py >= gH - 42) {
      showBusyIndicator("LOADING DISK...");
      waitForRelease();
      doLoadSelected();
      hideBusyIndicator();
      drawDetailsFromNFO(detail_filename);
      return;
    }

    // UNLOAD button (x: 240-330)
    if (px >= 240 && px <= 330 && py >= gH - 42) {
      showBusyIndicator("UNLOADING...");
      doUnload();
      hideBusyIndicator();
      current_screen = SCR_SELECTION;
      drawList();
      return;
    }

    // Left/right edge tap: navigate to previous/next game
    if (py >= LIST_START_Y && py < gH - 48) {
      if (px <= 40) { detailGoToPrev(); return; }
      if (px >= gW - 40) { detailGoToNext(); return; }
    }

    // Disk selector buttons (multi-disk games)
    if (disk_set.size() > 1) {
      int diskRowH = 34;
      int diskTop = gH - 42 - diskRowH;
      if (py >= diskTop && py <= diskTop + 28) {
        int btnW = 44, gap = 4;
        int numDisks = disk_set.size();
        int totalW = numDisks * btnW + (numDisks - 1) * gap;
        int startX = (gW - totalW) / 2;
        int hitIdx = (px - startX) / (btnW + gap);
        int btnLeft = startX + hitIdx * (btnW + gap);
        if (hitIdx >= 0 && hitIdx < numDisks &&
            px >= btnLeft && px <= btnLeft + btnW) {
          selected_index = disk_set[hitIdx];
          detail_filename = file_list[selected_index];
          showBusyIndicator("SWITCHING DISK...");
          waitForRelease();
          doLoadSelected();
          hideBusyIndicator();
          drawDetailsFromNFO(detail_filename);
        }
      }
    }
  }

  // ══════════════════════════════════════
  // INFO SCREEN
  // ══════════════════════════════════════
  else if (current_screen == SCR_INFO) {
    if (px >= 20 && px <= 110 && py >= gH - 42) {
      current_screen = SCR_SELECTION;
      drawList();
      return;
    }
    if (px >= 130 && px <= 240 && py >= gH - 42) {
      cycleTheme();
      drawInfoScreen();
      return;
    }
  }
}
