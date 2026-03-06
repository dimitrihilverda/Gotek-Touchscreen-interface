#ifndef API_HANDLERS_H
#define API_HANDLERS_H

/*
  Gotek Touchscreen — REST API Handlers
  Uses raw WiFiClient responses (no external dependencies).
  All endpoints return JSON.
*/

// ============================================================================
// Helpers
// ============================================================================

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

String readFileString(const String &path) {
  File f = SD_MMC.open(path.c_str(), "r");
  if (!f) return "";
  String content = f.readString();
  f.close();
  return content;
}

bool deleteDir(fs::FS &fs, const String &path) {
  File dir = fs.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return fs.remove(path.c_str());
  }
  File entry;
  while ((entry = dir.openNextFile())) {
    String entryPath = entry.name();
    if (!entryPath.startsWith("/")) entryPath = "/" + entryPath;
    bool isDir = entry.isDirectory();
    entry.close();
    if (isDir) deleteDir(fs, entryPath);
    else fs.remove(entryPath.c_str());
  }
  dir.close();
  return fs.rmdir(path.c_str());
}

size_t getFileSize(const String &path) {
  File f = SD_MMC.open(path.c_str(), "r");
  if (!f) return 0;
  size_t sz = f.size();
  f.close();
  return sz;
}

void refreshGameList() {
  file_list = listImages();
  buildDisplayNames(file_list);
  sortByDisplay();
  buildGameList();
}

// ============================================================================
// GET /api/system/info
// ============================================================================

void handleSystemInfo(WiFiClient &client) {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t freePsram = ESP.getFreePsram();
  uint64_t sdTotal = SD_MMC.totalBytes();
  uint64_t sdUsed  = SD_MMC.usedBytes();

  String loaded = "none";
  if (cfg_lastfile.length() > 0) {
    loaded = basenameNoExt(filenameOnly(cfg_lastfile));
  }

  String json = "{";
  json += "\"firmware\":\"" + String(FW_VERSION) + "\",";
  json += "\"heap_free\":" + String(freeHeap) + ",";
  json += "\"psram_free\":" + String(freePsram) + ",";
  json += "\"sd_total_mb\":" + String((uint32_t)(sdTotal / (1024 * 1024))) + ",";
  json += "\"sd_used_mb\":" + String((uint32_t)(sdUsed / (1024 * 1024))) + ",";
  json += "\"game_count\":" + String(game_list.size()) + ",";
  json += "\"file_count\":" + String(file_list.size()) + ",";
  json += "\"loaded_game\":\"" + jsonEscape(loaded) + "\",";
  json += "\"theme\":\"" + jsonEscape(cfg_theme) + "\",";
  json += "\"mode\":\"" + String(g_mode == MODE_ADF ? "ADF" : "DSK") + "\",";
  json += "\"wifi_ssid\":\"" + jsonEscape(cfg_wifi_ssid) + "\",";
  json += "\"wifi_ip\":\"" + wifi_ap_ip + "\",";
  json += "\"wifi_clients\":" + String(WiFi.softAPgetStationNum());
  json += "}";

  sendJSON(client, 200, json);
}

// ============================================================================
// GET /api/config
// ============================================================================

void handleConfigGet(WiFiClient &client) {
  String json = "{";
  json += "\"DISPLAY\":\"" + jsonEscape(cfg_display) + "\",";
  json += "\"LASTFILE\":\"" + jsonEscape(cfg_lastfile) + "\",";
  json += "\"LASTMODE\":\"" + jsonEscape(cfg_lastmode) + "\",";
  json += "\"THEME\":\"" + jsonEscape(cfg_theme) + "\",";
  json += "\"WIFI_ENABLED\":\"" + String(cfg_wifi_enabled ? "1" : "0") + "\",";
  json += "\"WIFI_SSID\":\"" + jsonEscape(cfg_wifi_ssid) + "\",";
  json += "\"WIFI_PASS\":\"" + jsonEscape(cfg_wifi_pass) + "\",";
  json += "\"WIFI_CHANNEL\":\"" + String(cfg_wifi_channel) + "\"";
  json += "}";

  sendJSON(client, 200, json);
}

// ============================================================================
// POST /api/config
// ============================================================================

