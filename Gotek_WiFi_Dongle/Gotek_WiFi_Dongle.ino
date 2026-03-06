/*
  Gotek WiFi Dongle — Headless USB Stick Edition

  A tiny ESP32-S3 based WiFi USB dongle that plugs into a Gotek's USB port.
  No display, no touchscreen — all control via phone/laptop web browser.

  Target board: Seeed XIAO ESP32-S3 (Sense variant recommended for microSD)
    - 21 x 17.5 mm — small enough for USB stick form factor
    - ESP32-S3 dual-core 240MHz, 8MB Flash, 8MB PSRAM, 512KB SRAM
    - WiFi 802.11 b/g/n + Bluetooth 5.0 LE
    - USB-C (USB OTG capable)
    - 11 GPIO pins: IO2-IO10, IO20, IO21

  Features:
    - USB Mass Storage emulation (FAT12 RAM disk in PSRAM)
    - WiFi AP + optional STA for internet access
    - Web UI served from PROGMEM — browse & load games from phone/laptop
    - SD card for game image storage (ADF/DSK/IMG)
    - Cover art, NFO files, multi-disk game support

  Board settings (Arduino IDE):
    Board: XIAO_ESP32S3
    USB CDC On Boot → Enabled
    PSRAM → OPI PSRAM
    Flash Size → 8MB
    Partition → Huge APP (3MB No OTA/1MB SPIFFS)

  XIAO ESP32-S3 Pinout:
    IO2  (A0)   — GPIO, ADC
    IO3  (A1)   — GPIO, ADC          ⚠ avoid (flash related)
    IO4  (A2)   — GPIO, ADC
    IO5  (A3)   — GPIO, ADC
    IO6  (SDA)  — GPIO, I2C Data
    IO7  (SCL)  — GPIO, I2C Clock
    IO8  (SCK)  — GPIO, SPI Clock
    IO9  (MISO) — GPIO, SPI Data     ⚠ avoid (bootstrap)
    IO10 (MOSI) — GPIO, SPI Data     ⚠ avoid (bootstrap)
    IO20 (RX)   — UART Receive       ⚠ avoid (USB)
    IO21 (TX)   — UART Transmit / LED

  SD Card wiring (SDMMC 1-bit mode):
    SD_CLK → IO7 (SCL)
    SD_CMD → IO8 (SCK)
    SD_D0  → IO9 (MISO)    — or use SPI mode on safe pins

  For XIAO ESP32-S3 Sense variant:
    Built-in microSD slot uses dedicated pins (no GPIO needed)

  Safe GPIO for general use: IO2, IO4, IO5, IO6, IO21
*/

#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>
#include <vector>
#include <algorithm>
#include <ctype.h>

#include <USB.h>
#include <USBMSC.h>
#include <WiFi.h>

extern "C" {
  extern bool tud_mounted(void);
  extern void tud_disconnect(void);
  extern void tud_connect(void);
  extern void* ps_malloc(size_t size);
}

#define FW_VERSION "v0.8.0-WiFiDongle"

// ==========================================================================
// HEADLESS MODE FLAG — used by shared headers to skip display calls
// ==========================================================================
#define HEADLESS_MODE 1

using std::vector;
using std::sort;
using std::swap;

// ==========================================================================
// BOARD SELECTOR
// ==========================================================================

#define BOARD_XIAO_S3        1
#define BOARD_XIAO_S3_SENSE  2
#define BOARD_GENERIC_S3     3

// SELECT YOUR BOARD HERE:
#define ACTIVE_BOARD BOARD_XIAO_S3_SENSE

// ==========================================================================
// SD Card Pin Configuration (per board)
// ==========================================================================

#if ACTIVE_BOARD == BOARD_XIAO_S3_SENSE
  // XIAO ESP32-S3 Sense — built-in microSD slot
  // Uses dedicated internal pins, no external wiring needed
  #define SD_CLK  7
  #define SD_CMD  9
  #define SD_D0   8

#elif ACTIVE_BOARD == BOARD_XIAO_S3
  // XIAO ESP32-S3 (no Sense) — external microSD breakout
  // SDMMC 1-bit mode on safe GPIO pins
  #define SD_CLK  7   // IO7 (SCL)
  #define SD_CMD  5   // IO5 (A3)
  #define SD_D0   4   // IO4 (A2)

#elif ACTIVE_BOARD == BOARD_GENERIC_S3
  // Generic ESP32-S3 dev board
  #define SD_CLK  12
  #define SD_CMD  11
  #define SD_D0   13
