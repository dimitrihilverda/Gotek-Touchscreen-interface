#ifndef WEBSERVER_H
#define WEBSERVER_H

/*
  Gotek Touchscreen — WiFi Access Point Web Server

  Creates a WiFi AP for browser-based configuration and game management.
  Uses ESPAsyncWebServer for non-blocking request handling.

  Default AP: SSID "Gotek-Setup", password "retrogaming", channel 6
  Web UI served from PROGMEM at http://192.168.4.1/
*/

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
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
// WiFi AP State (declared before includes so api_handlers.h can access them)
// ============================================================================

AsyncWebServer webServer(80);
bool wifi_ap_active = false;
String wifi_ap_ip = "";

// Include the API handlers and embedded web UI
#include "webui.h"
#include "api_handlers.h"

// ============================================================================
// WiFi AP Management
// ============================================================================

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

bool isWiFiActive() {
  return wifi_ap_active;
}

String getAPIP() {
  return wifi_ap_ip;
}

// ============================================================================
// URL Router — parse dynamic segments from URL path
// ============================================================================
// Handles: /api/games/{mode}/{name}, /api/games/{mode}/{name}/nfo, etc.
// Called from onNotFound for routes that need dynamic path segments.

void routeRequest(AsyncWebServerRequest *request) {
  String url = request->url();
  WebRequestMethodComposite method = request->method();

  // ── /api/games/{mode}/{name}/cover ──
  if (url.startsWith("/api/games/") && url.endsWith("/cover")) {
    String rest = url.substring(11);  // after "/api/games/"
    rest = rest.substring(0, rest.length() - 6);  // remove "/cover"
    int slash = rest.indexOf('/');
    if (slash > 0) {
      String mode = rest.substring(0, slash);
      String name = rest.substring(slash + 1);
      // URL decode the name
      name.replace("%20", " ");

      if (method == HTTP_GET) {
        handleCoverServe(request, mode, name);
        return;
      }
    }
  }

  // ── /api/games/{mode}/{name}/nfo ──
  if (url.startsWith("/api/games/") && url.endsWith("/nfo")) {
    String rest = url.substring(11);
    rest = rest.substring(0, rest.length() - 4);  // remove "/nfo"
    int slash = rest.indexOf('/');
    if (slash > 0 && method == HTTP_POST) {
      String mode = rest.substring(0, slash);
      String name = rest.substring(slash + 1);
      name.replace("%20", " ");
      handleNFOUpdateParsed(request, mode, name);
      return;
    }
  }

  // ── /api/games/{mode}/{name} (detail or delete) ──
  if (url.startsWith("/api/games/") && !url.endsWith("/list") && !url.endsWith("/upload")) {
    String rest = url.substring(11);  // after "/api/games/"
    int slash = rest.indexOf('/');
    if (slash > 0) {
      String mode = rest.substring(0, slash);
      String name = rest.substring(slash + 1);
      name.replace("%20", " ");

      if ((mode == "adf" || mode == "dsk") && name.length() > 0) {
        // Don't match sub-paths like /nfo, /cover (already handled above)
        if (name.indexOf('/') < 0) {
          if (method == HTTP_GET) { handleGameDetailParsed(request, mode, name); return; }
          if (method == HTTP_DELETE) { handleGameDeleteParsed(request, mode, name); return; }
        }
      }
    }
  }

  // ── /api/themes/{name}/activate ──
  if (url.startsWith("/api/themes/") && url.endsWith("/activate")) {
    String name = url.substring(12);  // after "/api/themes/"
    name = name.substring(0, name.length() - 9);  // remove "/activate"
    name.replace("%20", " ");
    if (method == HTTP_POST && name.length() > 0) {
      handleThemeActivateParsed(request, name);
      return;
    }
  }

  // ── CORS preflight for dynamic routes ──
  if (method == HTTP_OPTIONS) {
    AsyncWebServerResponse *response = request->beginResponse(200);
    response->addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type");
    request->send(response);
    return;
  }

  // ── Not found ──
  request->send(404, "application/json", "{\"error\":\"Not found\"}");
}

// ============================================================================
// Web Server Route Registration
// ============================================================================

void registerWebRoutes() {
  // CORS headers on all responses
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  // Serve the SPA (gzipped HTML from PROGMEM)
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(
      200, "text/html", webui_html_gz, webui_html_gz_len
    );
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "max-age=86400");
    request->send(response);
  });

  // ── System Info ──
  webServer.on("/api/system/info", HTTP_GET, handleSystemInfo);

  // ── Config ──
  webServer.on("/api/config", HTTP_GET, handleConfigGet);
  webServer.on("/api/config", HTTP_POST, handleConfigPost);

  // ── Games ──
  webServer.on("/api/games/list", HTTP_GET, handleGamesList);

  // File upload (multipart) — special handler with onUpload callback
  webServer.on("/api/games/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) { handleUploadComplete(request); },
    handleFileUpload
  );

  // Upload progress
  webServer.on("/api/upload/progress", HTTP_GET, handleUploadProgress);

  // ── Themes ──
  webServer.on("/api/themes/list", HTTP_GET, handleThemesList);

  // ── Rescan SD ──
  webServer.on("/api/rescan", HTTP_POST, [](AsyncWebServerRequest *request) {
    file_list = listImages();
    buildDisplayNames(file_list);
    sortByDisplay();
    buildGameList();
    request->send(200, "application/json",
      "{\"status\":\"ok\",\"games\":" + String(game_list.size()) + "}");
  });

  // CORS preflight
  webServer.on("/api/*", HTTP_OPTIONS, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(200);
    response->addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type");
    request->send(response);
  });

  // Dynamic routes (game detail, delete, nfo, cover, theme activate)
  webServer.onNotFound(routeRequest);
}

// ============================================================================
// Start / Stop Web Server
// ============================================================================

void startWebServer() {
  registerWebRoutes();
  webServer.begin();
  Serial.println("Web server started on port 80");
}

void stopWebServer() {
  webServer.end();
  Serial.println("Web server stopped");
}

#endif // WEBSERVER_H
