// ============================================================================
// Gotek Dongle — a screenless, SD-less disk loader
// ============================================================================
//
// The whole device: plug it into the Gotek's USB port, join its WiFi, open a
// page on your phone, pick an .adf, and it is mounted. No display, no touch, no
// SD card, no library — the image goes straight into the RAM disk that the
// Gotek reads as a USB drive.
//
// Build with -DACTIVE_BOARD=BOARD_XIAO or -DACTIVE_BOARD=BOARD_SUPERMINI.
//
//   XIAO ESP32S3   8 MB octal PSRAM -> 2 MB volume, Amiga HD images fit
//                  arduino-cli: PSRAM=opi        (IDE: "OPI PSRAM")
//   SuperMini      2 MB quad PSRAM  -> 1 MB volume, standard 880 KB DD only
//                  arduino-cli: PSRAM=enabled    (IDE: "QSPI PSRAM")
//
// Verified running on a SuperMini with exactly:
//   esp32:esp32:esp32s3:PSRAM=enabled,FlashSize=4M,PartitionScheme=min_spiffs,
//                       USBMode=default,CDCOnBoot=cdc
//
// USBMode=default (USB-OTG / TinyUSB) is what lets USBMSC present the drive.
// Note the board's only USB-C is the port it hands to the Gotek, so there is no
// second UART: after USB.begin() the CDC port moves, and before it there is no
// serial at all. Bring-up therefore leans on the access point, not the cable.
//
// If the board seems dead after flashing, suspect the ROM download mode before
// suspecting the firmware — esptool's "hard reset via RTS" is only a request on
// a board with no physical reset wiring, and it can be ignored. Unplug and
// replug. That cost an hour here.
//
// This shares its disk and parsing code with the touchscreen firmware rather
// than copying it: ram_disk.h, board_profile.h and multipart_scan.h are the
// same files that build there.

#include <Arduino.h>
#include <WiFi.h>
#include "USB.h"
#include "USBMSC.h"

// The same files the touchscreen firmware builds, included rather than copied.
// If these ever diverge, one of the two boards is quietly running different
// disk code — which is exactly the trap the upstream project fell into with
// seven near-identical sketches.
#include "../Gotek_Touchscreen/board_profile.h"
#include "../Gotek_Touchscreen/ram_disk.h"
#include "../Gotek_Touchscreen/multipart_scan.h"

#if HAS_DISPLAY
#error "Gotek_Dongle is the screenless build - pick BOARD_XIAO or BOARD_SUPERMINI"
#endif

// ── Configuration ────────────────────────────────────────────────────────
// No SD card, so nothing reads CONFIG.TXT. These are the defaults until the
// settings move into NVS.
static const char *AP_SSID = "Gotek-Dongle";
static const char *AP_PASS = "retrogaming";
static const uint8_t AP_CHANNEL = 6;

// The radio is the biggest load on a port that is also feeding the Gotek, and
// an Amiga's thirty-year-old 5V rail does not hold up under a full-power burst.
// There is no backlight to dim here, so this is the only lever left.
static const int WIFI_TX_DBM = 15;

#define DONGLE_VERSION "v0.1.0"

WiFiServer httpServer(80);

// ── A log you can read without a serial cable ────────────────────────────
//
// No screen and no SD means a dongle that misbehaves can tell you nothing at
// all. A small ring buffer in RAM, served at /log, is the entire diagnostic
// surface — so it is worth having from the first build rather than added after
// the first mystery.
#define LOG_LINES 40
#define LOG_LINE_LEN 96
static char     g_log[LOG_LINES][LOG_LINE_LEN];
static uint8_t  g_logHead = 0;
static uint8_t  g_logCount = 0;

void dlog(const String &msg) {
  Serial.println(msg);
  strncpy(g_log[g_logHead], msg.c_str(), LOG_LINE_LEN - 1);
  g_log[g_logHead][LOG_LINE_LEN - 1] = 0;
  g_logHead = (g_logHead + 1) % LOG_LINES;
  if (g_logCount < LOG_LINES) g_logCount++;
}

// ── State ────────────────────────────────────────────────────────────────
static bool     g_mediaPresent = false;
static uint32_t g_inquiryRev   = 1;

// Changing the reported revision makes the Gotek notice that the medium
// changed; without it a swapped image can be served from its own cache.
static void bumpInquiryRevision() {
  char rev[8];
  snprintf(rev, sizeof(rev), "%lu", (unsigned long)g_inquiryRev++);
  msc.productRevision(rev);
}