#endif

// ==========================================================================
// STATUS LED
// ==========================================================================
#if ACTIVE_BOARD == BOARD_XIAO_S3 || ACTIVE_BOARD == BOARD_XIAO_S3_SENSE
  #define LED_PIN 21  // XIAO ESP32-S3 built-in LED on IO21
#else
  #define LED_PIN 2   // Generic boards typically use GPIO2
#endif
bool has_led = false;

void ledInit() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  has_led = true;
}

void ledBlink(int times = 1, int ms = 100) {
  if (!has_led) return;
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(ms);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(ms);
  }
}

// ==========================================================================
// CONFIG VARIABLES
// ==========================================================================

String cfg_display = "";
String cfg_lastfile = "";
String cfg_lastmode = "";
String cfg_theme = "DEFAULT";

// WiFi config — AP (always-on hotspot)
bool   cfg_wifi_enabled = true;
String cfg_wifi_ssid    = "Gotek-Dongle";
String cfg_wifi_pass    = "retrogaming";
uint8_t cfg_wifi_channel = 6;

// WiFi config — Client (connect to home network for internet)
bool   cfg_wifi_client_enabled = false;
String cfg_wifi_client_ssid    = "";
String cfg_wifi_client_pass    = "";

// WiFi state
bool wifi_ap_active = false;
String wifi_ap_ip = "";
bool wifi_sta_connected = false;
String wifi_sta_ip = "";
bool isWiFiActive() { return wifi_ap_active; }

// Theme (kept for API compatibility, not used visually)
String theme_path = "/THEMES/DEFAULT";
vector<String> theme_list;

// RAM disk
#define RAM_DISK_SIZE (2880 * 512)
uint8_t *ram_disk = NULL;

// Disk mode
enum DiskMode { MODE_ADF = 0, MODE_DSK = 1 };
DiskMode g_mode = MODE_ADF;

// USB MSC
USBMSC msc;
uint32_t msc_block_count;

// UI state (headless — no screens, but kept for API compatibility)
enum Screen { SCR_SELECTION = 0, SCR_DETAILS = 1, SCR_INFO = 2 };
Screen current_screen = SCR_SELECTION;

// File list
vector<String> file_list;
vector<String> display_names;
int selected_index = 0;

// Game list
struct GameEntry {
  String name;
  String jpg_path;
  int first_file_index;
  int disk_count;
};
vector<GameEntry> game_list;
int game_selected = 0;
int scroll_offset = 0;

// Details (headless — no screen, but API uses these)
String detail_filename = "";
String detail_nfo_text = "";
String detail_jpg_path = "";

// Multi-disk
vector<int> disk_set;
int loaded_disk_index = -1;

// UI busy flag (used by web handlers)
bool ui_busy = false;

// ==========================================================================
// CONFIG SYSTEM
// ==========================================================================

void loadConfig() {
  cfg_display = "";
  cfg_lastfile = "";
  cfg_lastmode = "";

  File f = SD_MMC.open("/CONFIG.TXT", "r");
  if (!f) return;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;

    int eqIdx = line.indexOf('=');
    if (eqIdx < 0) continue;

    String key = line.substring(0, eqIdx);
    String val = line.substring(eqIdx + 1);
    key.trim();
    val.trim();

    if (key == "DISPLAY") cfg_display = val;
    else if (key == "LASTFILE") cfg_lastfile = val;
    else if (key == "LASTMODE") cfg_lastmode = val;
    else if (key == "THEME") cfg_theme = val;
    else if (key == "WIFI_ENABLED") cfg_wifi_enabled = (val == "1" || val == "true");
    else if (key == "WIFI_SSID") cfg_wifi_ssid = val;
    else if (key == "WIFI_PASS") cfg_wifi_pass = val;
    else if (key == "WIFI_CHANNEL") {
      cfg_wifi_channel = (uint8_t)val.toInt();
      if (cfg_wifi_channel < 1 || cfg_wifi_channel > 13) cfg_wifi_channel = 6;
    }
    else if (key == "WIFI_CLIENT_ENABLED") cfg_wifi_client_enabled = (val == "1" || val == "true");
    else if (key == "WIFI_CLIENT_SSID") cfg_wifi_client_ssid = val;
    else if (key == "WIFI_CLIENT_PASS") cfg_wifi_client_pass = val;
  }
  f.close();

  if (cfg_theme.length() == 0) cfg_theme = "DEFAULT";
  theme_path = "/THEMES/" + cfg_theme;
}

