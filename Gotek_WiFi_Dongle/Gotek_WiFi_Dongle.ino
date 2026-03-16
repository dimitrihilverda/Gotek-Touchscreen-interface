/*
  Gotek WiFi Dongle — Headless PSRAM-only Edition

  A minimal ESP32-S3 WiFi dongle that plugs into a Gotek's USB port.
  No SD card, no display — disk images are sent via WiFi and stored in PSRAM.

  How it works:
    1. Dongle creates WiFi AP ("Gotek-Dongle") + optionally connects to your home network
    2. Open http://192.168.4.1 (AP) or the assigned IP (STA) on your phone/laptop
    3. Upload, FTP browse, or WebDAV browse disk images
    4. Dongle loads it into PSRAM → presents as USB floppy to Gotek
    5. Play!

  Target board: Seeed XIAO ESP32-S3 (21 x 17.5 mm)
    - ESP32-S3 dual-core 240MHz
    - 8MB PSRAM (plenty for a 1.44MB floppy image)
    - 8MB Flash
    - WiFi 802.11 b/g/n
    - USB-C with OTG support
    - No SD card needed!

  Board settings (Arduino IDE):
    Board: XIAO_ESP32S3
    USB CDC On Boot → Enabled
    PSRAM → OPI PSRAM
    Flash Size → 8MB
    Partition → Default 4MB with spiffs

  Wiring:
    USB-A plug → Gotek USB port
    That's it. No other connections needed.
*/

#include <Arduino.h>
#include <vector>
#include <USB.h>
#include <USBMSC.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <SPIFFS.h>

// ─── Device target ───────────────────────────────────────────────────────────
#define DEVICE_WIFI_DONGLE

// ─── Shared library storage backend — SPIFFS, flat namespace ─────────────────
#define DAV_CACHE_FS          SPIFFS
#define DAV_CACHE_FS_IS_SPIFFS
#define DAV_CACHE_DIR         ""

extern "C" {
  extern bool tud_mounted(void);
  extern void tud_disconnect(void);
  extern void tud_connect(void);
  extern void* ps_malloc(size_t size);
}

#define FW_VERSION "v2.0.0-WiFiDongle"

// ==========================================================================
// STATUS LED (XIAO ESP32-S3 built-in LED on IO21)
// ==========================================================================
#define LED_PIN 21

void ledBlink(int times = 1, int ms = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(ms);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(ms);
  }
}

// ==========================================================================
// CONFIG — stored in NVS flash (no SD card on dongle)
// ==========================================================================
Preferences prefs;

// WiFi AP config
String cfg_wifi_ssid     = "Gotek-Dongle";
String cfg_wifi_pass     = "retrogaming";
uint8_t cfg_wifi_channel = 6;

// WiFi Client (STA) config — connect to existing network
bool   cfg_wifi_client_enabled = false;
String cfg_wifi_client_ssid    = "";
String cfg_wifi_client_pass    = "";

// FTP config
bool   cfg_ftp_enabled = false;
String cfg_ftp_host    = "";
int    cfg_ftp_port    = 21;
String cfg_ftp_user    = "anonymous";
String cfg_ftp_pass    = "gotek@local";
String cfg_ftp_path    = "/";

// Logging config
bool   cfg_log_enabled = true;

// WebDAV config
bool   cfg_dav_enabled = false;
String cfg_dav_host    = "";
int    cfg_dav_port    = 443;
String cfg_dav_user    = "";
String cfg_dav_pass    = "";
String cfg_dav_path    = "/remote.php/webdav/";
bool   cfg_dav_https   = true;

// WiFi state
bool wifi_ap_active = false;
String wifi_ap_ip = "";
bool wifi_sta_connected = false;
String wifi_sta_ip = "";

void loadConfig() {
  prefs.begin("gotek", true);  // read-only
  cfg_wifi_ssid     = prefs.getString("ap_ssid", "Gotek-Dongle");
  cfg_wifi_pass     = prefs.getString("ap_pass", "retrogaming");
  cfg_wifi_channel  = prefs.getUChar("ap_chan", 6);

  cfg_wifi_client_enabled = prefs.getBool("sta_en", false);
  cfg_wifi_client_ssid    = prefs.getString("sta_ssid", "");
  cfg_wifi_client_pass    = prefs.getString("sta_pass", "");

  cfg_ftp_enabled = prefs.getBool("ftp_en", false);
  cfg_ftp_host    = prefs.getString("ftp_host", "");
  cfg_ftp_port    = prefs.getInt("ftp_port", 21);
  cfg_ftp_user    = prefs.getString("ftp_user", "anonymous");
  cfg_ftp_pass    = prefs.getString("ftp_pass", "gotek@local");
  cfg_ftp_path    = prefs.getString("ftp_path", "/");

  cfg_dav_enabled = prefs.getBool("dav_en", false);
  cfg_dav_host    = prefs.getString("dav_host", "");
  cfg_dav_port    = prefs.getInt("dav_port", 443);
  cfg_dav_user    = prefs.getString("dav_user", "");
  cfg_dav_pass    = prefs.getString("dav_pass", "");
  cfg_dav_path    = prefs.getString("dav_path", "/remote.php/webdav/");
  cfg_dav_https   = prefs.getBool("dav_https", true);

  cfg_log_enabled = prefs.getBool("log_en", true);
  prefs.end();
}

