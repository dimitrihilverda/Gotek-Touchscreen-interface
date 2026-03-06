#ifndef API_HANDLERS_H
#define API_HANDLERS_H

/*
  Gotek Touchscreen — REST API Handlers

  All endpoints return JSON responses.
  File uploads use multipart/form-data streaming.
*/

// Upload tracking
static File uploadFile;
static size_t upload_bytes_received = 0;
static size_t upload_total_bytes = 0;
static String upload_target_path = "";
static String upload_game_name = "";
static bool upload_in_progress = false;

// ============================================================================
// Helper: JSON string escape
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

// Helper: rescan games after changes
void refreshGameList() {
  file_list = listImages();
  buildDisplayNames(file_list);
  sortByDisplay();
  buildGameList();
}

// ============================================================================
// GET /api/system/info
// ============================================================================

void handleSystemInfo(AsyncWebServerRequest *request) {
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

  request->send(200, "application/json", json);
}

// ============================================================================
// GET /api/config
// ============================================================================

void handleConfigGet(AsyncWebServerRequest *request) {
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

  request->send(200, "application/json", json);
}

// ============================================================================
// POST /api/config (form-encoded)
// ============================================================================

void handleConfigPost(AsyncWebServerRequest *request) {
  if (request->hasParam("DISPLAY", true)) cfg_display = request->getParam("DISPLAY", true)->value();
  if (request->hasParam("THEME", true)) {
    cfg_theme = request->getParam("THEME", true)->value();
    theme_path = "/THEMES/" + cfg_theme;
  }
  if (request->hasParam("LASTMODE", true)) cfg_lastmode = request->getParam("LASTMODE", true)->value();
  if (request->hasParam("WIFI_ENABLED", true)) {
    String v = request->getParam("WIFI_ENABLED", true)->value();
    cfg_wifi_enabled = (v == "1" || v == "true");
  }
  if (request->hasParam("WIFI_SSID", true)) cfg_wifi_ssid = request->getParam("WIFI_SSID", true)->value();
  if (request->hasParam("WIFI_PASS", true)) cfg_wifi_pass = request->getParam("WIFI_PASS", true)->value();
  if (request->hasParam("WIFI_CHANNEL", true)) {
    cfg_wifi_channel = (uint8_t)request->getParam("WIFI_CHANNEL", true)->value().toInt();
    if (cfg_wifi_channel < 1 || cfg_wifi_channel > 13) cfg_wifi_channel = 6;
  }

  saveConfig();
  request->send(200, "application/json", "{\"status\":\"ok\"}");
}

// ============================================================================
// GET /api/games/list
// ============================================================================

void handleGamesList(AsyncWebServerRequest *request) {
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
  request->send(200, "application/json", json);
}

// ============================================================================
// GET /api/games/{mode}/{name} — game detail
// ============================================================================

void handleGameDetailParsed(AsyncWebServerRequest *request, const String &mode, const String &name) {
  int found = -1;
  for (int i = 0; i < (int)game_list.size(); i++) {
    if (game_list[i].name == name) { found = i; break; }
  }
  if (found < 0) {
    request->send(404, "application/json", "{\"error\":\"Game not found\"}");
    return;
  }

  GameEntry &g = game_list[found];
  String dir = file_list[g.first_file_index];
  int sl = dir.lastIndexOf('/');
  if (sl > 0) dir = dir.substring(0, sl);

  // NFO
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

  request->send(200, "application/json", json);
}

// ============================================================================
// DELETE /api/games/{mode}/{name}
// ============================================================================

void handleGameDeleteParsed(AsyncWebServerRequest *request, const String &mode, const String &name) {
  String modeDir = (mode == "adf") ? "/ADF" : "/DSK";
  String gamePath = modeDir + "/" + name;

  if (!SD_MMC.exists(gamePath.c_str())) {
    request->send(404, "application/json", "{\"error\":\"Game folder not found\"}");
    return;
  }

  bool ok = deleteDir(SD_MMC, gamePath);
  if (ok) {
    refreshGameList();
    request->send(200, "application/json",
      "{\"status\":\"deleted\",\"games\":" + String(game_list.size()) + "}");
  } else {
    request->send(500, "application/json", "{\"error\":\"Failed to delete\"}");
  }
}

// ============================================================================
// POST /api/games/{mode}/{name}/nfo
// ============================================================================

void handleNFOUpdateParsed(AsyncWebServerRequest *request, const String &mode, const String &name) {
  String modeDir = (mode == "adf") ? "/ADF" : "/DSK";
  String nfoPath = modeDir + "/" + name + "/" + name + ".nfo";

  String content = "";
  if (request->hasParam("content", true)) {
    content = request->getParam("content", true)->value();
  }

  File f = SD_MMC.open(nfoPath.c_str(), "w");
  if (!f) {
    request->send(500, "application/json", "{\"error\":\"Cannot write NFO\"}");
    return;
  }
  f.print(content);
  f.close();

  request->send(200, "application/json", "{\"status\":\"ok\"}");
}

