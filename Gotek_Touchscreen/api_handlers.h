#ifndef API_HANDLERS_H
#define API_HANDLERS_H

/*
  Gotek Touchscreen — REST API Handlers
  Uses built-in ESP32 WebServer (no external dependencies).
  All endpoints return JSON. File uploads use multipart/form-data.
*/

// Upload tracking
static File uploadFile;
static size_t upload_bytes_received = 0;
static String upload_target_path = "";
static String upload_game_name = "";
static bool upload_in_progress = false;

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

void handleSystemInfo() {
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

  sendJSON(200, json);
}

// ============================================================================
// GET /api/config
// ============================================================================

void handleConfigGet() {
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

  sendJSON(200, json);
}

// ============================================================================
// POST /api/config
// ============================================================================

void handleConfigPost() {
  if (httpServer.hasArg("DISPLAY")) cfg_display = httpServer.arg("DISPLAY");
  if (httpServer.hasArg("THEME")) {
    cfg_theme = httpServer.arg("THEME");
    theme_path = "/THEMES/" + cfg_theme;
  }
  if (httpServer.hasArg("LASTMODE")) cfg_lastmode = httpServer.arg("LASTMODE");
  if (httpServer.hasArg("WIFI_ENABLED")) {
    String v = httpServer.arg("WIFI_ENABLED");
    cfg_wifi_enabled = (v == "1" || v == "true");
  }
  if (httpServer.hasArg("WIFI_SSID")) cfg_wifi_ssid = httpServer.arg("WIFI_SSID");
  if (httpServer.hasArg("WIFI_PASS")) cfg_wifi_pass = httpServer.arg("WIFI_PASS");
  if (httpServer.hasArg("WIFI_CHANNEL")) {
    cfg_wifi_channel = (uint8_t)httpServer.arg("WIFI_CHANNEL").toInt();
    if (cfg_wifi_channel < 1 || cfg_wifi_channel > 13) cfg_wifi_channel = 6;
  }

  saveConfig();
  sendJSON(200, "{\"status\":\"ok\"}");
}

// ============================================================================
// GET /api/games/list
// ============================================================================

void handleGamesList() {
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
  sendJSON(200, json);
}

// ============================================================================
// GET /api/games/{mode}/{name} — game detail (called from onNotFound router)
// ============================================================================