void saveConfig() {
  prefs.begin("gotek", false);  // read-write
  prefs.putString("ap_ssid", cfg_wifi_ssid);
  prefs.putString("ap_pass", cfg_wifi_pass);
  prefs.putUChar("ap_chan", cfg_wifi_channel);

  prefs.putBool("sta_en", cfg_wifi_client_enabled);
  prefs.putString("sta_ssid", cfg_wifi_client_ssid);
  prefs.putString("sta_pass", cfg_wifi_client_pass);

  prefs.putBool("ftp_en", cfg_ftp_enabled);
  prefs.putString("ftp_host", cfg_ftp_host);
  prefs.putInt("ftp_port", cfg_ftp_port);
  prefs.putString("ftp_user", cfg_ftp_user);
  prefs.putString("ftp_pass", cfg_ftp_pass);
  prefs.putString("ftp_path", cfg_ftp_path);

  prefs.putBool("dav_en", cfg_dav_enabled);
  prefs.putString("dav_host", cfg_dav_host);
  prefs.putInt("dav_port", cfg_dav_port);
  prefs.putString("dav_user", cfg_dav_user);
  prefs.putString("dav_pass", cfg_dav_pass);
  prefs.putString("dav_path", cfg_dav_path);
  prefs.putBool("dav_https", cfg_dav_https);

  prefs.putBool("log_en", cfg_log_enabled);
  prefs.end();
}

// ==========================================================================
// FTP & WebDAV CLIENTS — shared library (no SD storage on dongle)
// ==========================================================================
// sdLog stub — dongle has no SD card, logging goes to Serial + ring buffer only
inline void sdLog(const String &msg) { (void)msg; }

#include "../shared/ftp_client.h"
#include "../shared/webdav_client.h"
// ftpClient and davClient are declared as globals inside the shared headers

// ==========================================================================
// DAV CACHE — in-memory + SPIFFS persistent cache (no SD card on dongle)
// ==========================================================================
std::vector<DAVFileEntry> dav_entries;  // in-memory cache of root listing

#define DAV_CACHE_FILE "/DAV_CACHE.TXT"

// DAV cache v3 format — compatible with shared DAVFileEntry struct
// Format per line:
//   D|name|href|coverPath|nfoPath|diskCount|indexed
//   F|size|name
void davSaveCache() {
  File f = SPIFFS.open(DAV_CACHE_FILE, "w");
  if (!f) return;
  f.println("V3");
  f.println("HOST=" + cfg_dav_host);
  f.println("COUNT=" + String(dav_entries.size()));
  for (const auto &e : dav_entries) {
    if (e.isDir) {
      // D|name|href|coverPath|nfoPath|diskCount|indexed
      f.print("D|");
      f.print(e.name); f.print("|");
      f.print(e.href); f.print("|");
      f.print(e.coverPath); f.print("|");
      f.print(e.nfoPath); f.print("|");
      f.print(String(e.diskCount)); f.print("|");
      f.println(e.indexed ? "1" : "0");
    } else {
      f.println("F|" + String(e.size) + "|" + e.name);
    }
  }
  f.close();
}

bool davLoadCache() {
  if (!SPIFFS.exists(DAV_CACHE_FILE)) return false;
  File f = SPIFFS.open(DAV_CACHE_FILE, "r");
  if (!f) return false;

  dav_entries.clear();
  String line;
  bool hostOk = false;
  bool isV3 = false;

  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (line == "V3") { isV3 = true; continue; }
    if (line.startsWith("HOST=")) {
      hostOk = (line.substring(5) == cfg_dav_host);
      continue;
    }
    if (line.startsWith("COUNT=")) continue;

    if (!hostOk) {
      f.close();
      dav_entries.clear();
      return false;
    }

    if (line.startsWith("D|")) {
      DAVFileEntry entry;
      entry.isDir = true;
      entry.size = 0;
      entry.hasCover = false;
      entry.hasNfo = false;

      if (isV3) {
        // D|name|href|coverPath|nfoPath|diskCount|indexed
        String rest = line.substring(2);
        int p1 = rest.indexOf('|');
        if (p1 < 0) { entry.name = rest; }
        else {
          entry.name = rest.substring(0, p1);
          int p2 = rest.indexOf('|', p1 + 1);
          if (p2 >= 0) {
            entry.href = rest.substring(p1 + 1, p2);
            int p3 = rest.indexOf('|', p2 + 1);
            if (p3 >= 0) {
              entry.coverPath = rest.substring(p2 + 1, p3);
              entry.hasCover = (entry.coverPath.length() > 0);
              int p4 = rest.indexOf('|', p3 + 1);
              if (p4 >= 0) {
                entry.nfoPath = rest.substring(p3 + 1, p4);
                entry.hasNfo = (entry.nfoPath.length() > 0);
                int p5 = rest.indexOf('|', p4 + 1);
                if (p5 >= 0) {
                  entry.diskCount = rest.substring(p4 + 1, p5).toInt();
                  entry.indexed = (rest.substring(p5 + 1) == "1");
                }
              }
            }
          }
        }
      } else {
        // Legacy v1: D|name
        entry.name = line.substring(2);
      }
      dav_entries.push_back(entry);
    } else if (line.startsWith("F|")) {
      String rest = line.substring(2);
      int p1 = rest.indexOf('|');
      if (p1 < 0) continue;

      DAVFileEntry entry;
      entry.isDir = false;
      entry.size = rest.substring(0, p1).toInt();
      entry.hasCover = false;
      entry.hasNfo = false;

      if (isV3) {
        // F|size|name
        entry.name = rest.substring(p1 + 1);
      } else {
        // Legacy v1: F|size|name|coverFile|nfoFile
        int p2 = rest.indexOf('|', p1 + 1);
        if (p2 < 0) { entry.name = rest.substring(p1 + 1); }
        else {
          entry.name = rest.substring(p1 + 1, p2);
          int p3 = rest.indexOf('|', p2 + 1);
          if (p3 >= 0) {
            entry.coverFile = rest.substring(p2 + 1, p3);
            entry.nfoFile = rest.substring(p3 + 1);
            entry.hasCover = (entry.coverFile.length() > 0);
            entry.hasNfo = (entry.nfoFile.length() > 0);
          }
        }
      }
      dav_entries.push_back(entry);
    }
  }
  f.close();
  return (dav_entries.size() > 0);
}

