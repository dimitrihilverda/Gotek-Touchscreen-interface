#ifndef WEBSERVER_H
#define WEBSERVER_H

/*
  Gotek Touchscreen — WiFi Access Point Web Server

  Creates a WiFi AP for browser-based configuration and game management.
  Uses the built-in ESP32 WebServer (no external libraries needed).

  Default AP: SSID "Gotek-Setup", password "retrogaming", channel 6
  Web UI served from PROGMEM at http://192.168.4.1/
*/

#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>
#include <FS.h>

// Forward declarations from main sketch
extern String cfg_display, cfg_lastfile, cfg_lastmode, cfg_theme;
extern bool cfg_wifi_enabled;
extern String cfg_wifi_ssid, cfg_wifi_pass;
extern uint8_t cfg_wifi_channel;
extern String theme_path;
extern vector<String> theme_list;
extern vector<String> file_list;
extern vector<String> display_names;
extern vector<GameEntry> game_list;
extern DiskMode g_mode;
extern int selected_index;
extern int loaded_disk_index;
extern Screen current_screen;

extern void loadConfig();
extern void saveConfig();
extern void scanThemes();
extern vector<String> listImages();
extern void buildDisplayNames(const vector<String> &files);
extern void sortByDisplay();
extern void buildGameList();
extern String filenameOnly(const String &path);
extern String basenameNoExt(const String &path);

// ============================================================================
// WiFi AP State (wifi_ap_active and wifi_ap_ip defined in main .ino)
// ============================================================================

WebServer httpServer(80);
extern bool wifi_ap_active;
extern String wifi_ap_ip;

// Include the embedded web UI and API handlers
#include "webui.h"
#include "api_handlers.h"

// ============================================================================
// WiFi AP Management
// ============================================================================

// isWiFiActive() is defined in main .ino (needed before this header is included)

String getAPIP() {
  return wifi_ap_ip;
}

bool initWiFiAP() {
  if (!cfg_wifi_enabled) return false;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(cfg_wifi_ssid.c_str(), cfg_wifi_pass.c_str(), cfg_wifi_channel);

  delay(200);  // let AP stabilize
  wifi_ap_ip = WiFi.softAPIP().toString();
  wifi_ap_active = true;

  Serial.println("WiFi AP started: " + cfg_wifi_ssid);
  Serial.println("IP: " + wifi_ap_ip);

  return true;
}

void stopWiFiAP() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifi_ap_active = false;
  wifi_ap_ip = "";
  Serial.println("WiFi AP stopped");
}

// ============================================================================
// CORS helper
// ============================================================================

void sendCORS() {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.sendHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
  httpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void sendJSON(int code, const String &json) {
  sendCORS();
  httpServer.send(code, "application/json", json);
}

// ============================================================================
// URL path parser — extract segments from request URI
// ============================================================================

// Parse: /api/games/{mode}/{name}[/{action}]
// Returns true if matched, fills mode/name/action
bool parseGamePath(const String &uri, String &mode, String &name, String &action) {
  if (!uri.startsWith("/api/games/")) return false;
  String rest = uri.substring(11);  // after "/api/games/"

  int s1 = rest.indexOf('/');
  if (s1 < 0) return false;

  mode = rest.substring(0, s1);
  if (mode != "adf" && mode != "dsk") return false;

  String remainder = rest.substring(s1 + 1);
  int s2 = remainder.indexOf('/');
  if (s2 >= 0) {
    name = remainder.substring(0, s2);
    action = remainder.substring(s2 + 1);
  } else {
    name = remainder;
    action = "";
  }

  // URL decode spaces
  name.replace("%20", " ");
  return name.length() > 0;
}

// Parse: /api/themes/{name}/activate
bool parseThemeActivate(const String &uri, String &name) {
  if (!uri.startsWith("/api/themes/") || !uri.endsWith("/activate")) return false;
  name = uri.substring(12, uri.length() - 9);
  name.replace("%20", " ");
  return name.length() > 0;
}

// ============================================================================
// Web Server Route Registration
// ============================================================================

void registerWebRoutes() {

  // ── Serve the SPA (gzipped HTML from PROGMEM) ──
  httpServer.on("/", HTTP_GET, []() {
    httpServer.sendHeader("Content-Encoding", "gzip");
    httpServer.sendHeader("Cache-Control", "max-age=86400");
    httpServer.send_P(200, "text/html", (const char *)webui_html_gz, webui_html_gz_len);
  });

  // ── System Info ──
  httpServer.on("/api/system/info", HTTP_GET, handleSystemInfo);

  // ── Config ──
  httpServer.on("/api/config", HTTP_GET, handleConfigGet);
  httpServer.on("/api/config", HTTP_POST, handleConfigPost);

  // ── Games List ──
  httpServer.on("/api/games/list", HTTP_GET, handleGamesList);

  // ── File Upload (multipart) ──
  httpServer.on("/api/games/upload", HTTP_POST, handleUploadComplete, handleFileUpload);

  // ── Upload Progress ──
  httpServer.on("/api/upload/progress", HTTP_GET, handleUploadProgress);

  // ── Themes List ──
  httpServer.on("/api/themes/list", HTTP_GET, handleThemesList);

  // ── Rescan SD ──
  httpServer.on("/api/rescan", HTTP_POST, []() {
    file_list = listImages();
    buildDisplayNames(file_list);
    sortByDisplay();
    buildGameList();
    sendJSON(200, "{\"status\":\"ok\",\"games\":" + String(game_list.size()) + "}");
  });

  // ── Dynamic routes (game detail/delete/nfo/cover, theme activate) ──
  httpServer.onNotFound([]() {
    // Handle CORS preflight for all routes
    if (httpServer.method() == HTTP_OPTIONS) {
      sendCORS();
      httpServer.send(200);
      return;
    }

    String uri = httpServer.uri();
    String mode, name, action;

    // ── /api/themes/{name}/activate ──
    if (parseThemeActivate(uri, name)) {
      if (httpServer.method() == HTTP_POST) {
        handleThemeActivateParsed(name);
        return;
      }
    }

    // ── /api/games/{mode}/{name}[/{action}] ──
    if (parseGamePath(uri, mode, name, action)) {
      if (action == "cover") {
        if (httpServer.method() == HTTP_GET) { handleCoverServe(mode, name); return; }
      }
      else if (action == "nfo") {
        if (httpServer.method() == HTTP_POST) { handleNFOUpdateParsed(mode, name); return; }
      }
      else if (action == "") {
        if (httpServer.method() == HTTP_GET) { handleGameDetailParsed(mode, name); return; }
        if (httpServer.method() == HTTP_DELETE) { handleGameDeleteParsed(mode, name); return; }
      }
    }

    sendJSON(404, "{\"error\":\"Not found\"}");
  });
}

// ============================================================================
// Start / Stop Web Server
// ============================================================================

void startWebServer() {
  registerWebRoutes();
  httpServer.begin();
  Serial.println("Web server started on port 80");
}

void stopWebServer() {
  httpServer.stop();
  Serial.println("Web server stopped");
}

// Call this from loop() to process incoming HTTP requests
void handleWebServer() {
  if (wifi_ap_active) {
    httpServer.handleClient();
  }
}

#endif // WEBSERVER_H
