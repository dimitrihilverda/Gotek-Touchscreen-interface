#pragma once
//
// Settings and logging for a device with no SD card.
//
// The touchscreen keeps its configuration in CONFIG.TXT, which you can edit by
// putting the card in a laptop. A dongle has no card and no screen, so the
// settings live in NVS and the only way to reach them is the web interface —
// which means getting them wrong locks you out of nothing, but losing them
// means retyping a WiFi password on a phone. Worth persisting properly.
//
// This header also owns the log, because it has to exist before webdav_client.h
// is included: that client logs through sdLog(), and on this board there is no
// card to log to.

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

// ── The log ──────────────────────────────────────────────────────────────
//
// A ring buffer in RAM, served at /api/log. On a board whose only connector is
// the port it hands to the Gotek, and where the serial port does not exist
// until USB.begin(), this is the entire diagnostic surface.
// In RTC memory, and NOINIT specifically, so it survives a crash.
//
// A log that lives in ordinary RAM is wiped by the very reboot you are trying
// to explain: you come back to nothing but the startup lines, which looks like
// "logging is broken" and is actually "the device restarted". RTC_NOINIT is
// kept across esp_restart() and a panic, and only lost on real power removal.
#define LOG_LINES     40
#define LOG_LINE_LEN  96
#define LOG_MAGIC     0x60EC1067u

RTC_NOINIT_ATTR static char     g_log[LOG_LINES][LOG_LINE_LEN];
RTC_NOINIT_ATTR static uint8_t  g_logHead;
RTC_NOINIT_ATTR static uint8_t  g_logCount;
RTC_NOINIT_ATTR static uint32_t g_logMagic;

// Call once at boot, before anything logs.
void logBegin() {
  if (g_logMagic != LOG_MAGIC) {      // cold start: the buffer is garbage
    g_logMagic = LOG_MAGIC;
    g_logHead = 0;
    g_logCount = 0;
    memset(g_log, 0, sizeof(g_log));
  }
  if (g_logHead >= LOG_LINES) g_logHead = 0;      // paranoia against garbage
  if (g_logCount > LOG_LINES) g_logCount = LOG_LINES;
}

// Why we are running. A panic here is the difference between "the transfer is
// slow" and "the device died and took the explanation with it".
const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "PANIC (crash)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power dipped)";
    case ESP_RST_USB:      return "USB reset";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    default:               return "unknown";
  }
}

void dlog(const String &msg) {
  Serial.println(msg);
  strncpy(g_log[g_logHead], msg.c_str(), LOG_LINE_LEN - 1);
  g_log[g_logHead][LOG_LINE_LEN - 1] = 0;
  g_logHead = (g_logHead + 1) % LOG_LINES;
  if (g_logCount < LOG_LINES) g_logCount++;
}

// webdav_client.h logs through this name. Same destination here.
void sdLog(const String &msg) { dlog(msg); }

// ── Settings ─────────────────────────────────────────────────────────────
// The cfg_dav_* names are what webdav_client.h reads; do not rename them.
String  cfg_dav_host    = "";
uint16_t cfg_dav_port   = 443;
bool    cfg_dav_https   = true;
String  cfg_dav_user    = "";
String  cfg_dav_pass    = "";
String  cfg_dav_path    = "/";
bool    cfg_dav_enabled = false;

String  cfg_wifi_ssid   = "Gotek-Dongle";
String  cfg_wifi_pass   = "retrogaming";
bool    cfg_wifi_client_enabled = false;
String  cfg_wifi_client_ssid = "";
String  cfg_wifi_client_pass = "";

// The radio is the largest load on a port that is also feeding the Gotek, and a
// thirty-year-old Amiga 5V rail does not hold up under a full-power burst.
// There is no backlight to dim on a dongle, so this is the only lever left.
int     cfg_wifi_tx_dbm = 15;

// LAN name, as <name>.local. Deliberately NOT "gotek": that is the
// touchscreen's default, and a dongle and a panel on one network both
// claiming it made which-device-answers a coin flip. Set gotek1/gotek2/...
// when running several dongles.
String  cfg_mdns_name = "gotek-dongle";

static Preferences prefs;
#define CFG_NS "gotek"

void loadConfig() {
  // Factory-unique defaults, derived before NVS is even opened — a truly
  // fresh chip takes the early return below and must already be unique by
  // then. Two dongles out of the box both called gotek-dongle.local, both
  // broadcasting "Gotek-Dongle", is the mDNS coin flip twice over: the last
  // two MAC octets make every device distinct, and anything the user ever
  // saved simply overrides these through the reads below.
  {
    char suf[6];
    snprintf(suf, sizeof(suf), "%04X",
             (unsigned)((ESP.getEfuseMac() >> 32) & 0xFFFF));
    cfg_mdns_name = "gotek-" + String(suf);
    cfg_wifi_ssid = "Gotek-Dongle-" + String(suf);
  }

  if (!prefs.begin(CFG_NS, true)) {   // read-only; absent on a fresh chip
    dlog("Config: nothing stored yet, using defaults");
    return;
  }
  cfg_dav_host    = prefs.getString("dav_host", cfg_dav_host);
  cfg_dav_port    = prefs.getUShort("dav_port", cfg_dav_port);
  cfg_dav_https   = prefs.getBool  ("dav_https", cfg_dav_https);
  cfg_dav_user    = prefs.getString("dav_user", cfg_dav_user);
  cfg_dav_pass    = prefs.getString("dav_pass", cfg_dav_pass);
  cfg_dav_path    = prefs.getString("dav_path", cfg_dav_path);
  cfg_dav_enabled = prefs.getBool  ("dav_on",   cfg_dav_enabled);

  cfg_wifi_ssid   = prefs.getString("ap_ssid",  cfg_wifi_ssid);
  cfg_wifi_pass   = prefs.getString("ap_pass",  cfg_wifi_pass);
  cfg_wifi_client_enabled = prefs.getBool("sta_on", cfg_wifi_client_enabled);
  cfg_wifi_client_ssid    = prefs.getString("sta_ssid", cfg_wifi_client_ssid);
  cfg_wifi_client_pass    = prefs.getString("sta_pass", cfg_wifi_client_pass);
  cfg_wifi_tx_dbm = prefs.getInt("tx_dbm", cfg_wifi_tx_dbm);
  cfg_mdns_name   = prefs.getString("mdns", cfg_mdns_name);

  prefs.end();
  dlog("Config loaded from NVS");
}

void saveConfig() {
  if (!prefs.begin(CFG_NS, false)) {
    dlog("Config: NVS write failed");
    return;
  }
  prefs.putString("dav_host", cfg_dav_host);
  prefs.putUShort("dav_port", cfg_dav_port);
  prefs.putBool  ("dav_https", cfg_dav_https);
  prefs.putString("dav_user", cfg_dav_user);
  prefs.putString("dav_pass", cfg_dav_pass);
  prefs.putString("dav_path", cfg_dav_path);
  prefs.putBool  ("dav_on",   cfg_dav_enabled);

  prefs.putString("ap_ssid",  cfg_wifi_ssid);
  prefs.putString("ap_pass",  cfg_wifi_pass);
  prefs.putBool  ("sta_on",   cfg_wifi_client_enabled);
  prefs.putString("sta_ssid", cfg_wifi_client_ssid);
  prefs.putString("sta_pass", cfg_wifi_client_pass);
  prefs.putInt   ("tx_dbm",   cfg_wifi_tx_dbm);
  prefs.putString("mdns",     cfg_mdns_name);
  prefs.end();
  dlog("Config saved to NVS");
}