void davClearCache() {
  if (SPIFFS.exists(DAV_CACHE_FILE)) {
    SPIFFS.remove(DAV_CACHE_FILE);
  }
}

inline bool davCacheExists() {
  return (dav_entries.size() > 0) || SPIFFS.exists(DAV_CACHE_FILE);
}

// ==========================================================================
// SHARED LIBRARY — per-folder DAV cache (SPIFFS backend for dongle)
// ==========================================================================
#include "../shared/dav_folder_cache.h"

// ==========================================================================
// RAM DISK — FAT12 floppy in PSRAM
// ==========================================================================
#define RAM_DISK_SIZE (2880 * 512)   // 1.44 MB
#define FAT1_OFFSET   512
#define FAT2_OFFSET   5120
#define ROOTDIR_OFFSET 9728
#define DATA_OFFSET   16896

uint8_t *ram_disk = NULL;
uint32_t msc_block_count;

// Current state
String loaded_filename = "";
size_t loaded_size = 0;
bool disk_present = false;

// USB MSC
USBMSC msc;

// ==========================================================================
// FAT12 FILESYSTEM
// ==========================================================================

void fat12_set(uint8_t *fat, int idx, uint16_t val) {
  if (idx % 2 == 0) {
    fat[idx * 3 / 2] = val & 0xFF;
    fat[idx * 3 / 2 + 1] = (fat[idx * 3 / 2 + 1] & 0xF0) | ((val >> 8) & 0x0F);
  } else {
    fat[idx * 3 / 2] = (fat[idx * 3 / 2] & 0x0F) | ((val & 0x0F) << 4);
    fat[idx * 3 / 2 + 1] = (val >> 4) & 0xFF;
  }
}

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
  buf[0x24] = 0x00;
  buf[0x26] = 0x29;
  buf[0x27] = 0x47; buf[0x28] = 0x4F; buf[0x29] = 0x54; buf[0x2A] = 0x4B;
  memcpy(&buf[0x2B], "GOTEK      ", 11);
  memcpy(&buf[0x36], "FAT12   ", 8);
  buf[510] = 0x55; buf[511] = 0xAA;
}

void make_83_name(const char *src, uint8_t *dst) {
  memset(dst, ' ', 11);
  const char *dot = strrchr(src, '.');
  int nameLen = dot ? (int)(dot - src) : (int)strlen(src);
  for (int i = 0, j = 0; i < nameLen && j < 8; i++) dst[j++] = toupper(src[i]);
  if (dot) { dot++; for (int j = 8; *dot && j < 11; dot++) dst[j++] = toupper(*dot); }
}

void build_empty_volume() {
  memset(ram_disk, 0, RAM_DISK_SIZE);
  build_boot_sector(&ram_disk[0]);
  uint8_t *fat1 = &ram_disk[FAT1_OFFSET];
  uint8_t *fat2 = &ram_disk[FAT2_OFFSET];
  memset(fat1, 0, 4608);
  memset(fat2, 0, 4608);
  fat12_set(fat1, 0, 0xFF0);
  fat12_set(fat1, 1, 0xFFF);
  fat12_set(fat2, 0, 0xFF0);
  fat12_set(fat2, 1, 0xFFF);
  memset(&ram_disk[ROOTDIR_OFFSET], 0, 7168);
  msc_block_count = RAM_DISK_SIZE / 512;
}