// ============================================================================
// GET /api/games/{mode}/{name}/cover — serve cover image
// ============================================================================

void handleCoverServe(AsyncWebServerRequest *request, const String &mode, const String &name) {
  for (int i = 0; i < (int)game_list.size(); i++) {
    if (game_list[i].name == name && game_list[i].jpg_path.length() > 0) {
      String path = game_list[i].jpg_path;
      String contentType = "image/jpeg";
      if (path.endsWith(".png")) contentType = "image/png";
      request->send(SD_MMC, path.c_str(), contentType.c_str());
      return;
    }
  }
  request->send(404, "application/json", "{\"error\":\"No cover found\"}");
}

// ============================================================================
// POST /api/games/upload — multipart file upload
// ============================================================================

void handleFileUpload(AsyncWebServerRequest *request, const String &filename,
                      size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    upload_in_progress = true;
    upload_bytes_received = 0;

    // Derive game folder name from filename
    upload_game_name = filename;
    int dotPos = upload_game_name.lastIndexOf('.');
    if (dotPos > 0) upload_game_name = upload_game_name.substring(0, dotPos);

    // Strip disk number suffix (e.g. "Game-1" → "Game")
    String folderName = upload_game_name;
    int dashPos = folderName.lastIndexOf('-');
    if (dashPos > 0) {
      String suffix = folderName.substring(dashPos + 1);
      bool allDigits = true;
      for (unsigned int c = 0; c < suffix.length(); c++) {
        if (!isDigit(suffix[c])) { allDigits = false; break; }
      }
      if (allDigits && suffix.length() > 0) folderName = folderName.substring(0, dashPos);
    }

    // Explicit game_name from form
    if (request->hasParam("game_name", true)) {
      folderName = request->getParam("game_name", true)->value();
    }

    String modeDir = (g_mode == MODE_ADF) ? "/ADF" : "/DSK";
    String gameDir = modeDir + "/" + folderName;

    if (!SD_MMC.exists(gameDir.c_str())) SD_MMC.mkdir(gameDir.c_str());

    upload_target_path = gameDir + "/" + filename;
    upload_game_name = folderName;

    Serial.println("Upload start: " + upload_target_path);
    uploadFile = SD_MMC.open(upload_target_path.c_str(), "w");
    if (!uploadFile) {
      Serial.println("ERROR: Cannot create upload file");
      upload_in_progress = false;
      return;
    }
  }

  if (uploadFile && len > 0) {
    uploadFile.write(data, len);
    upload_bytes_received += len;
  }

  if (final) {
    if (uploadFile) uploadFile.close();
    Serial.println("Upload complete: " + upload_target_path + " (" + String(upload_bytes_received) + " bytes)");
    upload_in_progress = false;
  }
}

void handleUploadComplete(AsyncWebServerRequest *request) {
  refreshGameList();
  String json = "{\"status\":\"ok\",\"game\":\"" + jsonEscape(upload_game_name) + "\",";
  json += "\"bytes\":" + String(upload_bytes_received) + ",";
  json += "\"games\":" + String(game_list.size()) + "}";
  request->send(200, "application/json", json);
}

// ============================================================================
// GET /api/upload/progress
// ============================================================================

void handleUploadProgress(AsyncWebServerRequest *request) {
  String json = "{";
  json += "\"in_progress\":" + String(upload_in_progress ? "true" : "false") + ",";
  json += "\"bytes_received\":" + String(upload_bytes_received) + ",";
  json += "\"game\":\"" + jsonEscape(upload_game_name) + "\"";
  json += "}";
  request->send(200, "application/json", json);
}

// ============================================================================
// GET /api/themes/list
// ============================================================================

void handleThemesList(AsyncWebServerRequest *request) {
  String json = "{\"active\":\"" + jsonEscape(cfg_theme) + "\",\"themes\":[";
  for (int i = 0; i < (int)theme_list.size(); i++) {
    if (i > 0) json += ",";
    json += "\"" + jsonEscape(theme_list[i]) + "\"";
  }
  json += "]}";
  request->send(200, "application/json", json);
}

// ============================================================================
// POST /api/themes/{name}/activate
// ============================================================================

void handleThemeActivateParsed(AsyncWebServerRequest *request, const String &name) {
  bool found = false;
  for (const auto &t : theme_list) {
    if (t == name) { found = true; break; }
  }
  if (!found) {
    request->send(404, "application/json", "{\"error\":\"Theme not found\"}");
    return;
  }

  cfg_theme = name;
  theme_path = "/THEMES/" + cfg_theme;
  saveConfig();

  request->send(200, "application/json", "{\"status\":\"ok\",\"theme\":\"" + jsonEscape(name) + "\"}");
}

#endif // API_HANDLERS_H