static void mountImage(const String &name, uint32_t bytes) {
  // The FAT chain for a file that starts at cluster 2 and runs to its end.
  const uint16_t clusters = (bytes + 511) / 512;
  for (uint16_t c = 2; c < 2 + clusters; c++) {
    const uint16_t next = (c < 2 + clusters - 1) ? (uint16_t)(c + 1) : 0xFFF;
    fat12_set(&ram_disk[FAT1_OFFSET], c, next);
    fat12_set(&ram_disk[FAT2_OFFSET], c, next);
  }
  *(uint16_t *)&ram_disk[ROOTDIR_OFFSET + 26] = 2;
  *(uint32_t *)&ram_disk[ROOTDIR_OFFSET + 28] = bytes;

  g_mountLabel = name;
  g_mountPath  = "";          // nothing local to write a save back to
  g_mountBytes = bytes;

  bumpInquiryRevision();
  msc.mediaPresent(true);
  g_mediaPresent = true;
  tud_connect();
  dlog("Mounted " + name + " (" + String(bytes / 1024) + " KB)");
}

static void ejectImage() {
  tud_disconnect();
  delay(50);
  g_mountFilename = "";
  g_mountLabel    = "";
  g_mountBytes    = 0;
  svReset();
  build_volume_with_file();
  bumpInquiryRevision();
  msc.mediaPresent(false);
  g_mediaPresent = false;
  tud_connect();
  dlog("Ejected");
}

#include "dongle_web.h"

// ============================================================================

void setup() {
  Serial.begin(115200);

  // ── The access point is the console ──────────────────────────────────
  //
  // This board's only connector is the USB port it hands to the Gotek, and with
  // TinyUSB the serial port does not exist until USB.begin() — so anything that
  // goes wrong before then is completely silent. Nothing to watch, nothing to
  // read, no LED worth trusting.
  //
  // So the radio comes up FIRST, with the PSRAM figure in its name. Whatever
  // happens afterwards, a phone within a few metres can read how much memory
  // the chip reported. It is renamed to the real SSID once the disk is up, so a
  // working device never shows the diagnostic name.
  const size_t psram = ESP.getPsramSize();
  char bootSsid[32];
  snprintf(bootSsid, sizeof(bootSsid), "Gotek-BOOT-%uK", (unsigned)(psram / 1024));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(bootSsid, AP_PASS, AP_CHANNEL);
  delay(200);

  dlog("Gotek Dongle " BOARD_NAME);
  dlog("PSRAM: " + String((uint32_t)(psram / 1024)) + " KB, volume needs " +
       String(RAM_DISK_SIZE / 1024) + " KB");

  if (psram < RAM_DISK_SIZE) {
    WiFi.softAP("Gotek-FAIL-PSRAM", AP_PASS, AP_CHANNEL);
    for (;;) {
      Serial.println("FATAL: PSRAM too small. SuperMini needs PSRAM=QSPI, XIAO needs OPI.");
      delay(2000);
    }
  }

  ram_disk = (uint8_t *)ps_malloc(RAM_DISK_SIZE);
  if (!ram_disk) {
    WiFi.softAP("Gotek-FAIL-ALLOC", AP_PASS, AP_CHANNEL);
    for (;;) {
      Serial.println("FATAL: RAM disk allocation failed despite PSRAM being present.");
      delay(2000);
    }
  }
  g_mountFilename = "";
  build_volume_with_file();
  dlog("RAM disk ready: " + String(MAX_IMAGE_BYTES / 1024) + " KB usable" +
       String(SUPPORTS_HD ? " (HD capable)" : " (DD only)"));

  // The radio comes up first, and deliberately so.
  //
  // On a board whose only connector is the USB port it is presenting to the
  // Gotek, serial is not a reliable way to find out what happened — there is no
  // second UART to fall back on, and with TinyUSB the CDC port does not exist
  // until USB.begin(). If USB never comes up, a device that started its access
  // point at least says "I booted" from across the room.
  //
  // The two are still sequenced rather than simultaneous, which is what the
  // brownout concern was really about: an Amiga's 5V rail does not like a radio
  // burst and USB enumeration landing together.
  // Everything worked: drop the diagnostic name.
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
  delay(200);
  dlog("AP " + String(AP_SSID) + " at " + WiFi.softAPIP().toString());
  delay(150);

  msc.vendorID("Gotek");
  msc.productID("Disk");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.mediaPresent(false);
  msc.begin(RAM_DISK_SIZE / 512, 512);
  USB.begin();
  dlog("USB mass storage up");

  httpServer.begin();
  dlog("Ready");
}

void loop() {
  WiFiClient client = httpServer.available();
  if (client) handleClient(client);
  delay(2);
}