void build_fat_for_file(const char *filename, size_t fileSize) {
  uint8_t *root = &ram_disk[ROOTDIR_OFFSET];
  memset(root, 0, 32);
  uint8_t fname83[11];
  make_83_name(filename, fname83);
  memcpy(root, fname83, 11);
  root[11] = 0x20;
  *(uint16_t *)&root[26] = 2;
  *(uint32_t *)&root[28] = fileSize;

  uint16_t clusters = (fileSize + 511) / 512;
  uint8_t *fat1 = &ram_disk[FAT1_OFFSET];
  uint8_t *fat2 = &ram_disk[FAT2_OFFSET];
  for (int c = 2; c < 2 + clusters; c++) {
    uint16_t val = (c < 2 + clusters - 1) ? (c + 1) : 0xFFF;
    fat12_set(fat1, c, val);
    fat12_set(fat2, c, val);
  }
}

// ==========================================================================
// USB MSC CALLBACKS
// ==========================================================================

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

// ==========================================================================
// DISK LOAD / EJECT
// ==========================================================================

void loadDisk(const String &filename, size_t size) {
  tud_disconnect();
  delay(200);

  build_boot_sector(&ram_disk[0]);
  uint8_t *fat1 = &ram_disk[FAT1_OFFSET];
  uint8_t *fat2 = &ram_disk[FAT2_OFFSET];
  memset(fat1, 0, 4608);
  memset(fat2, 0, 4608);
  fat12_set(fat1, 0, 0xFF0);
  fat12_set(fat1, 1, 0xFFF);
  fat12_set(fat2, 0, 0xFF0);
  fat12_set(fat2, 1, 0xFFF);
  memset(&ram_disk[ROOTDIR_OFFSET], 0, 7168);

  build_fat_for_file(filename.c_str(), size);

  loaded_filename = filename;
  loaded_size = size;
  disk_present = true;

  msc.mediaPresent(true);
  tud_connect();

  Serial.println("Loaded: " + filename + " (" + String(size) + " bytes)");
  ledBlink(2, 50);
}

void ejectDisk() {
  tud_disconnect();
  delay(200);

  build_empty_volume();
  loaded_filename = "";
  loaded_size = 0;
  disk_present = false;

  msc.mediaPresent(false);
  tud_connect();

  Serial.println("Disk ejected");
  ledBlink(3, 50);
}

// Stream a file from FTP directly into RAM disk
size_t loadFileFromFTP(const String &remotePath) {
  tud_disconnect();
  delay(200);

  build_empty_volume();

  size_t maxData = RAM_DISK_SIZE - DATA_OFFSET;
  long totalRead = ftpClient.streamToBuffer(remotePath, &ram_disk[DATA_OFFSET], maxData);

  if (totalRead <= 0) {
    tud_connect();
    return 0;
  }

  // Extract filename from path
  String filename = remotePath;
  int lastSlash = filename.lastIndexOf('/');
  if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);
  if (filename.length() == 0) filename = "DISK.ADF";

  build_fat_for_file(filename.c_str(), totalRead);
  loaded_filename = filename;
  loaded_size = totalRead;
  disk_present = true;

  msc.mediaPresent(true);
  tud_connect();
  ledBlink(2, 50);
  return totalRead;
}

// Stream a file from WebDAV directly into RAM disk
size_t loadFileFromDAV(const String &remotePath) {
  tud_disconnect();
  delay(200);

  build_empty_volume();

  size_t maxData = RAM_DISK_SIZE - DATA_OFFSET;
  long totalRead = davClient.streamToBuffer(remotePath, &ram_disk[DATA_OFFSET], maxData);

  if (totalRead <= 0) {
    tud_connect();
    return 0;
  }

  String filename = remotePath;
  int lastSlash = filename.lastIndexOf('/');
  if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);
  if (filename.length() == 0) filename = "DISK.ADF";

  build_fat_for_file(filename.c_str(), totalRead);
  loaded_filename = filename;
  loaded_size = totalRead;
  disk_present = true;

  msc.mediaPresent(true);
  tud_connect();
  ledBlink(2, 50);
  return totalRead;
}

// ==========================================================================
// SHARED LIBRARY — HTTP utilities, log buffer, web UI
// ==========================================================================
#include "webui.h"
#include "../shared/http_utils.h"
#include "../shared/log_buffer.h"
#include "../shared/connectivity_api.h"

// ==========================================================================
// REQUEST HANDLER
// ==========================================================================

WiFiServer httpServer(80);

