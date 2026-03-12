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
// DAV CACHE — in-memory + SPIFFS persistent cache (no SD card on dongle)
// ==========================================================================
std::vector<DAVFileEntry> dav_entries;  // in-memory cache of root listing

#define DAV_CACHE_FILE "/DAV_CACHE.TXT"

void davSaveCache() {
  File f = SPIFFS.open(DAV_CACHE_FILE, "w");
  if (!f) return;
  f.println("HOST=" + cfg_dav_host);
  f.println("COUNT=" + String(dav_entries.size()));
  for (const auto &e : dav_entries) {
    if (e.isDir) {
      f.println("D|" + e.name);
    } else {
      f.println("F|" + String(e.size) + "|" + e.name + "|" + e.coverFile + "|" + e.nfoFile);
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

  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

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
      entry.name = line.substring(2);
      entry.isDir = true;
      entry.size = 0;
      entry.hasCover = false;
      entry.hasNfo = false;
      dav_entries.push_back(entry);
    } else if (line.startsWith("F|")) {
      String rest = line.substring(2);
      int p1 = rest.indexOf('|');
      if (p1 < 0) continue;
      int p2 = rest.indexOf('|', p1 + 1);
      if (p2 < 0) continue;
      int p3 = rest.indexOf('|', p2 + 1);

      DAVFileEntry entry;
      entry.isDir = false;
      entry.size = rest.substring(0, p1).toInt();
      entry.name = rest.substring(p1 + 1, p2);
      entry.hasCover = false;
      entry.hasNfo = false;
      if (p3 >= 0) {
        entry.coverFile = rest.substring(p2 + 1, p3);
        entry.nfoFile = rest.substring(p3 + 1);
        if (entry.coverFile.length() > 0) entry.hasCover = true;
        if (entry.nfoFile.length() > 0) entry.hasNfo = true;
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

// ==========================================================================
// DAV FOLDER CACHE — per-folder disk list cache in SPIFFS
// Mirrors the SD-based DAV_FOLDER_CACHE on JC3248.
// Format (per file):
//   HOST=<dav_host>
//   DISK=<filename>   (repeated for each disk file)
//   COVER=<filename>  (or empty)
//   NFO=<filename>    (or empty)
// ==========================================================================
#define DAV_FOLDER_CACHE_PREFIX "/dfc_"   // prefix + sanitised folder name

String davFolderCachePath(const String &folderPath) {
  // Sanitise: replace non-alphanum with '_', max 24 chars
  String safe = "";
  for (unsigned int i = 0; i < folderPath.length() && (int)safe.length() < 24; i++) {
    char c = folderPath[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-') {
      safe += c;
    } else {
      safe += '_';
    }
  }
  return String(DAV_FOLDER_CACHE_PREFIX) + safe + ".txt";
}

void davSaveFolderCache(const String &folderPath,
                        const std::vector<String> &disks,
                        const String &coverFile,
                        const String &nfoFile) {
  String path = davFolderCachePath(folderPath);
  File f = SPIFFS.open(path, "w");
  if (!f) return;
  f.println("HOST=" + cfg_dav_host);
  for (const auto &d : disks) f.println("DISK=" + d);
  f.println("COVER=" + coverFile);
  f.println("NFO=" + nfoFile);
  f.close();
}

bool davLoadFolderCache(const String &folderPath,
                        std::vector<String> &disks,
                        String &coverFile,
                        String &nfoFile) {
  String path = davFolderCachePath(folderPath);
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;

  disks.clear();
  coverFile = "";
  nfoFile   = "";
  bool hostOk = false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (line.startsWith("HOST=")) {
      hostOk = (line.substring(5) == cfg_dav_host);
    } else if (line.startsWith("DISK=") && hostOk) {
      String d = line.substring(5);
      if (d.length() > 0) disks.push_back(d);
    } else if (line.startsWith("COVER=") && hostOk) {
      coverFile = line.substring(6);
    } else if (line.startsWith("NFO=") && hostOk) {
      nfoFile = line.substring(4);
    }
  }
  f.close();
  if (!hostOk) { disks.clear(); coverFile = ""; nfoFile = ""; return false; }
  return true;
}

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
  delay(50);

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
  delay(50);

  build_empty_volume();
  loaded_filename = "";
  loaded_size = 0;
  disk_present = false;

  msc.mediaPresent(false);
  tud_connect();

  Serial.println("Disk ejected");
  ledBlink(3, 50);
}

// ==========================================================================
// FTP & WebDAV CLIENTS
// ==========================================================================
#include "ftp_client.h"
#include "webdav_client.h"

GotekFTP ftpClient;
GotekDAV davClient;

// Stream a file from FTP directly into RAM disk
size_t loadFileFromFTP(const String &remotePath) {
  tud_disconnect();
  delay(50);

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
  delay(50);

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
// LOG BUFFER — simple ring buffer for web-accessible log output
// ==========================================================================
#define LOG_BUF_MAX 4096
static char   log_buf[LOG_BUF_MAX];
static int    log_buf_len = 0;
static bool   log_buf_wrapped = false;

void logAppend(const String &line) {
  if (!cfg_log_enabled) return;
  String entry = line + "\n";
  int n = entry.length();
  if (n >= LOG_BUF_MAX) return;
  if (log_buf_len + n < LOG_BUF_MAX) {
    memcpy(log_buf + log_buf_len, entry.c_str(), n);
    log_buf_len += n;
  } else {
    // Wrap: discard oldest to make room
    int excess = (log_buf_len + n) - LOG_BUF_MAX + 1;
    memmove(log_buf, log_buf + excess, log_buf_len - excess);
    log_buf_len -= excess;
    log_buf_wrapped = true;
    memcpy(log_buf + log_buf_len, entry.c_str(), n);
    log_buf_len += n;
  }
  log_buf[log_buf_len] = 0;
  Serial.println(line);
}

// ==========================================================================
// HTTP HELPERS
// ==========================================================================

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 10);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

void sendResponse(WiFiClient &client, int code, const String &contentType, const String &body) {
  client.println("HTTP/1.1 " + String(code) + " OK");
  client.println("Content-Type: " + contentType);
  client.println("Content-Length: " + String(body.length()));
  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Methods: GET,POST,DELETE,OPTIONS");
  client.println("Access-Control-Allow-Headers: Content-Type,X-Filename");
  client.println("Connection: close");
  client.println();
  client.print(body);
}

void sendJSON(WiFiClient &client, int code, const String &json) {
  sendResponse(client, code, "application/json", json);
}

String urlDecode(const String &in) {
  String out;
  out.reserve(in.length());
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.length()) {
      char hi = in[i + 1];
      char lo = in[i + 2];
      int h = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
      int l = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
      if (h >= 0 && l >= 0) {
        out += (char)((h << 4) | l);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }
  return out;
}

String getFormValue(const String &body, const String &key) {
  String search = key + "=";
  int start = body.indexOf(search);
  if (start < 0) return "";
  if (start > 0 && body[start - 1] != '&') {
    search = "&" + key + "=";
    start = body.indexOf(search);
    if (start < 0) return "";
    start += 1;
  }
  start += key.length() + 1;
  int end = body.indexOf('&', start);
  String val = (end < 0) ? body.substring(start) : body.substring(start, end);
  return urlDecode(val);
}

// ==========================================================================
// WEB UI — embedded HTML (gzipped in webui.h)
// ==========================================================================
#include "webui.h"

void sendGzipResponse(WiFiClient &client, const String &contentType, const uint8_t *data, size_t len) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: " + contentType);
  client.println("Content-Encoding: gzip");
  client.println("Content-Length: " + String(len));
  client.println("Connection: close");
  client.println();

  size_t sent = 0;
  while (sent < len) {
    size_t chunk = len - sent;
    if (chunk > 2048) chunk = 2048;
    client.write(&data[sent], chunk);
    sent += chunk;
    yield();
  }
}

// ==========================================================================
// HTTP REQUEST PARSER
// ==========================================================================

struct HttpRequest {
  String method, path, query, body;
  int contentLength;
  String filename;
  String contentType;
};

bool parseRequest(WiFiClient &client, HttpRequest &req) {
  req.method = "";
  req.path = "";
  req.query = "";
  req.body = "";
  req.contentLength = 0;
  req.filename = "";
  req.contentType = "";

  String line = client.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return false;

  int sp1 = line.indexOf(' ');
  int sp2 = line.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) return false;

  req.method = line.substring(0, sp1);
  String fullPath = line.substring(sp1 + 1, sp2);

  // Split path and query string
  int qIdx = fullPath.indexOf('?');
  if (qIdx >= 0) {
    req.path = fullPath.substring(0, qIdx);
    req.query = fullPath.substring(qIdx + 1);
  } else {
    req.path = fullPath;
  }

  // Read headers
  while (client.connected()) {
    String hdr = client.readStringUntil('\n');
    hdr.trim();
    if (hdr.length() == 0) break;

    String lower = hdr;
    lower.toLowerCase();

    if (lower.startsWith("content-length:")) {
      req.contentLength = hdr.substring(15).toInt();
    } else if (lower.startsWith("x-filename:")) {
      req.filename = hdr.substring(11);
      req.filename.trim();
    } else if (lower.startsWith("content-type:")) {
      req.contentType = hdr.substring(13);
      req.contentType.trim();
    }
  }

  // Read body for POST requests (non-binary)
  if (req.method == "POST" && req.contentLength > 0 && req.contentLength < 4096 &&
      req.contentType.indexOf("octet-stream") < 0) {
    unsigned long t = millis();
    while (client.available() < req.contentLength && millis() - t < 3000) {
      yield(); delay(1);
    }
    req.body = client.readString();
  }

  return true;
}

