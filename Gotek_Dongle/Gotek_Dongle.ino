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
#include <DNSServer.h>
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

// This product's settings become the client's settings. Called at boot and
// from the config handler in dongle_web.h; the client itself reads nothing.
static void davApplyConfig() {
  DavConfig c;
  c.host     = cfg_dav_host;
  c.port     = cfg_dav_port;
  c.https    = cfg_dav_https;
  c.user     = cfg_dav_user;
  c.pass     = cfg_dav_pass;
  c.basePath = cfg_dav_path;
  c.enabled  = cfg_dav_enabled;
  davClient.configure(c, dlog);
}

#if HAS_DISPLAY
#error "Gotek_Dongle is the screenless build - pick BOARD_XIAO or BOARD_SUPERMINI"
#endif

// The loop task runs the HTTP server, the WebDAV client and the disk load, and
// mbedTLS is not shy with stack — a TLS record plus _pumpBody's own 1 KB
// scratch does not fit in Arduino's 8 KB default. The touchscreen has carried
// this line since its own crashes; the dongle inherited the client but not the
// stack it needs, which is why an insert panicked mid-transfer and why it was
// intermittent: how deep TLS goes depends on the certificate chain.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static const uint8_t AP_CHANNEL = 6;

// Tracks the repo tag: two products, one number to quote.
#define DONGLE_VERSION "v0.25.2"

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

// Set by the HTTP handler, done by loop(). Keeping the transfer out of the
// request is what lets the device still answer /api/log and /api/disk/status
// while a 1.76 MB image is coming in over TLS.
static String g_pendingDavPath = "";
static bool   g_davBusy = false;      // reported so the page can say "loading"

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

// Captive portal: on the AP we answer every DNS question with ourselves, so
// the phone's connectivity probe lands on our web server, which answers it
// with a redirect — and the OS pops its sign-in sheet showing the interface.
// The point on a device with no screen: joining the AP IS the setup flow.
DNSServer dnsServer;

// Set by the config handler when WiFi-client settings changed: reboot after
// the response has left, because applying a network change mid-request would
// answer nobody. 0 = nothing pending.
uint32_t g_rebootAtMs = 0;

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
  // Wait for the host to actually READ us, not merely to claim it is attached.
  //
  // tud_mounted() was the obvious check and it was useless: right after
  // tud_connect() it still reports the previous session, so the timer read zero
  // every single time. A sector read is unambiguous — the Gotek is looking at
  // the new disk.
  const uint32_t t0 = millis();
  g_lastHostReadMs = 0;
  tud_connect();
  while (g_lastHostReadMs == 0 && millis() - t0 < 3000) delay(5);
  g_lastEnumerateOk = (g_lastHostReadMs != 0);
  g_lastEnumerateMs = millis() - t0;

  dlog("Mounted " + name + " (" + String(bytes / 1024) + " KB), host read after " +
       String(g_lastEnumerateMs) + "ms" + (g_lastEnumerateOk ? "" : " (no read yet)"));
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
  logBegin();
  // First line after a restart says why, and the previous run's lines are still
  // above it — which is the whole point of keeping the log in RTC memory.
  dlog("--- boot: " + String(resetReasonName()) + " ---");
  loadConfig();
  davApplyConfig();

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
  dnsServer.start(53, "*", WiFi.softAPIP());   // captive portal: all names are us

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

  // Its own name, not the touchscreen's: both products on one LAN claiming
  // "gotek" made which-device-answers a coin flip. Worth more here than on a
  // panel: a dongle has no screen to show you its address.
  if (MDNS.begin(cfg_mdns_name.c_str())) {
    MDNS.addService("http", "tcp", 80);
    dlog("Reachable at " + cfg_mdns_name + ".local");
  } else {
    dlog("mDNS failed; use the IP address");
  }

  httpServer.begin();
  dlog("Ready");
}

// The queued WebDAV load, run outside any request.
static void serviceDavLoad() {
  if (g_pendingDavPath.length() == 0) return;
  const String remote = g_pendingDavPath;
  g_pendingDavPath = "";
  g_davBusy = true;

  // Give the transfer every byte of heap we can. TLS wants ~46 KB and a failed
  // allocation inside the stream is an uncaught bad_alloc, which on this core
  // means abort() and a reboot rather than an error message.
  g_listCachePath = "";
  g_listCacheJson = "";
  dlog("DAV insert: heap " + String(ESP.getFreeHeap()) + " before transfer");

  // Breadcrumbs into the RTC log every 128 KB. The failure moved from PANIC to
  // watchdog when the heap pressure went away, which means the earlier model was
  // incomplete — and the backtrace goes to a serial port that a reset takes with
  // it. How far the transfer got, and what the heap was doing, is the one thing
  // that survives.
  davClient.setProgressCallback([](size_t got, size_t total) {
    static size_t nextMark = 0;
    if (got == 0) nextMark = 0;
    if (got < nextMark) return;
    nextMark = got + 128 * 1024;
    dlog("  ..." + String((uint32_t)(got / 1024)) + " KB, heap " +
         String(ESP.getFreeHeap()) + ", psram " + String(ESP.getFreePsram()));
  });

  Perf perf("DAV insert");
  tud_disconnect();
  delay(30);
  g_mountBytes = 0;
  svReset();
  perf.mark("detach");

  // Deliberately NOT on a pooled connection — see streamToBuffer. This is a
  // bisect, not a diagnosis: if the panic stops, reuse plus a large body is the
  // culprit and worth understanding properly.
  const long got = davClient.streamToBuffer(remote, &ram_disk[DATA_OFFSET],
                                            MAX_IMAGE_BYTES, false);
  perf.mark("fetch");

  if (got <= 0) {
    tud_connect();
    dlog("DAV load failed: " + davClient.lastError());
  } else if (davClient.lastTruncated()) {
    tud_connect();
    dlog("DAV load refused: image exceeds " + String((uint32_t)MAX_IMAGE_BYTES) +
         " bytes");
  } else {
    String name = remote;
    const int sl = name.lastIndexOf('/');
    if (sl >= 0) name = name.substring(sl + 1);
    build_boot_sector(&ram_disk[0]);
    build_fat(&ram_disk[FAT1_OFFSET]);
    build_fat(&ram_disk[FAT2_OFFSET]);
    g_mountFilename = name;
    build_root(&ram_disk[ROOTDIR_OFFSET]);
    mountImage(name, (uint32_t)got);
    perf.phase(g_lastEnumerateOk ? "host-read" : "host-silent", g_lastEnumerateMs);
    cacheArtFor(remote, perf);
    g_davLoadedPath = remote;
    g_listCacheJson = "";        // state changed; do not serve a stale view
    perf.bytes((uint32_t)got);
    dlog(perf.summary());
  }
  g_davBusy = false;
}

void loop() {
  dnsServer.processNextRequest();
  if (g_rebootAtMs && millis() > g_rebootAtMs) ESP.restart();
  WiFiClient client = httpServer.available();
  if (client) handleClient(client);
  serviceDavLoad();
  davClient.dropIdle();
  delay(2);
}