void handleRequest(WiFiClient &client) {
  client.setTimeout(5000);

  HttpRequest req;
  if (!parseRequest(client, req)) { client.stop(); return; }

  Serial.println(req.method + " " + req.path);

  // CORS preflight
  if (req.method == "OPTIONS") {
    sendResponse(client, 200, "text/plain", "");
    return;
  }

  // ── Serve Web UI (gzipped) ──
  if (req.path == "/" || req.path == "/index.html") {
    sendGzipResponse(client, "text/html", webui_html_gz, webui_html_gz_len);
    return;
  }

  // ── GET /api/status ──
  if (req.path == "/api/status" && req.method == "GET") {
    String json = "{";
    json += "\"loaded\":" + String(disk_present ? "true" : "false") + ",";
    json += "\"filename\":\"" + jsonEscape(loaded_filename) + "\",";
    json += "\"size\":" + String(loaded_size) + ",";
    json += "\"firmware\":\"" + String(FW_VERSION) + "\",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"free_psram\":" + String(ESP.getFreePsram()) + ",";
    json += "\"wifi_ap_ip\":\"" + wifi_ap_ip + "\",";
    json += "\"wifi_sta_ip\":\"" + wifi_sta_ip + "\",";
    json += "\"wifi_sta_connected\":" + String(wifi_sta_connected ? "true" : "false") + ",";
    json += "\"wifi_clients\":" + String(WiFi.softAPgetStationNum());
    json += "}";
    sendJSON(client, 200, json);
    return;
  }

  // ── POST /api/load — receive raw binary disk image ──
  if (req.path == "/api/load" && req.method == "POST") {
    if (req.contentLength <= 0) {
      sendJSON(client, 400, "{\"error\":\"No data\"}");
      return;
    }
    if (req.contentLength > (int)(RAM_DISK_SIZE - DATA_OFFSET)) {
      unsigned long t = millis();
      while (client.available() && millis() - t < 3000) { client.read(); yield(); }
      sendJSON(client, 413, "{\"error\":\"File too large. Max 1.44 MB.\"}");
      return;
    }

    String filename = req.filename;
    if (filename.length() == 0) filename = "DISK.ADF";

    ledBlink(1, 50);

    int toRead = req.contentLength;
    int pos = 0;
    unsigned long timeout = millis();

    while (pos < toRead && millis() - timeout < 30000) {
      if (client.available()) {
        int n = client.read(&ram_disk[DATA_OFFSET + pos], toRead - pos);
        if (n > 0) { pos += n; timeout = millis(); }
      } else {
        yield(); delay(1);
      }
    }

    if (pos < toRead) {
      sendJSON(client, 500, "{\"error\":\"Incomplete upload: " + String(pos) + "/" + String(toRead) + "\"}");
      return;
    }

    loadDisk(filename, pos);
    sendJSON(client, 200,
      "{\"status\":\"ok\",\"filename\":\"" + jsonEscape(filename) +
      "\",\"size\":" + String(pos) + "}");
    return;
  }

  // ── POST /api/eject ──
  if ((req.path == "/api/eject" || req.path == "/api/disk/unload") && req.method == "POST") {
    ejectDisk();
    sendJSON(client, 200, "{\"status\":\"ok\"}");
    return;
  }

  // ── GET /api/config ──
  if (req.path == "/api/config" && req.method == "GET") {
    String json = "{";
    json += "\"WIFI_SSID\":\"" + jsonEscape(cfg_wifi_ssid) + "\",";
    json += "\"WIFI_PASS\":\"" + jsonEscape(cfg_wifi_pass) + "\",";
    json += "\"WIFI_CHANNEL\":\"" + String(cfg_wifi_channel) + "\",";
    json += "\"WIFI_CLIENT_ENABLED\":\"" + String(cfg_wifi_client_enabled ? "1" : "0") + "\",";
    json += "\"WIFI_CLIENT_SSID\":\"" + jsonEscape(cfg_wifi_client_ssid) + "\",";
    json += "\"WIFI_CLIENT_PASS\":\"" + jsonEscape(cfg_wifi_client_pass) + "\",";
    json += "\"FTP_ENABLED\":\"" + String(cfg_ftp_enabled ? "1" : "0") + "\",";
    json += "\"FTP_HOST\":\"" + jsonEscape(cfg_ftp_host) + "\",";
    json += "\"FTP_PORT\":\"" + String(cfg_ftp_port) + "\",";
    json += "\"FTP_USER\":\"" + jsonEscape(cfg_ftp_user) + "\",";
    json += "\"FTP_PASS\":\"" + jsonEscape(cfg_ftp_pass) + "\",";
    json += "\"FTP_PATH\":\"" + jsonEscape(cfg_ftp_path) + "\",";
    json += "\"DAV_ENABLED\":\"" + String(cfg_dav_enabled ? "1" : "0") + "\",";
    json += "\"DAV_HOST\":\"" + jsonEscape(cfg_dav_host) + "\",";
    json += "\"DAV_PORT\":\"" + String(cfg_dav_port) + "\",";
    json += "\"DAV_USER\":\"" + jsonEscape(cfg_dav_user) + "\",";
    json += "\"DAV_PASS\":\"" + jsonEscape(cfg_dav_pass) + "\",";
    json += "\"DAV_PATH\":\"" + jsonEscape(cfg_dav_path) + "\",";
    json += "\"DAV_HTTPS\":\"" + String(cfg_dav_https ? "1" : "0") + "\",";
    json += "\"LOG_ENABLED\":\"" + String(cfg_log_enabled ? "1" : "0") + "\"";
    json += "}";
    sendJSON(client, 200, json);
    return;
  }

  // ── POST /api/config ──
  if (req.path == "/api/config" && req.method == "POST") {
    String val;

    val = getFormValue(req.body, "WIFI_SSID");
    if (val.length() > 0) cfg_wifi_ssid = val;

    val = getFormValue(req.body, "WIFI_PASS");
    if (val.length() > 0) cfg_wifi_pass = val;

    val = getFormValue(req.body, "WIFI_CHANNEL");
    if (val.length() > 0) {
      cfg_wifi_channel = (uint8_t)val.toInt();
      if (cfg_wifi_channel < 1 || cfg_wifi_channel > 13) cfg_wifi_channel = 6;
    }

    val = getFormValue(req.body, "WIFI_CLIENT_ENABLED");
    if (val.length() > 0) cfg_wifi_client_enabled = (val == "1" || val == "true");

    val = getFormValue(req.body, "WIFI_CLIENT_SSID");
    if (val.length() > 0) cfg_wifi_client_ssid = val;

    if (req.body.indexOf("WIFI_CLIENT_PASS=") >= 0)
      cfg_wifi_client_pass = getFormValue(req.body, "WIFI_CLIENT_PASS");

    val = getFormValue(req.body, "FTP_ENABLED");
    if (val.length() > 0) cfg_ftp_enabled = (val == "1" || val == "true");

    val = getFormValue(req.body, "FTP_HOST");
    if (val.length() > 0) cfg_ftp_host = val;
    else if (req.body.indexOf("FTP_HOST=") >= 0) cfg_ftp_host = "";

    val = getFormValue(req.body, "FTP_PORT");
    if (val.length() > 0) { cfg_ftp_port = val.toInt(); if (cfg_ftp_port <= 0) cfg_ftp_port = 21; }

    val = getFormValue(req.body, "FTP_USER");
    if (val.length() > 0) cfg_ftp_user = val;

    if (req.body.indexOf("FTP_PASS=") >= 0)
      cfg_ftp_pass = getFormValue(req.body, "FTP_PASS");

    val = getFormValue(req.body, "FTP_PATH");
    if (val.length() > 0) cfg_ftp_path = val;

    val = getFormValue(req.body, "DAV_ENABLED");
    if (val.length() > 0) cfg_dav_enabled = (val == "1" || val == "true");

    val = getFormValue(req.body, "DAV_HOST");
    if (val.length() > 0) cfg_dav_host = val;
    else if (req.body.indexOf("DAV_HOST=") >= 0) cfg_dav_host = "";

    val = getFormValue(req.body, "DAV_PORT");
    if (val.length() > 0) { cfg_dav_port = val.toInt(); if (cfg_dav_port <= 0) cfg_dav_port = 443; }

    val = getFormValue(req.body, "DAV_USER");
    if (val.length() > 0) cfg_dav_user = val;

    if (req.body.indexOf("DAV_PASS=") >= 0)
      cfg_dav_pass = getFormValue(req.body, "DAV_PASS");

    val = getFormValue(req.body, "DAV_PATH");
    if (val.length() > 0) cfg_dav_path = val;

    val = getFormValue(req.body, "DAV_HTTPS");
    if (val.length() > 0) cfg_dav_https = (val == "1" || val == "true");

    val = getFormValue(req.body, "LOG_ENABLED");
    if (val.length() > 0) cfg_log_enabled = (val == "1" || val == "true");

    saveConfig();
    sendJSON(client, 200, "{\"status\":\"ok\"}");
    return;
  }

  // ══════════════════════════════════════
  // FTP API ENDPOINTS
  // ══════════════════════════════════════

  if (req.path == "/api/ftp/status"     && req.method == "GET")  { handleFTPStatus(client); return; }
  if (req.path == "/api/ftp/connect"    && req.method == "POST") { handleFTPConnect(client); return; }
  if (req.path == "/api/ftp/disconnect" && req.method == "POST") { handleFTPDisconnect(client); return; }
  if (req.path == "/api/ftp/list"       && req.method == "GET")  { handleFTPList(client, getQueryParam(req.query, "path")); return; }

  // POST /api/ftp/load — Stream file from FTP directly into RAM
  if (req.path == "/api/ftp/load" && req.method == "POST") {
    if (!ftpClient.isConnected()) {
      sendJSON(client, 503, "{\"error\":\"FTP not connected\"}");
      return;
    }
    String remotePath = "";
    int pathIdx = req.body.indexOf("path=");
    if (pathIdx >= 0) {
      remotePath = req.body.substring(pathIdx + 5);
      int ampIdx = remotePath.indexOf("&");
      if (ampIdx >= 0) remotePath = remotePath.substring(0, ampIdx);
      remotePath = urlDecode(remotePath);
    }
    if (remotePath.length() == 0) {
      sendJSON(client, 400, "{\"error\":\"Missing path\"}");
      return;
    }
    size_t loaded = loadFileFromFTP(remotePath);
    if (loaded > 0) {
      sendJSON(client, 200, "{\"status\":\"ok\",\"file\":\"" + jsonEscape(remotePath) +
        "\",\"bytes\":" + String(loaded) + ",\"name\":\"" + jsonEscape(loaded_filename) + "\"}");
    } else {
      sendJSON(client, 500, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}");
    }
    return;
  }

  // ══════════════════════════════════════
  // WEBDAV API ENDPOINTS
  // ══════════════════════════════════════

  if (req.path == "/api/dav/status"     && req.method == "GET")  { handleDAVStatus(client); return; }
  if (req.path == "/api/dav/connect"    && req.method == "POST") { handleDAVConnect(client); return; }
  if (req.path == "/api/dav/disconnect" && req.method == "POST") { handleDAVDisconnect(client); return; }
  if (req.path == "/api/dav/list"       && req.method == "GET")  { handleDAVList(client, getQueryParam(req.query, "path"), req.query.indexOf("refresh=1") >= 0); return; }
  if (req.path == "/api/dav/load"       && req.method == "POST") {
    if (!cfg_dav_enabled) { sendJSON(client, 400, "{\"error\":\"WebDAV not enabled\"}"); return; }
    String remotePath = getFormValue(req.body, "path");
    if (remotePath.length() == 0) { sendJSON(client, 400, "{\"error\":\"Missing path\"}"); return; }
    size_t loaded = loadFileFromDAV(remotePath);
    if (loaded > 0) {
      sendJSON(client, 200, "{\"status\":\"ok\",\"file\":\"" + jsonEscape(remotePath) +
        "\",\"bytes\":" + String(loaded) + ",\"name\":\"" + jsonEscape(loaded_filename) + "\"}");
    } else {
      String json = "{\"error\":\"" + jsonEscape(davClient.lastError()) + "\"";
      String dbg = davClient.lastDebug();
      if (dbg.length() > 0) json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
      json += "}";
      sendJSON(client, 500, json);
    }
    return;
  }
  if (req.path == "/api/dav/cover"      && req.method == "GET")  { handleDAVCover(client, getQueryParam(req.query, "path")); return; }
  if (req.path == "/api/dav/nfo"        && req.method == "GET")  { handleDAVNfo(client, getQueryParam(req.query, "path")); return; }

  // ── GET /api/system/info ──
  if (req.path == "/api/system/info" && req.method == "GET") {
    String json = "{";
    json += "\"firmware\":\"" + String(FW_VERSION) + "\",";
    json += "\"device\":\"WiFi-Dongle\",";
    json += "\"chip\":\"ESP32-S3\",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"free_psram\":" + String(ESP.getFreePsram()) + ",";
    json += "\"total_psram\":" + String(ESP.getPsramSize()) + ",";
    json += "\"spiffs_total\":" + String(SPIFFS.totalBytes()) + ",";
    json += "\"spiffs_used\":" + String(SPIFFS.usedBytes()) + ",";
    json += "\"uptime_ms\":" + String(millis()) + ",";
    json += "\"wifi_ap_ssid\":\"" + jsonEscape(cfg_wifi_ssid) + "\",";
    json += "\"wifi_ap_ip\":\"" + wifi_ap_ip + "\",";
    json += "\"wifi_sta_ip\":\"" + wifi_sta_ip + "\",";
    json += "\"wifi_sta_connected\":" + String(wifi_sta_connected ? "true" : "false") + ",";
    json += "\"wifi_clients\":" + String(WiFi.softAPgetStationNum());
    json += "}";
    sendJSON(client, 200, json);
    return;
  }

  // ── GET /api/disk/status ──
  if (req.path == "/api/disk/status" && req.method == "GET") {
    String json = "{";
    json += "\"loaded\":" + String(disk_present ? "true" : "false") + ",";
    json += "\"filename\":\"" + jsonEscape(loaded_filename) + "\",";
    json += "\"size\":" + String(loaded_size);
    json += "}";
    sendJSON(client, 200, json);
    return;
  }

  if (req.path == "/api/wifi/status" && req.method == "GET") { handleWiFiStatus(client); return; }
  if (req.path == "/api/wifi/scan"   && req.method == "GET") { handleWiFiScan(client); return; }

  // ── GET /api/log ──
  if (req.path == "/api/log" && req.method == "GET") {
    if (!cfg_log_enabled) {
      sendJSON(client, 200, "{\"enabled\":false,\"log\":\"\"}");
      return;
    }
    String content = String(log_buf);
    sendJSON(client, 200, "{\"enabled\":true,\"log\":\"" + jsonEscape(content) + "\"}");
    return;
  }

  // ── POST /api/log/clear ──
  if (req.path == "/api/log/clear" && req.method == "POST") {
    log_buf_len = 0;
    log_buf[0] = 0;
    log_buf_wrapped = false;
    sendJSON(client, 200, "{\"status\":\"ok\"}");
    return;
  }

  // ── GET /api/themes/list ──
  if (req.path == "/api/themes/list" && req.method == "GET") {
    // Dongle has no display so no active theme stored, return static list
    sendJSON(client, 200, "{\"themes\":[\"default\",\"dark\",\"amber\",\"green\",\"matrix\"],\"active\":\"default\"}");
    return;
  }

  // ── POST /api/themes/*/activate ──
  if (req.path.startsWith("/api/themes/") && req.path.endsWith("/activate") && req.method == "POST") {
    // Dongle has no display — acknowledge but do nothing
    String theme = req.path.substring(12, req.path.length() - 9);
    sendJSON(client, 200, "{\"status\":\"ok\",\"theme\":\"" + jsonEscape(theme) + "\"}");
    return;
  }

  // ── GET /api/dav/cache/clear ──
  if ((req.path == "/api/dav/cache/clear" || req.path == "/api/dav/cache") && req.method == "DELETE") {
    davClearCache();
    sendJSON(client, 200, "{\"status\":\"ok\"}");
    return;
  }

  // 404
  sendJSON(client, 404, "{\"error\":\"Not found\"}");
}