// Helper to extract query parameter
String getQueryParam(const String &query, const String &key) {
  String search = key + "=";
  int start = query.indexOf(search);
  if (start < 0) return "";
  if (start > 0 && query[start - 1] != '&') {
    search = "&" + key + "=";
    start = query.indexOf(search);
    if (start < 0) return "";
    start += 1;
  }
  start += key.length() + 1;
  int end = query.indexOf('&', start);
  String val = (end < 0) ? query.substring(start) : query.substring(start, end);
  return urlDecode(val);
}

// ==========================================================================
// REQUEST HANDLER
// ==========================================================================

WiFiServer httpServer(80);

void handleRequest(WiFiClient &client) {
  client.setTimeout(5);

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

  if (req.path == "/api/ftp/status" && req.method == "GET") {
    String json = "{";
    json += "\"enabled\":" + String(cfg_ftp_enabled ? "true" : "false");
    json += ",\"host\":\"" + jsonEscape(cfg_ftp_host) + "\"";
    json += ",\"port\":" + String(cfg_ftp_port);
    json += ",\"user\":\"" + jsonEscape(cfg_ftp_user) + "\"";
    json += ",\"path\":\"" + jsonEscape(cfg_ftp_path) + "\"";
    json += ",\"connected\":" + String(ftpClient.isConnected() ? "true" : "false");
    json += ",\"wifi_connected\":" + String(wifi_sta_connected ? "true" : "false");
    json += "}";
    sendJSON(client, 200, json);
    return;
  }

  if (req.path == "/api/ftp/connect" && req.method == "POST") {
    if (!cfg_ftp_enabled) { sendJSON(client, 400, "{\"error\":\"FTP not enabled\"}"); return; }
    if (!wifi_sta_connected) { sendJSON(client, 503, "{\"error\":\"WiFi not connected to network\"}"); return; }
    if (ftpClient.isConnected()) ftpClient.disconnect();
    if (ftpClient.connect()) {
      sendJSON(client, 200, "{\"status\":\"connected\"}");
    } else {
      sendJSON(client, 503, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}");
    }
    return;
  }

  if (req.path == "/api/ftp/disconnect" && req.method == "POST") {
    ftpClient.disconnect();
    sendJSON(client, 200, "{\"status\":\"disconnected\"}");
    return;
  }

  if (req.path == "/api/ftp/list" && req.method == "GET") {
    if (!ftpClient.isConnected()) {
      if (cfg_ftp_enabled && wifi_sta_connected) {
        if (!ftpClient.connect()) {
          sendJSON(client, 503, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}");
          return;
        }
      } else {
        sendJSON(client, 503, "{\"error\":\"FTP not connected\"}");
        return;
      }
    }
    String path = getQueryParam(req.query, "path");
    if (path.length() == 0) path = "/";
    std::vector<FTPFileEntry> entries;
    if (!ftpClient.listDir(path, entries)) {
      sendJSON(client, 500, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}");
      return;
    }
    String json = "{\"path\":\"" + jsonEscape(path) + "\",\"entries\":[";
    for (int i = 0; i < (int)entries.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"name\":\"" + jsonEscape(entries[i].name) + "\"";
      json += ",\"dir\":" + String(entries[i].isDir ? "true" : "false");
      json += ",\"size\":" + String(entries[i].size);
      json += "}";
    }
    json += "]}";
    sendJSON(client, 200, json);
    return;
  }

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

  if (req.path == "/api/dav/status" && req.method == "GET") {
    String json = "{";
    json += "\"enabled\":" + String(cfg_dav_enabled ? "true" : "false");
    json += ",\"host\":\"" + jsonEscape(cfg_dav_host) + "\"";
    json += ",\"port\":" + String(cfg_dav_port);
    json += ",\"user\":\"" + jsonEscape(cfg_dav_user) + "\"";
    json += ",\"path\":\"" + jsonEscape(cfg_dav_path) + "\"";
    json += ",\"https\":" + String(cfg_dav_https ? "true" : "false");
    json += ",\"connected\":" + String(davClient.isConnected() ? "true" : "false");
    json += ",\"wifi_connected\":" + String(wifi_sta_connected ? "true" : "false");
    // Tell web UI if a cache exists (so it can show games without connecting first)
    bool hasCache = (dav_entries.size() > 0) || SPIFFS.exists(DAV_CACHE_FILE);
    json += ",\"has_cache\":" + String(hasCache ? "true" : "false");
    // Include now-playing state so web UI knows what's loaded
    if (disk_present && loaded_filename.length() > 0) {
      json += ",\"now_playing\":{";
      json += "\"source\":\"dav\"";
      json += ",\"name\":\"" + jsonEscape(loaded_filename) + "\"";
      json += ",\"path\":\"" + jsonEscape(loaded_filename) + "\"";
      json += "}";
    }
    String dbg = davClient.lastDebug();
    if (dbg.length() > 0) json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
    json += "}";
    sendJSON(client, 200, json);
    return;
  }

  if (req.path == "/api/dav/connect" && req.method == "POST") {
    if (!cfg_dav_enabled) { sendJSON(client, 400, "{\"error\":\"WebDAV not enabled\"}"); return; }
    if (!wifi_sta_connected) { sendJSON(client, 503, "{\"error\":\"WiFi not connected to network\"}"); return; }
    if (davClient.connect()) {
      String json = "{\"status\":\"connected\"";
      String dbg = davClient.lastDebug();
      if (dbg.length() > 0) json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
      json += "}";
      sendJSON(client, 200, json);
    } else {
      String json = "{\"error\":\"" + jsonEscape(davClient.lastError()) + "\"";
      String dbg = davClient.lastDebug();
      if (dbg.length() > 0) json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
      json += "}";
      sendJSON(client, 503, json);
    }
    return;
  }

  if (req.path == "/api/dav/disconnect" && req.method == "POST") {
    davClient.disconnect();
    sendJSON(client, 200, "{\"status\":\"disconnected\"}");
    return;
  }

  if (req.path == "/api/dav/list" && req.method == "GET") {
    if (!cfg_dav_enabled) { sendJSON(client, 400, "{\"error\":\"WebDAV not enabled\"}"); return; }

    String path = getQueryParam(req.query, "path");
    if (path.length() == 0) path = "/";
    bool forceRefresh = (req.query.indexOf("refresh=1") >= 0);

    // For root path: try returning cached data first (unless forced refresh)
    if (path == "/" && !forceRefresh) {
      // Check in-memory cache first
      if (dav_entries.size() > 0) {
        String json = "{\"path\":\"/\",\"cached\":true,\"entries\":[";
        bool first = true;
        for (int i = 0; i < (int)dav_entries.size(); i++) {
          if (!first) json += ",";
          first = false;
          json += "{\"name\":\"" + jsonEscape(dav_entries[i].name) + "\"";
          json += ",\"dir\":" + String(dav_entries[i].isDir ? "true" : "false");
          json += ",\"size\":" + String(dav_entries[i].size);
          json += "}";
        }
        json += "]}";
        sendJSON(client, 200, json);
        return;
      }
      // Try SPIFFS cache
      if (davLoadCache()) {
        String json = "{\"path\":\"/\",\"cached\":true,\"entries\":[";
        bool first = true;
        for (int i = 0; i < (int)dav_entries.size(); i++) {
          if (!first) json += ",";
          first = false;
          json += "{\"name\":\"" + jsonEscape(dav_entries[i].name) + "\"";
          json += ",\"dir\":" + String(dav_entries[i].isDir ? "true" : "false");
          json += ",\"size\":" + String(dav_entries[i].size);
          json += "}";
        }
        json += "]}";
        sendJSON(client, 200, json);
        return;
      }
    }

    // For sub-folder paths: check per-folder SPIFFS cache first
    if (path != "/" && !forceRefresh) {
      std::vector<String> cachedDisks;
      String cachedCover, cachedNfo;
      if (davLoadFolderCache(path, cachedDisks, cachedCover, cachedNfo)) {
        String json = "{\"path\":\"" + jsonEscape(path) + "\",\"cached\":true";
        if (cachedCover.length() > 0) json += ",\"cover\":\"" + jsonEscape(cachedCover) + "\"";
        if (cachedNfo.length() > 0)   json += ",\"nfo\":\"" + jsonEscape(cachedNfo) + "\"";
        json += ",\"entries\":[";
        for (int i = 0; i < (int)cachedDisks.size(); i++) {
          if (i > 0) json += ",";
          json += "{\"name\":\"" + jsonEscape(cachedDisks[i]) + "\",\"dir\":false,\"size\":0}";
        }
        json += "]}";
        sendJSON(client, 200, json);
        return;
      }
    }

    // No cache or forced refresh — need WiFi for PROPFIND
    if (!wifi_sta_connected) { sendJSON(client, 503, "{\"error\":\"WiFi not connected\"}"); return; }

    std::vector<DAVFileEntry> entries;
    if (!davClient.listDir(path, entries)) {
      sendJSON(client, 500, "{\"error\":\"" + jsonEscape(davClient.lastError()) + "\"}");
      return;
    }

    String coverFile = "", nfoFile = "";
    std::vector<String> diskFiles;
    static const char *diskExts[] = {".adf", ".dms", ".ipf", ".img", ".ima", ".st", ".adz", nullptr};
    for (int i = 0; i < (int)entries.size(); i++) {
      if (entries[i].coverFile.length() > 0 && coverFile.length() == 0) coverFile = entries[i].coverFile;
      if (entries[i].nfoFile.length() > 0 && nfoFile.length() == 0)     nfoFile   = entries[i].nfoFile;
      if (!entries[i].isDir && entries[i].coverFile.length() == 0 && entries[i].nfoFile.length() == 0) {
        String lower = entries[i].name; lower.toLowerCase();
        for (int k = 0; diskExts[k]; k++) {
          if (lower.endsWith(diskExts[k])) { diskFiles.push_back(entries[i].name); break; }
        }
      }
    }

    // Save per-folder cache if it's a subfolder
    if (path != "/") {
      davSaveFolderCache(path, diskFiles, coverFile, nfoFile);
    }

    String json = "{\"path\":\"" + jsonEscape(path) + "\"";
    if (coverFile.length() > 0) json += ",\"cover\":\"" + jsonEscape(coverFile) + "\"";
    if (nfoFile.length() > 0)   json += ",\"nfo\":\"" + jsonEscape(nfoFile) + "\"";
    json += ",\"entries\":[";
    bool first = true;
    for (int i = 0; i < (int)entries.size(); i++) {
      if (entries[i].coverFile.length() > 0 || entries[i].nfoFile.length() > 0) continue;
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"" + jsonEscape(entries[i].name) + "\"";
      json += ",\"dir\":" + String(entries[i].isDir ? "true" : "false");
      json += ",\"size\":" + String(entries[i].size);
      json += "}";
    }
    json += "]";
    String dbg = davClient.lastDebug();
    if (dbg.length() > 0) json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
    json += "}";
    sendJSON(client, 200, json);

    // Update in-memory + SPIFFS cache for root listing
    if (path == "/") {
      dav_entries.clear();
      for (int i = 0; i < (int)entries.size(); i++) {
        dav_entries.push_back(entries[i]);
      }
      davSaveCache();
    }
    return;
  }

  // POST /api/dav/load — Stream file from WebDAV directly into RAM
  if (req.path == "/api/dav/load" && req.method == "POST") {
    if (!cfg_dav_enabled) { sendJSON(client, 400, "{\"error\":\"WebDAV not enabled\"}"); return; }
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

  // GET /api/dav/cover?path= — Proxy cover image from WebDAV
  if (req.path == "/api/dav/cover" && req.method == "GET") {
    String coverPath = getQueryParam(req.query, "path");
    if (!cfg_dav_enabled || coverPath.length() == 0) {
      sendJSON(client, 400, "{\"error\":\"Invalid request\"}");
      return;
    }
    size_t maxCover = 150 * 1024;
    uint8_t *buf = (uint8_t *)ps_malloc(maxCover);
    if (!buf) { sendJSON(client, 500, "{\"error\":\"Out of PSRAM\"}"); return; }

    long bytes = davClient.streamToBuffer(coverPath, buf, maxCover);
    if (bytes <= 0) { free(buf); sendJSON(client, 404, "{\"error\":\"Cover not found\"}"); return; }

    String lp = coverPath; lp.toLowerCase();
    String ct = lp.endsWith(".png") ? "image/png" : "image/jpeg";

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: " + ct);
    client.println("Content-Length: " + String(bytes));
    client.println("Cache-Control: max-age=3600");
    client.println("Connection: close");
    client.println();

    size_t sent = 0;
    while (sent < (size_t)bytes) {
      size_t chunk = bytes - sent;
      if (chunk > 4096) chunk = 4096;
      client.write(&buf[sent], chunk);
      sent += chunk;
      yield();
    }
    free(buf);
    return;
  }

  // GET /api/dav/nfo?path= — Proxy NFO text from WebDAV
  if (req.path == "/api/dav/nfo" && req.method == "GET") {
    String nfoPath = getQueryParam(req.query, "path");
    if (!cfg_dav_enabled || nfoPath.length() == 0) {
      sendJSON(client, 400, "{\"error\":\"Invalid request\"}");
      return;
    }
    uint8_t buf[2048];
    long bytes = davClient.streamToBuffer(nfoPath, buf, sizeof(buf) - 1);
    if (bytes <= 0) { sendJSON(client, 404, "{\"error\":\"NFO not found\"}"); return; }
    buf[bytes] = 0;
    sendJSON(client, 200, "{\"nfo\":\"" + jsonEscape(String((char *)buf)) + "\"}");
    return;
  }

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

  // ── GET /api/wifi/status ──
  if (req.path == "/api/wifi/status" && req.method == "GET") {
    String json = "{";
    json += "\"ap_active\":" + String(wifi_ap_active ? "true" : "false") + ",";
    json += "\"ap_ip\":\"" + wifi_ap_ip + "\",";
    json += "\"ap_ssid\":\"" + jsonEscape(cfg_wifi_ssid) + "\",";
    json += "\"ap_clients\":" + String(WiFi.softAPgetStationNum()) + ",";
    json += "\"sta_connected\":" + String(wifi_sta_connected ? "true" : "false") + ",";
    json += "\"sta_ip\":\"" + wifi_sta_ip + "\",";
    json += "\"sta_ssid\":\"" + jsonEscape(cfg_wifi_client_ssid) + "\"";
    json += "}";
    sendJSON(client, 200, json);
    return;
  }

  // ── GET /api/wifi/scan ──
  if (req.path == "/api/wifi/scan" && req.method == "GET") {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\"";
      json += ",\"rssi\":" + String(WiFi.RSSI(i));
      json += ",\"enc\":" + String((int)WiFi.encryptionType(i));
      json += "}";
    }
    json += "]";
    WiFi.scanDelete();
    sendJSON(client, 200, json);
    return;
  }

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
