#pragma once
/*
  shared/connectivity_api.h
  ==========================
  API route handlers shared by ALL Gotek device targets.

  Covers:
    - FTP status / connect / disconnect / list / load (stream to RAM)
    - WebDAV status / connect / disconnect / list / load / cover / nfo
    - WiFi status / scan
    - Log GET / clear
    - Config GET / POST (connectivity fields only — device-specific fields
      are handled in the device's own handler after calling
      applyConnectivityConfig())

  Requires (defined in device .ino before including this file):
    - http_utils.h       — sendJSON, jsonEscape, getFormValue, getQueryParam, etc.
    - log_buffer.h       — logAppend, logGetContents, logClear (dongle)
                           OR sdLog() and file-based log (JC3248)
    - dav_folder_cache.h — davSaveFolderCache / davLoadFolderCache
    - ftp_client.h, webdav_client.h
    - GotekFTP ftpClient; GotekDAV davClient;
    - std::vector<DAVFileEntry> dav_entries;
    - davSaveCache() / davLoadCache()
    - extern String cfg_dav_host, cfg_dav_user, cfg_dav_pass, cfg_dav_path
    - extern bool   cfg_dav_enabled, cfg_dav_https, cfg_ftp_enabled
    - extern String cfg_ftp_host, cfg_ftp_user, cfg_ftp_pass, cfg_ftp_path
    - extern int    cfg_dav_port, cfg_ftp_port
    - extern bool   cfg_log_enabled
    - extern bool   wifi_sta_connected
    - extern String wifi_ap_ip, wifi_sta_ip, wifi_ap_ssid_display (or cfg_wifi_ssid)
    - extern bool   wifi_ap_active
    - void saveConfig()
    - extern "C" void* ps_malloc(size_t)

  Device-specific note:
    JC3248 DAV cover handler uses SD cover cache (davReadCachedCover /
    davSaveCachedCover). Define DEVICE_HAS_SD_COVER_CACHE to enable that path;
    otherwise covers are proxied directly from WebDAV.
*/

#include <vector>
#include "ftp_client.h"
#include "webdav_client.h"

// ─────────────────────────────────────────────────────────────────────────────
// Disk extension list — shared between DAV list handler and folder cache
// ─────────────────────────────────────────────────────────────────────────────
static const char * const kDiskExts[] = {
  ".adf", ".dsk", ".adz", ".img", ".ima", ".st", ".ipf", nullptr
};