// ==========================================================================
// WIFI SETUP
// ==========================================================================

void setupWiFi() {
  // Always start AP mode
  if (cfg_wifi_client_enabled && cfg_wifi_client_ssid.length() > 0) {
    // AP + STA dual mode
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_AP);
  }

  // Start AP
  WiFi.softAP(cfg_wifi_ssid.c_str(), cfg_wifi_pass.c_str(), cfg_wifi_channel);
  delay(200);
  wifi_ap_ip = WiFi.softAPIP().toString();
  wifi_ap_active = true;
  Serial.println("WiFi AP: " + cfg_wifi_ssid + " @ " + wifi_ap_ip);

  // Connect to home network if configured
  if (cfg_wifi_client_enabled && cfg_wifi_client_ssid.length() > 0) {
    Serial.println("WiFi STA: connecting to " + cfg_wifi_client_ssid + "...");
    WiFi.begin(cfg_wifi_client_ssid.c_str(), cfg_wifi_client_pass.c_str());

    // Wait up to 10 seconds for connection
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(250);
      ledBlink(1, 50);
    }

    if (WiFi.status() == WL_CONNECTED) {
      wifi_sta_connected = true;
      wifi_sta_ip = WiFi.localIP().toString();
      Serial.println("WiFi STA: connected! IP: " + wifi_sta_ip);
    } else {
      Serial.println("WiFi STA: connection failed, AP-only mode");
    }
  }
}