void saveConfig() {
  File f = SD_MMC.open("/CONFIG.TXT", "w");
  if (!f) return;

  if (cfg_display.length() > 0) f.println("DISPLAY=" + cfg_display);
  if (cfg_lastfile.length() > 0) f.println("LASTFILE=" + cfg_lastfile);
  if (cfg_lastmode.length() > 0) f.println("LASTMODE=" + cfg_lastmode);
  f.println("THEME=" + cfg_theme);
  f.println("WIFI_ENABLED=" + String(cfg_wifi_enabled ? "1" : "0"));
  f.println("WIFI_SSID=" + cfg_wifi_ssid);
  f.println("WIFI_PASS=" + cfg_wifi_pass);
  f.println("WIFI_CHANNEL=" + String(cfg_wifi_channel));
  f.println("WIFI_CLIENT_ENABLED=" + String(cfg_wifi_client_enabled ? "1" : "0"));
  if (cfg_wifi_client_ssid.length() > 0) {
    f.println("WIFI_CLIENT_SSID=" + cfg_wifi_client_ssid);
    f.println("WIFI_CLIENT_PASS=" + cfg_wifi_client_pass);
  }
  f.close();
}

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
      int lastSlash = name.lastIndexOf('/');
      if (lastSlash >= 0) name = name.substring(lastSlash + 1);
      if (name.length() > 0 && !name.startsWith(".")) {
        theme_list.push_back(name);
      }
    }
    entry.close();
  }
  root.close();
  sort(theme_list.begin(), theme_list.end());
  if (theme_list.empty()) theme_list.push_back("DEFAULT");
}

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

// ==========================================================================
// SD CARD INTERFACE
// ==========================================================================

void init_sd_card() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD card mount failed!");
  }
}

