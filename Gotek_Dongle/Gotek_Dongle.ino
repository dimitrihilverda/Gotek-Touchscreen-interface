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
#include <ESPmDNS.h>
#include "USB.h"
#include "USBMSC.h"

// The same files the touchscreen firmware builds, included rather than copied.
// If these ever diverge, one of the two boards is quietly running different
// disk code — which is exactly the trap the upstream project fell into with
// seven near-identical sketches.
#include "dongle_config.h"        // settings + log; must precede webdav_client.h
#include "../Gotek_Touchscreen/board_profile.h"
#include "../Gotek_Touchscreen/ram_disk.h"
#include "../Gotek_Touchscreen/multipart_scan.h"
#include "../Gotek_Touchscreen/perf.h"
#include "../Gotek_Touchscreen/webdav_client.h"

#if HAS_DISPLAY
#error "Gotek_Dongle is the screenless build - pick BOARD_XIAO or BOARD_SUPERMINI"
#endif

static const uint8_t AP_CHANNEL = 6;

#define DONGLE_VERSION "v0.2.0"

static wifi_power_t txPowerLevel(int dbm) {
  if (dbm <= 2)  return WIFI_POWER_2dBm;
  if (dbm <= 5)  return WIFI_POWER_5dBm;
  if (dbm <= 7)  return WIFI_POWER_7dBm;
  if (dbm <= 9)  return WIFI_POWER_8_5dBm;
  if (dbm <= 11) return WIFI_POWER_11dBm;
  if (dbm <= 13) return WIFI_POWER_13dBm;
  if (dbm <= 15) return WIFI_POWER_15dBm;
  if (dbm <= 17) return WIFI_POWER_17dBm;
  if (dbm <= 19) return WIFI_POWER_19dBm;
  return WIFI_POWER_19_5dBm;
}

// davClient itself is declared by webdav_client.h.
static bool davConnected = false;
static String g_davLoadedPath = "";   // what the page shows as playing

// ── Cover and notes for the game that is in ──────────────────────────────
//
// The touchscreen keeps these on the card. There is no card here, so the one
// that matters — the game currently mounted — lives in PSRAM instead. Browsing
// still shows "No Art" for everything else, which is honest: fetching a cover
// per row over TLS with no persistent store would cost more than it gives.
//
// Capped, because PSRAM is the same pool the RAM disk came out of: on a
// SuperMini there is roughly a megabyte left once the volume is allocated.
#define COVER_MAX_BYTES (128 * 1024)
static uint8_t *g_coverBuf  = nullptr;
static size_t   g_coverLen  = 0;
static String   g_coverPath = "";      // remote path the bytes came from
static String   g_nfoPath   = "";
static String   g_nfoText   = "";

WiFiServer httpServer(80);

// ── State ────────────────────────────────────────────────────────────────
static bool     g_mediaPresent = false;
static uint32_t g_lastEnumerateMs = 0;   // how long the host took to re-attach
static bool     g_lastEnumerateOk = false;
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

  // Re-attach, then wait for the host to actually take us back.
  //
  // Everything before this is our own work; this is the part the Gotek
  // controls, and it is what "ready on the drive" really means. Without
  // measuring it the timings stop one step short of the thing you care about.
  //
  // Bounded, because a Gotek that is switched off will never enumerate and the
  // web request must still return.
  const uint32_t t0 = millis();
  tud_connect();
  while (!tud_mounted() && millis() - t0 < 3000) delay(5);
  g_lastEnumerateMs = millis() - t0;
  g_lastEnumerateOk = tud_mounted();

  dlog("Mounted " + name + " (" + String(bytes / 1024) + " KB), host took " +
       String(g_lastEnumerateMs) + "ms" + (g_lastEnumerateOk ? "" : " (no host)"));
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
  loadConfig();

  const size_t psram = ESP.getPsramSize();
  char bootSsid[32];
  snprintf(bootSsid, sizeof(bootSsid), "Gotek-BOOT-%uK", (unsigned)(psram / 1024));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(bootSsid, cfg_wifi_pass.c_str(), AP_CHANNEL);
  delay(200);

  dlog("Gotek Dongle " BOARD_NAME);
  dlog("PSRAM: " + String((uint32_t)(psram / 1024)) + " KB, volume needs " +
       String(RAM_DISK_SIZE / 1024) + " KB");

  if (psram < RAM_DISK_SIZE) {
    WiFi.softAP("Gotek-FAIL-PSRAM", cfg_wifi_pass.c_str(), AP_CHANNEL);
    for (;;) {
      Serial.println("FATAL: PSRAM too small. SuperMini needs PSRAM=QSPI, XIAO needs OPI.");
      delay(2000);
    }
  }

  ram_disk = (uint8_t *)ps_malloc(RAM_DISK_SIZE);
  if (!ram_disk) {
    WiFi.softAP("Gotek-FAIL-ALLOC", cfg_wifi_pass.c_str(), AP_CHANNEL);
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
  // Everything worked: drop the diagnostic name. AP+STA if a network is
  // configured, because a WebDAV server lives out there, not on our own AP.
  WiFi.mode(cfg_wifi_client_enabled && cfg_wifi_client_ssid.length() ? WIFI_AP_STA
                                                                    : WIFI_AP);
  WiFi.setTxPower(txPowerLevel(cfg_wifi_tx_dbm));
  WiFi.softAP(cfg_wifi_ssid.c_str(), cfg_wifi_pass.c_str(),
              (cfg_wifi_client_enabled && cfg_wifi_client_ssid.length()) ? 0
                                                                        : AP_CHANNEL);
  delay(200);
  dlog("AP " + cfg_wifi_ssid + " at " + WiFi.softAPIP().toString());

  if (cfg_wifi_client_enabled && cfg_wifi_client_ssid.length()) {
    // Non-blocking: the page is reachable on the AP whether or not the network
    // ever answers, which matters when the password is what you came to fix.
    WiFi.begin(cfg_wifi_client_ssid.c_str(), cfg_wifi_client_pass.c_str());
    dlog("Joining " + cfg_wifi_client_ssid);
  }
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

  // gotek.local, the same name the touchscreen answers to. Worth more here
  // than there: a dongle has no screen to show you its address.
  if (MDNS.begin("gotek")) {
    MDNS.addService("http", "tcp", 80);
    dlog("Reachable at gotek.local");
  } else {
    dlog("mDNS failed; use the IP address");
  }

  httpServer.begin();
  dlog("Ready");
}

void loop() {
  WiFiClient client = httpServer.available();
  if (client) handleClient(client);
  delay(2);
}