void handleGameDetailParsed(const String &mode, const String &name) {
  int found = -1;
  for (int i = 0; i < (int)game_list.size(); i++) {
    if (game_list[i].name == name) { found = i; break; }
  }
  if (found < 0) {
    sendJSON(404, "{\"error\":\"Game not found\"}");
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

  sendJSON(200, json);
}

// ============================================================================
// DELETE /api/games/{mode}/{name}
// ============================================================================

void handleGameDeleteParsed(const String &mode, const String &name) {
  String modeDir = (mode == "adf") ? "/ADF" : "/DSK";
  String gamePath = modeDir + "/" + name;

  if (!SD_MMC.exists(gamePath.c_str())) {
    sendJSON(404, "{\"error\":\"Game folder not found\"}");
    return;
  }

  bool ok = deleteDir(SD_MMC, gamePath);
  if (ok) {
    refreshGameList();
    sendJSON(200, "{\"status\":\"deleted\",\"games\":" + String(game_list.size()) + "}");
  } else {
    sendJSON(500, "{\"error\":\"Failed to delete\"}");
  }
}

// ============================================================================
// POST /api/games/{mode}/{name}/nfo
// ============================================================================

void handleNFOUpdateParsed(const String &mode, const String &name) {
  String modeDir = (mode == "adf") ? "/ADF" : "/DSK";
  String nfoPath = modeDir + "/" + name + "/" + name + ".nfo";

  String content = "";
  if (httpServer.hasArg("content")) {
    content = httpServer.arg("content");
  }

  File f = SD_MMC.open(nfoPath.c_str(), "w");
  if (!f) {
    sendJSON(500, "{\"error\":\"Cannot write NFO\"}");
    return;
  }
  f.print(content);
  f.close();

  sendJSON(200, "{\"status\":\"ok\"}");
}

// ============================================================================
// GET /api/games/{mode}/{name}/cover — serve cover image
// ============================================================================

void handleCoverServe(const String &mode, const String &name) {
  for (int i = 0; i < (int)game_list.size(); i++) {
    if (game_list[i].name == name && game_list[i].jpg_path.length() > 0) {
      String path = game_list[i].jpg_path;
      File coverFile = SD_MMC.open(path.c_str(), "r");
      if (!coverFile) {
        sendJSON(404, "{\"error\":\"Cover file not readable\"}");
        return;
      }

      String contentType = "image/jpeg";
      if (path.endsWith(".png")) contentType = "image/png";

      size_t fileSize = coverFile.size();
      sendCORS();
      httpServer.sendHeader("Cache-Control", "max-age=3600");
      httpServer.streamFile(coverFile, contentType);
      coverFile.close();
      return;
    }
  }
  sendJSON(404, "{\"error\":\"No cover found\"}");
}

// ============================================================================
// POST /api/games/upload — multipart file upload
// ============================================================================

void handleFileUpload() {
  HTTPUpload &upload = httpServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    upload_in_progress = true;
    upload_bytes_received = 0;

    // Derive game folder name from filename
    upload_game_name = upload.filename;
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

    // Check form param for explicit game name
    if (httpServer.hasArg("game_name")) {
      folderName = httpServer.arg("game_name");
    }

    String modeDir = (g_mode == MODE_ADF) ? "/ADF" : "/DSK";
    String gameDir = modeDir + "/" + folderName;

    if (!SD_MMC.exists(gameDir.c_str())) SD_MMC.mkdir(gameDir.c_str());

    upload_target_path = gameDir + "/" + upload.filename;
    upload_game_name = folderName;

    Serial.println("Upload start: " + upload_target_path);
    uploadFile = SD_MMC.open(upload_target_path.c_str(), "w");
    if (!uploadFile) {
      Serial.println("ERROR: Cannot create upload file");
      upload_in_progress = false;
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
      upload_bytes_received += upload.currentSize;
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    Serial.println("Upload complete: " + upload_target_path + " (" + String(upload_bytes_received) + " bytes)");
    upload_in_progress = false;
  }
}

void handleUploadComplete() {
  refreshGameList();
  String json = "{\"status\":\"ok\",\"game\":\"" + jsonEscape(upload_game_name) + "\",";
  json += "\"bytes\":" + String(upload_bytes_received) + ",";
  json += "\"games\":" + String(game_list.size()) + "}";
  sendJSON(200, json);
}

// ============================================================================
// GET /api/upload/progress
// ============================================================================

void handleUploadProgress() {
  String json = "{";
  json += "\"in_progress\":" + String(upload_in_progress ? "true" : "false") + ",";
  json += "\"bytes_received\":" + String(upload_bytes_received) + ",";
  json += "\"game\":\"" + jsonEscape(upload_game_name) + "\"";
  json += "}";
  sendJSON(200, json);
}

// ============================================================================
// GET /api/themes/list
// ============================================================================

void handleThemesList() {
  String json = "{\"active\":\"" + jsonEscape(cfg_theme) + "\",\"themes\":[";
  for (int i = 0; i < (int)theme_list.size(); i++) {
    if (i > 0) json += ",";
    json += "\"" + jsonEscape(theme_list[i]) + "\"";
  }
  json += "]}";
  sendJSON(200, json);
}

// ============================================================================
// POST /api/themes/{name}/activate
// ============================================================================

void handleThemeActivateParsed(const String &name) {
  bool found = false;
  for (const auto &t : theme_list) {
    if (t == name) { found = true; break; }
  }
  if (!found) {
    sendJSON(404, "{\"error\":\"Theme not found\"}");
    return;
  }

  cfg_theme = name;
  theme_path = "/THEMES/" + cfg_theme;
  saveConfig();

  sendJSON(200, "{\"status\":\"ok\",\"theme\":\"" + jsonEscape(name) + "\"}");
}

#endif // API_HANDLERS_H