// ==========================================================================
// SETUP
// ==========================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Gotek WiFi Dongle ===");
  Serial.println("Firmware: " + String(FW_VERSION));

  // LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  ledBlink(1, 200);

  // Load saved config from NVS
  loadConfig();

  // Initialize SPIFFS for DAV cache
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed — cache disabled");
  } else {
    Serial.println("SPIFFS mounted (" + String(SPIFFS.totalBytes() / 1024) + " KB total, " + String(SPIFFS.usedBytes() / 1024) + " KB used)");
  }

  // Allocate RAM disk in PSRAM
  ram_disk = (uint8_t *)ps_malloc(RAM_DISK_SIZE);
  if (!ram_disk) {
    Serial.println("FATAL: PSRAM allocation failed!");
    while (1) { ledBlink(5, 200); delay(1000); }
  }
  build_empty_volume();
  Serial.println("RAM disk: " + String(RAM_DISK_SIZE / 1024) + " KB allocated in PSRAM");

  // WiFi (AP + optional STA)
  setupWiFi();

  // HTTP server
  httpServer.begin();
  Serial.println("Web server on port 80");

  // USB Mass Storage
  msc.vendorID("Gotek");
  msc.productID("Disk");
  msc.productRevision("2.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.mediaPresent(false);
  msc.begin(msc_block_count, 512);
  USB.begin();
  Serial.println("USB MSC ready (no disk)");

  ledBlink(3, 100);
  Serial.println("\nReady! Connect to '" + cfg_wifi_ssid + "' → http://" + wifi_ap_ip);
  if (wifi_sta_connected) {
    Serial.println("Or use http://" + wifi_sta_ip + " from your home network");
  }
}

// ==========================================================================
// LOOP
// ==========================================================================

unsigned long lastWiFiCheck = 0;

void loop() {
  WiFiClient client = httpServer.available();
  if (client) {
    unsigned long start = millis();
    while (client.connected() && !client.available()) {
      if (millis() - start > 2000) { client.stop(); return; }
      yield();
      delay(1);
    }
    if (client.available()) {
      handleRequest(client);
    }
    delay(1);
    client.stop();
  }

  // Check WiFi STA connection periodically (auto-reconnect)
  if (cfg_wifi_client_enabled && millis() - lastWiFiCheck > 15000) {
    lastWiFiCheck = millis();
    bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected && !wifi_sta_connected) {
      wifi_sta_connected = true;
      wifi_sta_ip = WiFi.localIP().toString();
      Serial.println("WiFi STA: reconnected! IP: " + wifi_sta_ip);
    } else if (!connected && wifi_sta_connected) {
      wifi_sta_connected = false;
      wifi_sta_ip = "";
      Serial.println("WiFi STA: disconnected, reconnecting...");
      WiFi.begin(cfg_wifi_client_ssid.c_str(), cfg_wifi_client_pass.c_str());
    }
  }

  delay(5);
}