inline bool isDiskFile(const String &name) {
  String lower = name; lower.toLowerCase();
  for (int i = 0; kDiskExts[i]; i++) {
    if (lower.endsWith(kDiskExts[i])) return true;
  }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// SHARED CONFIG FIELDS — FTP + DAV + LOG (subset of config GET/POST)
// Call from the device's handleConfigGet() to add connectivity fields.
// ─────────────────────────────────────────────────────────────────────────────

inline String connectivityConfigJson() {
  String json = "";
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
  return json;
}

// Apply connectivity fields from a POST body.
// Call from device's handleConfigPost(). Returns true if any DAV field changed
// (so device can invalidate the cache if needed).
inline bool applyConnectivityConfig(const String &body) {
  String val;
  bool davChanged = false;

  val = getFormValue(body, "FTP_ENABLED");
  if (val.length() > 0) cfg_ftp_enabled = (val == "1" || val == "true");

  val = getFormValue(body, "FTP_HOST");
  if (val.length() > 0) cfg_ftp_host = val;
  else if (body.indexOf("FTP_HOST=") >= 0) cfg_ftp_host = "";

  val = getFormValue(body, "FTP_PORT");
  if (val.length() > 0) { cfg_ftp_port = val.toInt(); if (cfg_ftp_port <= 0) cfg_ftp_port = 21; }

  val = getFormValue(body, "FTP_USER");
  if (val.length() > 0) cfg_ftp_user = val;

  if (body.indexOf("FTP_PASS=") >= 0)
    cfg_ftp_pass = getFormValue(body, "FTP_PASS");

  val = getFormValue(body, "FTP_PATH");
  if (val.length() > 0) cfg_ftp_path = val;

  val = getFormValue(body, "DAV_ENABLED");
  if (val.length() > 0) cfg_dav_enabled = (val == "1" || val == "true");

  val = getFormValue(body, "DAV_HOST");
  if (val.length() > 0) { if (val != cfg_dav_host) davChanged = true; cfg_dav_host = val; }
  else if (body.indexOf("DAV_HOST=") >= 0) { davChanged = true; cfg_dav_host = ""; }

  val = getFormValue(body, "DAV_PORT");
  if (val.length() > 0) { cfg_dav_port = val.toInt(); if (cfg_dav_port <= 0) cfg_dav_port = 443; }

  val = getFormValue(body, "DAV_USER");
  if (val.length() > 0) cfg_dav_user = val;

  if (body.indexOf("DAV_PASS=") >= 0)
    cfg_dav_pass = getFormValue(body, "DAV_PASS");

  val = getFormValue(body, "DAV_PATH");
  if (val.length() > 0) cfg_dav_path = val;

  val = getFormValue(body, "DAV_HTTPS");
  if (val.length() > 0) cfg_dav_https = (val == "1" || val == "true");

  val = getFormValue(body, "LOG_ENABLED");
  if (val.length() > 0) cfg_log_enabled = (val == "1" || val == "true");

  return davChanged;
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /api/wifi/status
// ─────────────────────────────────────────────────────────────────────────────

inline void handleWiFiStatus(WiFiClient &client) {
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
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /api/wifi/scan
// ─────────────────────────────────────────────────────────────────────────────

inline void handleWiFiScan(WiFiClient &client) {
  int n = WiFi.scanNetworks(false, false);
  String json = "{\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"encrypted\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]}";
  WiFi.scanDelete();
  sendJSON(client, 200, json);
}

// ─────────────────────────────────────────────────────────────────────────────
// FTP handlers
// ─────────────────────────────────────────────────────────────────────────────

inline void handleFTPStatus(WiFiClient &client) {
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
}

inline void handleFTPConnect(WiFiClient &client) {
  if (!cfg_ftp_enabled) { sendJSON(client, 400, "{\"error\":\"FTP not enabled in config\"}"); return; }
  if (!wifi_sta_connected) { sendJSON(client, 503, "{\"error\":\"WiFi client not connected to network\"}"); return; }
  if (ftpClient.isConnected()) ftpClient.disconnect();
  if (ftpClient.connect()) {
    sendJSON(client, 200, "{\"status\":\"connected\"}");
  } else {
    sendJSON(client, 503, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}");
  }
}

inline void handleFTPDisconnect(WiFiClient &client) {
  ftpClient.disconnect();
  sendJSON(client, 200, "{\"status\":\"disconnected\"}");
}

inline void handleFTPList(WiFiClient &client, const String &queryPath) {
  if (!ftpClient.isConnected()) {
    if (cfg_ftp_enabled && wifi_sta_connected) {
      if (!ftpClient.connect()) { sendJSON(client, 503, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}"); return; }
    } else {
      sendJSON(client, 503, "{\"error\":\"FTP not connected\"}"); return;
    }
  }
  String path = queryPath.length() > 0 ? queryPath : "/";
  std::vector<FTPFileEntry> entries;
  if (!ftpClient.listDir(path, entries)) {
    sendJSON(client, 500, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}"); return;
  }
  String json = "{\"path\":\"" + jsonEscape(path) + "\",\"entries\":[";
  for (int i = 0; i < (int)entries.size(); i++) {
    if (i > 0) json += ",";
    json += "{\"name\":\"" + jsonEscape(entries[i].name) + "\"";
    json += ",\"dir\":" + String(entries[i].isDir ? "true" : "false");
    json += ",\"size\":" + String(entries[i].size) + "}";
  }
  json += "]}";
  sendJSON(client, 200, json);
}

// ─────────────────────────────────────────────────────────────────────────────
// WebDAV handlers
// ─────────────────────────────────────────────────────────────────────────────

inline void handleDAVStatus(WiFiClient &client) {
  String json = "{";
  json += "\"enabled\":" + String(cfg_dav_enabled ? "true" : "false");
  json += ",\"host\":\"" + jsonEscape(cfg_dav_host) + "\"";
  json += ",\"port\":" + String(cfg_dav_port);
  json += ",\"user\":\"" + jsonEscape(cfg_dav_user) + "\"";
  json += ",\"path\":\"" + jsonEscape(cfg_dav_path) + "\"";
  json += ",\"https\":" + String(cfg_dav_https ? "true" : "false");
  json += ",\"connected\":" + String(davClient.isConnected() ? "true" : "false");
  json += ",\"wifi_connected\":" + String(wifi_sta_connected ? "true" : "false");
  String dbg = davClient.lastDebug();
  if (dbg.length() > 0) json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
  String err = davClient.lastError();
  if (err.length() > 0) json += ",\"error\":\"" + jsonEscape(err) + "\"";
  // Let web UI know a cache exists so it can show games without connecting
  bool hasCache = (dav_entries.size() > 0) || davCacheExists();
  json += ",\"has_cache\":" + String(hasCache ? "true" : "false");
  // Device-specific now-playing injected via DAV_STATUS_EXTRA if defined
#ifdef DAV_STATUS_EXTRA
  DAV_STATUS_EXTRA(json);
#endif
  json += "}";
  sendJSON(client, 200, json);
}

inline void handleDAVConnect(WiFiClient &client) {
  if (!cfg_dav_enabled) { sendJSON(client, 400, "{\"error\":\"WebDAV not enabled in config\"}"); return; }
  if (!wifi_sta_connected) { sendJSON(client, 503, "{\"error\":\"WiFi client not connected to network\"}"); return; }
  logAppend("DAV connect requested — heap=" + String(ESP.getFreeHeap()) + " stack=" + String(uxTaskGetStackHighWaterMark(NULL)));
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
}

inline void handleDAVDisconnect(WiFiClient &client) {
  davClient.disconnect();
  sendJSON(client, 200, "{\"status\":\"disconnected\"}");
}

// handleDAVList — root + subfolder listing
// Root: served from in-memory cache or SD cache, falls back to PROPFIND.
// Subfolder: served from indexed dav_entries (full paths), prioritises
//            background indexing of this folder if not yet done,
//            falls back to live PROPFIND only when truly needed.
inline void handleDAVList(WiFiClient &client, const String &queryPath, bool forceRefresh) {
  if (!cfg_dav_enabled) { sendJSON(client, 400, "{\"error\":\"WebDAV not enabled\"}"); return; }

  String path = queryPath.length() > 0 ? queryPath : "/";

  // ── Helper: build root JSON (includes disk count per folder) ──────────
  auto buildRootJson = [&]() {
    String json = "{\"path\":\"/\",\"cached\":true,\"entries\":[";
    bool first = true;
    for (int i = 0; i < (int)dav_entries.size(); i++) {
      if (!dav_entries[i].isDir) continue;
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"" + jsonEscape(dav_entries[i].name) + "\"";
      json += ",\"dir\":true";
      int dc = dav_entries[i].diskPaths.size() > 0 ? (int)dav_entries[i].diskPaths.size() : dav_entries[i].diskCount;
      json += ",\"disks\":" + String(dc);
      json += ",\"indexed\":" + String(dav_entries[i].indexed ? "true" : "false");
      json += ",\"hasCover\":" + String(dav_entries[i].hasCover ? "true" : "false");
      if (dav_entries[i].coverPath.length() > 0)
        json += ",\"coverPath\":\"" + jsonEscape(dav_entries[i].coverPath) + "\"";
      json += "}";
    }
    return json + "]}";
  };

  // ── Root listing ──────────────────────────────────────────────────────
  if (path == "/" && !forceRefresh) {
    if (dav_entries.size() > 0) { sendJSON(client, 200, buildRootJson()); return; }
    if (davLoadCache())          { sendJSON(client, 200, buildRootJson()); return; }
  }

  // ── Subfolder listing ─────────────────────────────────────────────────
  if (path != "/" && !forceRefresh) {
    // Extract folder name from path (last component)
    String folderName = path;
    if (folderName.endsWith("/")) folderName = folderName.substring(0, folderName.length() - 1);
    int ls = folderName.lastIndexOf('/');
    if (ls >= 0) folderName = folderName.substring(ls + 1);

    // Find in dav_entries
    for (int i = 0; i < (int)dav_entries.size(); i++) {
      if (!dav_entries[i].isDir || dav_entries[i].name != folderName) continue;
      DAVFileEntry &e = dav_entries[i];

      if (e.indexed) {
        // Fully cached — return immediately
        String json = "{\"path\":\"" + jsonEscape(path) + "\",\"cached\":true";
        if (e.coverPath.length() > 0) json += ",\"cover\":\"" + jsonEscape(e.coverPath) + "\"";
        if (e.nfoPath.length() > 0)   json += ",\"nfo\":\"" + jsonEscape(e.nfoPath) + "\"";
        json += ",\"entries\":[";
        bool first = true;
        for (const auto &dp : e.diskPaths) {
          if (!first) json += ",";
          first = false;
          // Extract filename from full path
          String fname = dp;
          int sl = fname.lastIndexOf('/');
          if (sl >= 0) fname = fname.substring(sl + 1);
          json += "{\"name\":\"" + jsonEscape(fname) + "\"";
          json += ",\"path\":\"" + jsonEscape(dp) + "\"";
          json += ",\"dir\":false,\"size\":0}";
        }
        sendJSON(client, 200, json + "]}");
        return;
      } else {
        // Not yet indexed — bump to front of background indexer queue
#ifdef ON_DAV_FOLDER_PRIORITY
        ON_DAV_FOLDER_PRIORITY(folderName);
#endif
        // Fall through to live PROPFIND below
        break;
      }
    }
  }

  // ── Live PROPFIND (root force-refresh, or unindexed subfolder) ────────
  if (!wifi_sta_connected) { sendJSON(client, 503, "{\"error\":\"WiFi not connected\"}"); return; }

  std::vector<DAVFileEntry> entries;
  if (!davClient.listDir(path, entries)) {
    sendJSON(client, 500, "{\"error\":\"" + jsonEscape(davClient.lastError()) + "\"}"); return;
  }

  if (path == "/") {
    // Root refresh — reset entries (keep indexed data if name matches)
    std::vector<DAVFileEntry> old = dav_entries;
    dav_entries.clear();
    for (int i = 0; i < (int)entries.size(); i++) {
      if (!entries[i].isDir) continue;
      // Preserve indexed data from old cache if name matches
      bool found = false;
      for (const auto &o : old) {
        if (o.isDir && o.name == entries[i].name && o.indexed) {
          dav_entries.push_back(o);
          found = true;
          break;
        }
      }
      if (!found) dav_entries.push_back(entries[i]);
    }
    davSaveCache();
    sendJSON(client, 200, buildRootJson());
#ifdef ON_DAV_ROOT_LOADED
    ON_DAV_ROOT_LOADED();
#endif
    return;
  }

  // Subfolder live PROPFIND result — find cover/nfo/disks
  // Prefer entry.href (authoritative full path from PROPFIND) over constructing path.
  String coverPath = "", nfoPath = "";
  std::vector<String> diskPaths;
  for (int i = 0; i < (int)entries.size(); i++) {
    if (entries[i].isDir) continue;
    String lname = entries[i].name; lname.toLowerCase();
    String fullPath = entries[i].href.length() > 0
                      ? entries[i].href
                      : (path + (path.endsWith("/") ? "" : "/") + entries[i].name);
    if ((lname.endsWith(".jpg") || lname.endsWith(".jpeg") || lname.endsWith(".png"))
        && coverPath.length() == 0) {
      coverPath = fullPath;
    } else if (lname.endsWith(".nfo") && nfoPath.length() == 0) {
      nfoPath = fullPath;
    } else if (isDiskFile(entries[i].name)) {
      diskPaths.push_back(fullPath);
    }
  }

  // Store result in dav_entries
  String folderName2 = path;
  if (folderName2.endsWith("/")) folderName2 = folderName2.substring(0, folderName2.length() - 1);
  int ls2 = folderName2.lastIndexOf('/');
  if (ls2 >= 0) folderName2 = folderName2.substring(ls2 + 1);
  for (int i = 0; i < (int)dav_entries.size(); i++) {
    if (dav_entries[i].isDir && dav_entries[i].name == folderName2) {
      dav_entries[i].coverPath = coverPath;
      dav_entries[i].nfoPath   = nfoPath;
      dav_entries[i].diskPaths = diskPaths;
      dav_entries[i].diskCount = (int)diskPaths.size();
      dav_entries[i].hasCover  = coverPath.length() > 0;
      dav_entries[i].hasNfo    = nfoPath.length() > 0;
      dav_entries[i].indexed   = true;
      break;
    }
  }
  davSaveCache();

  // Build response
  String json = "{\"path\":\"" + jsonEscape(path) + "\"";
  if (coverPath.length() > 0) json += ",\"cover\":\"" + jsonEscape(coverPath) + "\"";
  if (nfoPath.length() > 0)   json += ",\"nfo\":\"" + jsonEscape(nfoPath) + "\"";
  json += ",\"entries\":[";
  bool first = true;
  for (const auto &dp : diskPaths) {
    if (!first) json += ",";
    first = false;
    String fname = dp; int sl = fname.lastIndexOf('/'); if (sl >= 0) fname = fname.substring(sl + 1);
    json += "{\"name\":\"" + jsonEscape(fname) + "\",\"path\":\"" + jsonEscape(dp) + "\",\"dir\":false,\"size\":0}";
  }
  String dbg = davClient.lastDebug();
  json += (dbg.length() > 0) ? "],\"debug\":\"" + jsonEscape(dbg) + "\"}" : "]}";
  sendJSON(client, 200, json);
}

// GET /api/dav/nfo?path=... — proxy NFO text
inline void handleDAVNfo(WiFiClient &client, const String &queryPath) {
  if (!cfg_dav_enabled || queryPath.length() == 0) {
    sendJSON(client, 400, "{\"error\":\"Invalid request\"}"); return;
  }
  uint8_t buf[2048];
  long bytes = davClient.streamToBuffer(queryPath, buf, sizeof(buf) - 1);
  if (bytes <= 0) { sendJSON(client, 404, "{\"error\":\"NFO not found\"}"); return; }
  buf[bytes] = 0;
  sendJSON(client, 200, "{\"nfo\":\"" + jsonEscape(String((char *)buf)) + "\"}");
}

// GET /api/dav/cover?path=... — proxy cover image
// PSRAM buffer; JC3248 also checks SD cover cache via DEVICE_HAS_SD_COVER_CACHE.
inline void handleDAVCover(WiFiClient &client, const String &queryPath) {
  if (!cfg_dav_enabled || queryPath.length() == 0) {
    sendJSON(client, 400, "{\"error\":\"Invalid request\"}"); return;
  }
  size_t maxCover = 150 * 1024;
  uint8_t *buf = (uint8_t *)ps_malloc(maxCover);
  if (!buf) { sendJSON(client, 500, "{\"error\":\"Out of PSRAM\"}"); return; }

  long bytes = -1;
#ifdef DEVICE_HAS_SD_COVER_CACHE
  bytes = davReadCachedCover(queryPath, buf, maxCover);
  if (bytes <= 0) {
    bytes = davClient.streamToBuffer(queryPath, buf, maxCover);
    if (bytes > 0) davSaveCachedCover(queryPath, buf, bytes);
  }
#else
  bytes = davClient.streamToBuffer(queryPath, buf, maxCover);
#endif

  if (bytes <= 0) { free(buf); sendJSON(client, 404, "{\"error\":\"Cover not found\"}"); return; }

  String lp = queryPath; lp.toLowerCase();
  String ct = lp.endsWith(".png") ? "image/png" : "image/jpeg";

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: " + ct);
  client.println("Content-Length: " + String(bytes));
  client.println("Cache-Control: max-age=86400");
  client.println("Connection: close");
  client.println();
  size_t sent = 0;
  while (sent < (size_t)bytes) {
    size_t chunk = bytes - sent; if (chunk > 4096) chunk = 4096;
    client.write(&buf[sent], chunk); sent += chunk; yield();
  }
  free(buf);
}
