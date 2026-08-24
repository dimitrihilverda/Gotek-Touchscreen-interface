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
// Getting that option wrong is the classic failure: the chip then reports no
// PSRAM at all and the board looks dead. setup() checks for it and says so.
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
  delay(200);
  dlog("Gotek Dongle " BOARD_NAME);

  // The RAM disk is the product. If it cannot be allocated there is nothing to
  // do but say why — loudly, because there is no screen to say it on.
  const size_t psram = ESP.getPsramSize();
  dlog("PSRAM: " + String((uint32_t)(psram / 1024)) + " KB, volume needs " +
       String(RAM_DISK_SIZE / 1024) + " KB");
  if (psram < RAM_DISK_SIZE) {
    for (;;) {
      Serial.println("FATAL: PSRAM too small for the RAM disk.");
      Serial.println("  Check the board options: SuperMini needs PSRAM=QSPI, XIAO needs OPI.");
      delay(2000);
    }
  }

  ram_disk = (uint8_t *)ps_malloc(RAM_DISK_SIZE);
  if (!ram_disk) {
    for (;;) {
      Serial.println("FATAL: RAM disk allocation failed despite PSRAM being present.");
      delay(2000);
    }
  }
  g_mountFilename = "";
  build_volume_with_file();
  dlog("RAM disk ready: " + String(MAX_IMAGE_BYTES / 1024) + " KB usable" +
       String(SUPPORTS_HD ? " (HD capable)" : " (DD only)"));

  // USB first, then the radio, with a gap between: two current spikes in a row
  // is what browns out a Gotek port.
  msc.vendorID("Gotek");
  msc.productID("Disk");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.mediaPresent(false);
  msc.begin(RAM_DISK_SIZE / 512, 512);
  USB.begin();
  delay(150);

  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
  delay(200);
  dlog("AP " + String(AP_SSID) + " at " + WiFi.softAPIP().toString());

  httpServer.begin();
  dlog("Ready");
}

void loop() {
  WiFiClient client = httpServer.available();
  if (client) handleClient(client);
  delay(2);
}