void handleConfigPost(WiFiClient &client, const String &body) {
  String val;

  val = getFormValue(body, "DISPLAY");
  if (val.length() > 0) cfg_display = val;

  val = getFormValue(body, "THEME");
  if (val.length() > 0) {
    cfg_theme = val;
    theme_path = "/THEMES/" + cfg_theme;
  }

  val = getFormValue(body, "LASTMODE");
  if (val.length() > 0) cfg_lastmode = val;

  val = getFormValue(body, "WIFI_ENABLED");
  if (val.length() > 0) {
    cfg_wifi_enabled = (val == "1" || val == "true");
  }

  val = getFormValue(body, "WIFI_SSID");
  if (val.length() > 0) cfg_wifi_ssid = val;

  val = getFormValue(body, "WIFI_PASS");
  if (val.length() > 0) cfg_wifi_pass = val;

  val = getFormValue(body, "WIFI_CHANNEL");
  if (val.length() > 0) {
    cfg_wifi_channel = (uint8_t)val.toInt();
    if (cfg_wifi_channel < 1 || cfg_wifi_channel > 13) cfg_wifi_channel = 6;
  }

  saveConfig();
  sendJSON(client, 200, "{\"status\":\"ok\"}");
}

// ============================================================================
// GET /api/games/list
// ============================================================================

void handleGamesList(WiFiClient &client) {
  String json = "{\"mode\":\"" + String(g_mode == MODE_ADF ? "ADF" : "DSK") + "\",\"games\":[";

  for (int i = 0; i < (int)game_list.size(); i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"name\":\"" + jsonEscape(game_list[i].name) + "\",";
    json += "\"disks\":" + String(game_list[i].disk_count) + ",";
    json += "\"has_cover\":" + String(game_list[i].jpg_path.length() > 0 ? "true" : "false") + ",";

    // Check for NFO
    String nfoPath = "";
    if (game_list[i].first_file_index >= 0 && game_list[i].first_file_index < (int)file_list.size()) {
      String dir = file_list[game_list[i].first_file_index];
      int sl = dir.lastIndexOf('/');
      if (sl > 0) dir = dir.substring(0, sl);
      String tryNfo = dir + "/" + game_list[i].name + ".nfo";
      if (SD_MMC.exists(tryNfo.c_str())) nfoPath = tryNfo;
    }
    json += "\"has_nfo\":" + String(nfoPath.length() > 0 ? "true" : "false") + ",";
    json += "\"index\":" + String(i);
    json += "}";
  }

  json += "]}";
  sendJSON(client, 200, json);
}

// ============================================================================
// GET /api/games/{mode}/{name} — game detail
// ============================================================================

void handleGameDetailParsed(WiFiClient &client, const String &mode, const String &name) {
  int found = -1;
  for (int i = 0; i < (int)game_list.size(); i++) {
    if (game_list[i].name == name) { found = i; break; }
  }
  if (found < 0) {
    sendJSON(client, 404, "{\"error\":\"Game not found\"}");
    return;
  }

  GameEntry &g = game_list[found];
  String dir = file_list[g.first_file_index];
  int sl = dir.lastIndexOf('/');
  if (sl > 0) dir = dir.substring(0, sl);

  String nfoContent = "";
  String nfoPath = dir + "/" + g.name + ".nfo";
  if (SD_MMC.exists(nfoPath.c_str())) nfoContent = readFileString(nfoPath);

  // Disk files
  String disksJson = "[";
  for (int i = 0; i < (int)file_list.size(); i++) {
    String fileDir = file_list[i];
    int fsl = fileDir.lastIndexOf('/');
    if (fsl > 0) fileDir = fileDir.substring(0, fsl);
    if (fileDir == dir) {
      String fname = filenameOnly(file_list[i]);
      String upper = fname;
      upper.toUpperCase();
      String ext = (mode == "adf") ? ".ADF" : ".DSK";
      if (upper.endsWith(ext) || upper.endsWith(".IMG")) {
        if (disksJson.length() > 1) disksJson += ",";
        size_t fsize = getFileSize(file_list[i]);
        disksJson += "{\"file\":\"" + jsonEscape(fname) + "\",\"size\":" + String(fsize) + "}";
      }
    }
  }
  disksJson += "]";

  String json = "{";
  json += "\"name\":\"" + jsonEscape(g.name) + "\",";
  json += "\"folder\":\"" + jsonEscape(dir) + "\",";
  json += "\"disk_count\":" + String(g.disk_count) + ",";
  json += "\"disks\":" + disksJson + ",";
  json += "\"has_cover\":" + String(g.jpg_path.length() > 0 ? "true" : "false") + ",";
  json += "\"nfo\":\"" + jsonEscape(nfoContent) + "\"";
  json += "}";

  sendJSON(client, 200, json);
}