// Find a file by name (case-insensitive) inside a given directory
String findFileInDir(const String &dirPath, const String &targetName) {
  File dir = SD_MMC.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) return "";

  String targetUpper = targetName;
  targetUpper.toUpperCase();

  File entry;
  while ((entry = dir.openNextFile())) {
    String fname = entry.name();
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

// List disk images
vector<String> listImages() {
  vector<String> images;
  String modeDir = (g_mode == MODE_ADF) ? "/ADF" : "/DSK";
  String ext1 = (g_mode == MODE_ADF) ? ".ADF" : ".DSK";

  File root = SD_MMC.open(modeDir.c_str());
  if (root && root.isDirectory()) {
    File gameDir;
    while ((gameDir = root.openNextFile())) {
      String entryName = gameDir.name();
      if (!entryName.startsWith("/")) entryName = modeDir + "/" + entryName;

      if (gameDir.isDirectory()) {
        File entry;
        while ((entry = gameDir.openNextFile())) {
          String fname = entry.name();
          int slash = fname.lastIndexOf('/');
          if (slash >= 0) fname = fname.substring(slash + 1);
          String upper = fname;
          upper.toUpperCase();
          if (upper.endsWith(ext1) || upper.endsWith(".IMG")) {
            String fullPath = entryName + "/" + fname;
            if (!fullPath.startsWith("/")) fullPath = "/" + fullPath;
            images.push_back(fullPath);
          }
          entry.close();
        }
      } else {
        String fname = entryName;
        int slash = fname.lastIndexOf('/');
        if (slash >= 0) fname = fname.substring(slash + 1);
        String upper = fname;
        upper.toUpperCase();
        if (upper.endsWith(ext1) || upper.endsWith(".IMG")) {
          images.push_back(entryName);
        }
      }
      gameDir.close();
    }
    root.close();
  }

  // Also scan root for legacy flat layout
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

// Forward declarations
String getGameBaseName(const String &fullPath);

// Path helpers
String parentDir(const String &path) {
  int slash = path.lastIndexOf('/');
  if (slash <= 0) return "/";
  return path.substring(0, slash);
}

String filenameOnly(const String &path) {
  int slash = path.lastIndexOf('/');
  if (slash < 0) return path;
  return path.substring(slash + 1);
}

// Find NFO for a disk image
String findNFOFor(const String &imagePath) {
  String dir = parentDir(imagePath);
  String base = filenameOnly(imagePath);
  base = base.substring(0, base.lastIndexOf('.'));

  String result = findFileInDir(dir, base + ".nfo");
  if (result.length() > 0) return result;

  String gameName = getGameBaseName(imagePath);
  if (gameName != base) {
    result = findFileInDir(dir, gameName + ".nfo");
    if (result.length() > 0) return result;
  }

  result = findFileInDir(dir, "info.nfo");
  if (result.length() > 0) return result;
  return "";
}

// Find cover image for a disk image
String findJPGFor(const String &imagePath) {
  String dir = parentDir(imagePath);
  String base = filenameOnly(imagePath);
  base = base.substring(0, base.lastIndexOf('.'));

  for (const char *ext : {".jpg", ".jpeg", ".png"}) {
    String result = findFileInDir(dir, base + ext);
    if (result.length() > 0) return result;
  }

  String gameName = getGameBaseName(imagePath);
  if (gameName != base) {
    for (const char *ext : {".jpg", ".jpeg", ".png"}) {
      String result = findFileInDir(dir, gameName + ext);
      if (result.length() > 0) return result;
    }
  }

  for (const char *name : {"cover.jpg", "cover.png", "art.jpg"}) {
    String result = findFileInDir(dir, name);
    if (result.length() > 0) return result;
  }
  return "";
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

// ==========================================================================
// NFO PARSING
// ==========================================================================

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
      if (lines == 0) title = line;
      else blurb = line;
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

// Multi-disk helpers
String getGameBaseName(const String &fullPath) {
  String base = basenameNoExt(filenameOnly(fullPath));
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
  return 0;
}

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

// ==========================================================================
// DISPLAY NAMES (for sorting — no actual display)
// ==========================================================================

void buildDisplayNames(const vector<String> &files) {
  display_names.clear();
  display_names.resize(files.size());
  for (int i = 0; i < (int)files.size(); i++) {
    display_names[i] = basenameNoExt(filenameOnly(files[i]));
  }
}

void sortByDisplay() {
  // Insertion sort by display name
  for (int i = 1; i < (int)file_list.size(); i++) {
    String key = display_names[i];
    String keyLower = key;
    keyLower.toLowerCase();
    String keyFile = file_list[i];
    int j = i - 1;
    while (j >= 0) {
      String cmpLower = display_names[j];
      cmpLower.toLowerCase();
      if (cmpLower.compareTo(keyLower) <= 0) break;
      display_names[j + 1] = display_names[j];
      file_list[j + 1] = file_list[j];
      j--;
    }
    display_names[j + 1] = key;
    file_list[j + 1] = keyFile;
  }
}

// ==========================================================================
// GAME LIST
// ==========================================================================

int findGameIndex(int fileIndex) {
  for (int i = 0; i < (int)game_list.size(); i++) {
    if (game_list[i].first_file_index == fileIndex) return i;
    if (game_list[i].disk_count > 1) {
      String fileBase = getGameBaseName(file_list[fileIndex]);
      if (game_list[i].name == fileBase) return i;
    }
  }
  return 0;
}

void buildGameList() {
  game_list.clear();
  game_selected = 0;
  scroll_offset = 0;

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
      entry.name = baseName;
      int count = 1;
      for (int j = i + 1; j < (int)file_list.size(); j++) {
        if (used[j]) continue;
        if (parentDir(file_list[j]) == dir &&
            getGameBaseName(file_list[j]) == baseName &&
            getDiskNumber(file_list[j]) > 0) {
          used[j] = true;
          count++;
          if (getDiskNumber(file_list[j]) < getDiskNumber(file_list[entry.first_file_index])) {
            entry.first_file_index = j;
          }
        }
      }
      entry.disk_count = count;
    } else {
      entry.name = basenameNoExt(filenameOnly(file_list[i]));
    }

    used[i] = true;
    entry.jpg_path = findJPGFor(file_list[entry.first_file_index]);

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

  // Sort alphabetically
  for (int i = 0; i < (int)game_list.size(); i++) {
    for (int j = i + 1; j < (int)game_list.size(); j++) {
      if (game_list[i].name.compareTo(game_list[j].name) > 0) {
        swap(game_list[i], game_list[j]);
      }
    }
  }
}

// ==========================================================================
// FAT12 FILESYSTEM EMULATION
// ==========================================================================

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
  buf[0x24] = 0x00;
  buf[0x25] = 0x00;
  buf[0x26] = 0x29;
  buf[0x27] = 0x47;
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
}

void make_83_name(const char *src, uint8_t *dst) {
  memset(dst, ' ', 11);
  const char *dot = strrchr(src, '.');
  int nameLen = dot ? (int)(dot - src) : (int)strlen(src);
  for (int i = 0, j = 0; i < nameLen && j < 8; i++) {
    dst[j++] = toupper(src[i]);
  }
  if (dot) {
    dot++;
    for (int j = 8; *dot && j < 11; dot++) {
      dst[j++] = toupper(*dot);
    }
  }
}

void build_root(uint8_t *root) {
  memset(root, 0, 7168);
  uint8_t fname[11];
  make_83_name(getOutputFilename().c_str(), fname);
  memcpy(&root[0], fname, 11);
  root[11] = 0x20;
  *(uint16_t *)&root[26] = 0;
  *(uint32_t *)&root[28] = 0;
}

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
// FILE LOADING (Headless — no display, just Serial logging)
// ==========================================================================

size_t loadFileToRam(int index) {
  if (index < 0 || index >= (int)file_list.size()) return 0;

  String filepath = file_list[index];
  Serial.println("Loading: " + filepath);

  // LED feedback
  ledBlink(1, 50);

  build_volume_with_file();

  File f = SD_MMC.open(filepath.c_str(), "r");
  if (!f) {
    Serial.println("Error opening file!");
    return 0;
  }

  size_t fileSize = f.size();
  size_t maxData = RAM_DISK_SIZE - DATA_OFFSET;
  size_t toRead = (fileSize < maxData) ? fileSize : maxData;
  size_t totalRead = 0;

  while (totalRead < toRead) {
    size_t chunk = toRead - totalRead;
    if (chunk > 32768) chunk = 32768;
    size_t got = f.read(&ram_disk[DATA_OFFSET + totalRead], chunk);
    if (got == 0) break;
    totalRead += got;
    yield();
  }
  f.close();

  // Build FAT chain
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

  Serial.println("Loaded: " + String(totalRead) + " bytes");
  ledBlink(2, 50);

  return totalRead;
}

void doUnload() {
  tud_disconnect();
  delay(50);

  build_volume_with_file();
  msc.mediaPresent(false);

  tud_connect();

  cfg_lastfile = "";
  loaded_disk_index = -1;
  saveConfig();

  Serial.println("Drive unloaded");
  ledBlink(3, 50);
}

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

// ==========================================================================
// HEADLESS STUBS — these are called by api_handlers.h / webserver.h
// In headless mode they do nothing (no display to update)
// ==========================================================================

void drawDetailsFromNFO(const String &filename) {
  // No display — just update internal state for API consistency
  detail_filename = filename;
}

void drawList() {
  // No display
}

// ==========================================================================
// WiFi Web Server (include after all game/theme functions are defined)
// ==========================================================================
#include "webserver.h"

// ==========================================================================
// SETUP
// ==========================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Gotek WiFi Dongle starting...");
  Serial.println("Firmware: " + String(FW_VERSION));

  // Status LED
  ledInit();
  ledBlink(1, 200);

  // SD card
  init_sd_card();
  Serial.println("SD card initialized");

  // Config
  loadConfig();
  scanThemes();

  if (cfg_lastmode == "DSK") {
    g_mode = MODE_DSK;
  } else {
    g_mode = MODE_ADF;
  }

  // Scan games
  file_list = listImages();
  buildDisplayNames(file_list);
  sortByDisplay();
  buildGameList();
  Serial.println("Found " + String(file_list.size()) + " images (" + String(game_list.size()) + " games)");

  // RAM disk
  ram_disk = (uint8_t *)ps_malloc(RAM_DISK_SIZE);
  if (!ram_disk) {
    Serial.println("FATAL: RAM disk allocation failed!");
    while (1) { ledBlink(5, 200); delay(1000); }
  }

  build_volume_with_file();
  Serial.println("RAM disk initialized");

  // Auto-load last file
  bool autoloaded = false;
  if (cfg_lastfile.length() > 0) {
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

  // WiFi
  if (cfg_wifi_enabled) {
    if (initWiFiAP()) {
      startWebServer();
      Serial.println("Web server ready at http://" + wifi_ap_ip);
    }
  }

  // USB MSC
  msc.vendorID("Gotek");
  msc.productID("Disk");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.mediaPresent(autoloaded);
  msc.begin(msc_block_count, 512);
  USB.begin();
  Serial.println("USB MSC initialized");

  ledBlink(3, 100);
  Serial.println("Setup complete! Connect to WiFi '" + cfg_wifi_ssid + "' to manage games.");
}

// ==========================================================================
// LOOP — headless, just handle web requests
// ==========================================================================

void loop() {
  handleWebServer();
  delay(10);
}