// ============================================================================
// DELETE /api/games/{mode}/{name}
// ============================================================================

void handleGameDeleteParsed(WiFiClient &client, const String &mode, const String &name) {
  String modeDir = (mode == "adf") ? "/ADF" : "/DSK";
  String gamePath = modeDir + "/" + name;

  if (!SD_MMC.exists(gamePath.c_str())) {
    sendJSON(client, 404, "{\"error\":\"Game folder not found\"}");
    return;
  }

  bool ok = deleteDir(SD_MMC, gamePath);
  if (ok) {
    refreshGameList();
    sendJSON(client, 200, "{\"status\":\"deleted\",\"games\":" + String(game_list.size()) + "}");
  } else {
    sendJSON(client, 500, "{\"error\":\"Failed to delete\"}");
  }
}

// ============================================================================
// POST /api/games/{mode}/{name}/nfo
// ============================================================================

void handleNFOUpdateParsed(WiFiClient &client, const String &mode, const String &name, const String &body) {
  String modeDir = (mode == "adf") ? "/ADF" : "/DSK";
  String nfoPath = modeDir + "/" + name + "/" + name + ".nfo";

  String content = getFormValue(body, "content");

  File f = SD_MMC.open(nfoPath.c_str(), "w");
  if (!f) {
    sendJSON(client, 500, "{\"error\":\"Cannot write NFO\"}");
    return;
  }
  f.print(content);
  f.close();

  sendJSON(client, 200, "{\"status\":\"ok\"}");
}

// ============================================================================
// GET /api/games/{mode}/{name}/cover — serve cover image
// ============================================================================

void handleCoverServe(WiFiClient &client, const String &mode, const String &name) {
  for (int i = 0; i < (int)game_list.size(); i++) {
    if (game_list[i].name == name && game_list[i].jpg_path.length() > 0) {
      String path = game_list[i].jpg_path;
      String contentType = "image/jpeg";
      if (path.endsWith(".png")) contentType = "image/png";

      sendFileResponse(client, path, contentType);
      return;
    }
  }
  sendJSON(client, 404, "{\"error\":\"No cover found\"}");
}

// ============================================================================
// GET /api/upload/progress
// ============================================================================

void handleUploadProgress(WiFiClient &client) {
  // Upload progress is tracked in webserver.h multipart handler
  // For now return a simple status
  sendJSON(client, 200, "{\"in_progress\":false,\"bytes_received\":0}");
}

// ============================================================================
// GET /api/themes/list
// ============================================================================

void handleThemesList(WiFiClient &client) {
  String json = "{\"active\":\"" + jsonEscape(cfg_theme) + "\",\"themes\":[";
  for (int i = 0; i < (int)theme_list.size(); i++) {
    if (i > 0) json += ",";
    json += "\"" + jsonEscape(theme_list[i]) + "\"";
  }
  json += "]}";
  sendJSON(client, 200, json);
}

// ============================================================================
// POST /api/themes/{name}/activate
// ============================================================================

void handleThemeActivateParsed(WiFiClient &client, const String &name) {
  bool found = false;
  for (const auto &t : theme_list) {
    if (t == name) { found = true; break; }
  }
  if (!found) {
    sendJSON(client, 404, "{\"error\":\"Theme not found\"}");
    return;
  }

  cfg_theme = name;
  theme_path = "/THEMES/" + cfg_theme;
  saveConfig();

  sendJSON(client, 200, "{\"status\":\"ok\",\"theme\":\"" + jsonEscape(name) + "\"}");
}

#endif // API_HANDLERS_H
