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
  if (!dir) return false;
  if (!dir.isDirectory()) {
    dir.close();
    return fs.remove(path.c_str());
  }

  // Collect entries first, then delete (avoids iterator issues)
  std::vector<String> files;
  std::vector<String> dirs;

  File entry;
  while ((entry = dir.openNextFile())) {
    String entryName = entry.name();
    // entry.name() may return full path or just filename depending on ESP32 core
    // Build full path from parent + filename
    String fullPath;
    if (entryName.startsWith("/")) {
      fullPath = entryName;  // already absolute
    } else {
      fullPath = path;
      if (!fullPath.endsWith("/")) fullPath += "/";
      fullPath += entryName;
    }
    if (entry.isDirectory()) {
      dirs.push_back(fullPath);
    } else {
      files.push_back(fullPath);
    }
    entry.close();
  }
  dir.close();

  // Delete files first
  for (const auto &f : files) {
    Serial.println("DEL file: " + f);
    fs.remove(f.c_str());
  }

  // Recurse into subdirectories
  for (const auto &d : dirs) {
    deleteDir(fs, d);
  }

  // Now remove the (empty) directory itself
  Serial.println("DEL dir: " + path);
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

// Find a game's actual folder path on SD from game_list
// Tries exact match first, then case-insensitive, then falls back to /ADF|DSK/{name}
String getGameFolder(const String &name, const String &mode = "") {
  // Pass 1: exact match in game_list
  for (int i = 0; i < (int)game_list.size(); i++) {
    if (game_list[i].name == name && game_list[i].first_file_index >= 0 &&
        game_list[i].first_file_index < (int)file_list.size()) {
      String dir = file_list[game_list[i].first_file_index];
      int sl = dir.lastIndexOf('/');
      if (sl > 0) return dir.substring(0, sl);
    }
  }

  // Pass 2: case-insensitive match in game_list
  String nameLower = name;
  nameLower.toLowerCase();
  for (int i = 0; i < (int)game_list.size(); i++) {
    String gn = game_list[i].name;
    gn.toLowerCase();
    if (gn == nameLower && game_list[i].first_file_index >= 0 &&
        game_list[i].first_file_index < (int)file_list.size()) {
      String dir = file_list[game_list[i].first_file_index];
      int sl = dir.lastIndexOf('/');
      if (sl > 0) return dir.substring(0, sl);
    }
  }

  // Pass 3: try constructed path directly on SD
  const char *md = (mode == "adf") ? "/ADF" : (mode == "dsk") ? "/DSK" :
                   (g_mode == MODE_ADF) ? "/ADF" : "/DSK";
  String fallback = String(md) + "/" + name;
  if (SD_MMC.exists(fallback.c_str())) return fallback;

  // Pass 4: scan SD directory for case-insensitive folder match
  File root = SD_MMC.open(md);
  if (root && root.isDirectory()) {
    File entry;
    while ((entry = root.openNextFile())) {
      if (entry.isDirectory()) {
        String fn = entry.name();
        int sl = fn.lastIndexOf('/');
        if (sl >= 0) fn = fn.substring(sl + 1);
        String fnl = fn;
        fnl.toLowerCase();
        if (fnl == nameLower) {
          String result = String(md) + "/" + fn;
          entry.close();
          root.close();
          return result;
        }
      }
      entry.close();
    }
    root.close();
  }

  Serial.println("getGameFolder: NOT FOUND '" + name + "'");
  return "";
}

// Find game index in game_list by name (exact then case-insensitive)
int findGameByName(const String &name) {
  for (int i = 0; i < (int)game_list.size(); i++) {
    if (game_list[i].name == name) return i;
  }
  String nl = name;
  nl.toLowerCase();
  for (int i = 0; i < (int)game_list.size(); i++) {
    String gn = game_list[i].name;
    gn.toLowerCase();
    if (gn == nl) return i;
  }
  return -1;
}

// ============================================================================
// GET /api/disk/status — what's currently loaded
// ============================================================================

void handleDiskStatus(WiFiClient &client) {
  String json = "{";
  json += "\"loaded\":" + String(loaded_disk_index >= 0 ? "true" : "false") + ",";

  if (loaded_disk_index >= 0 && loaded_disk_index < (int)file_list.size()) {
    String path = file_list[loaded_disk_index];
    json += "\"file\":\"" + jsonEscape(filenameOnly(path)) + "\",";
    json += "\"path\":\"" + jsonEscape(path) + "\",";

    // Find which game this disk belongs to
    int gi = findGameIndex(loaded_disk_index);
    if (gi >= 0 && gi < (int)game_list.size()) {
      json += "\"game\":\"" + jsonEscape(game_list[gi].name) + "\",";
      json += "\"disk_num\":" + String(loaded_disk_index - game_list[gi].first_file_index + 1) + ",";
      json += "\"disk_total\":" + String(game_list[gi].disk_count) + ",";
    } else {
      json += "\"game\":\"" + jsonEscape(basenameNoExt(filenameOnly(path))) + "\",";
      json += "\"disk_num\":1,";
      json += "\"disk_total\":1,";
    }
  } else {
    json += "\"file\":\"\",";
    json += "\"path\":\"\",";
    json += "\"game\":\"\",";
    json += "\"disk_num\":0,";
    json += "\"disk_total\":0,";
  }

  json += "\"mode\":\"" + String(g_mode == MODE_ADF ? "ADF" : "DSK") + "\"";
  json += "}";

  sendJSON(client, 200, json);
}

// ============================================================================
// POST /api/games/{mode}/{name}/load — load a specific disk
// ============================================================================

void handleDiskLoad(WiFiClient &client, const String &mode, const String &name, const String &body) {
  // Optional: "disk" parameter to specify which disk (1-based index, default 1)
  String diskParam = getFormValue(body, "disk");
  int diskNum = (diskParam.length() > 0) ? diskParam.toInt() : 1;
  if (diskNum < 1) diskNum = 1;

  // Find the game in game_list
  int gameIdx = findGameByName(name);

  if (gameIdx < 0) {
    sendJSON(client, 404, "{\"error\":\"Game not found\"}");
    return;
  }

  GameEntry &g = game_list[gameIdx];

  // Resolve which file_list index to load
  // disk_num is 1-based, game disks are consecutive from first_file_index
  int targetIdx = g.first_file_index;

  if (g.disk_count > 1 && diskNum > 1) {
    // Find the Nth disk file for this game
    int count = 0;
    for (int i = 0; i < (int)file_list.size(); i++) {
      String fdir = file_list[i];
      int sl = fdir.lastIndexOf('/');
      if (sl > 0) fdir = fdir.substring(0, sl);
      String gdir = file_list[g.first_file_index];
      int gsl = gdir.lastIndexOf('/');
      if (gsl > 0) gdir = gdir.substring(0, gsl);

      if (fdir == gdir) {
        count++;
        if (count == diskNum) {
          targetIdx = i;
          break;
        }
      }
    }
  }

  if (targetIdx < 0 || targetIdx >= (int)file_list.size()) {
    sendJSON(client, 404, "{\"error\":\"Disk file not found\"}");
    return;
  }

  Serial.println("Web load: " + file_list[targetIdx]);

  // Set selected_index and call doLoadSelected()
  selected_index = targetIdx;
  doLoadSelected();

  // Check if it actually loaded
  if (loaded_disk_index == targetIdx) {
    // Switch touchscreen to detail view for this game
    detail_filename = file_list[targetIdx];
    current_screen = SCR_DETAILS;
    drawDetailsFromNFO(detail_filename);

    String loadedFile = filenameOnly(file_list[targetIdx]);
    sendJSON(client, 200,
      "{\"status\":\"ok\",\"file\":\"" + jsonEscape(loadedFile) +
      "\",\"game\":\"" + jsonEscape(name) +
      "\",\"disk\":" + String(diskNum) + "}");
  } else {
    sendJSON(client, 500, "{\"error\":\"Failed to load disk\"}");
  }
}

// ============================================================================
// POST /api/disk/unload — eject current disk
// ============================================================================

void handleDiskUnload(WiFiClient &client) {
  if (loaded_disk_index < 0) {
    sendJSON(client, 200, "{\"status\":\"ok\",\"message\":\"No disk loaded\"}");
    return;
  }

  doUnload();

  // Switch touchscreen back to game list
  current_screen = SCR_SELECTION;
  drawList();

  sendJSON(client, 200, "{\"status\":\"ok\"}");
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
  json += "\"wifi_clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  json += "\"internet\":" + String(wifi_sta_connected ? "true" : "false") + ",";
  json += "\"internet_ip\":\"" + wifi_sta_ip + "\",";
  json += "\"internet_ssid\":\"" + jsonEscape(cfg_wifi_client_ssid) + "\"";
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
  json += "\"WIFI_CHANNEL\":\"" + String(cfg_wifi_channel) + "\",";
  json += "\"WIFI_CLIENT_ENABLED\":\"" + String(cfg_wifi_client_enabled ? "1" : "0") + "\",";
  json += "\"WIFI_CLIENT_SSID\":\"" + jsonEscape(cfg_wifi_client_ssid) + "\",";
  json += "\"WIFI_CLIENT_PASS\":\"" + jsonEscape(cfg_wifi_client_pass) + "\"";
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

  val = getFormValue(body, "WIFI_CLIENT_ENABLED");
  if (val.length() > 0) {
    cfg_wifi_client_enabled = (val == "1" || val == "true");
  }

  val = getFormValue(body, "WIFI_CLIENT_SSID");
  if (val.length() > 0) cfg_wifi_client_ssid = val;

  // Allow empty password (open networks)
  if (body.indexOf("WIFI_CLIENT_PASS=") >= 0) {
    cfg_wifi_client_pass = getFormValue(body, "WIFI_CLIENT_PASS");
  }

  saveConfig();
  sendJSON(client, 200, "{\"status\":\"ok\"}");
}

// ============================================================================
// GET /api/games/list
// ============================================================================

void handleGamesList(WiFiClient &client) {
  // Find which game is currently loaded
  String loadedGame = "";
  String loadedFile = "";
  if (loaded_disk_index >= 0 && loaded_disk_index < (int)file_list.size()) {
    loadedFile = filenameOnly(file_list[loaded_disk_index]);
    int gi = findGameIndex(loaded_disk_index);
    if (gi >= 0 && gi < (int)game_list.size()) {
      loadedGame = game_list[gi].name;
    }
  }

  String json = "{\"mode\":\"" + String(g_mode == MODE_ADF ? "ADF" : "DSK") + "\",";
  json += "\"loaded_game\":\"" + jsonEscape(loadedGame) + "\",";
  json += "\"loaded_file\":\"" + jsonEscape(loadedFile) + "\",";
  json += "\"games\":[";

  for (int i = 0; i < (int)game_list.size(); i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"name\":\"" + jsonEscape(game_list[i].name) + "\",";
    json += "\"disks\":" + String(game_list[i].disk_count) + ",";
    json += "\"has_cover\":" + String(game_list[i].jpg_path.length() > 0 ? "true" : "false") + ",";
    json += "\"loaded\":" + String(game_list[i].name == loadedGame ? "true" : "false") + ",";

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
  int found = findGameByName(name);
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
  String gamePath = getGameFolder(name, mode);

  if (gamePath.length() == 0 || !SD_MMC.exists(gamePath.c_str())) {
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
  String gameDir = getGameFolder(name, mode);
  if (gameDir.length() == 0) {
    sendJSON(client, 404, "{\"error\":\"Game not found\"}");
    return;
  }
  String nfoPath = gameDir + "/" + name + ".nfo";

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
  int idx = findGameByName(name);
  if (idx >= 0 && game_list[idx].jpg_path.length() > 0) {
    String path = game_list[idx].jpg_path;
    String contentType = "image/jpeg";
    if (path.endsWith(".png")) contentType = "image/png";
    sendFileResponse(client, path, contentType);
    return;
  }

  // Fallback: scan game folder for any jpg/png
  String gameDir = getGameFolder(name, mode);
  if (gameDir.length() > 0) {
    File dir = SD_MMC.open(gameDir.c_str());
    if (dir && dir.isDirectory()) {
      File entry;
      while ((entry = dir.openNextFile())) {
        String fn = String(entry.name());
        fn.toLowerCase();
        if (!entry.isDirectory() && (fn.endsWith(".jpg") || fn.endsWith(".jpeg") || fn.endsWith(".png"))) {
          String fullPath = entry.name();
          // Ensure full path
          if (!fullPath.startsWith("/")) fullPath = gameDir + "/" + fullPath;
          String ct = fn.endsWith(".png") ? "image/png" : "image/jpeg";
          entry.close();
          dir.close();
          sendFileResponse(client, fullPath, ct);
          return;
        }
        entry.close();
      }
      dir.close();
    }
  }
  sendJSON(client, 404, "{\"error\":\"No cover found\"}");
}

// ============================================================================
// POST /api/games/{mode}/{name}/cover — upload cover image (multipart)
// ============================================================================

bool handleCoverUpload(WiFiClient &client, const HttpRequest &req, const String &mode, const String &name) {
  if (req.boundary.length() == 0 || req.contentLength <= 0) {
    sendJSON(client, 400, "{\"error\":\"Expected multipart upload\"}");
    return true;
  }

  String gameDir = getGameFolder(name, mode);
  if (gameDir.length() == 0) {
    // Drain remaining data to avoid corrupting connection
    unsigned long t = millis();
    while (client.available() && millis() - t < 3000) { client.read(); }
    sendJSON(client, 404, "{\"error\":\"Game folder not found\"}");
    return true;
  }

  String delim = "--" + req.boundary;
  String delimEnd = delim + "--";
  String savePath = "";
  File outFile;
  bool inFileData = false;
  bool done = false;
  size_t totalWritten = 0;

  while (client.connected() && !done) {
    if (!inFileData) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.startsWith(delimEnd)) { done = true; break; }
      if (line.startsWith(delim)) {
        String disp = client.readStringUntil('\n');
        disp.trim();
        int fnIdx = disp.indexOf("filename=\"");
        if (fnIdx >= 0) {
          fnIdx += 10;
          int fnEnd = disp.indexOf("\"", fnIdx);
          String origName = disp.substring(fnIdx, fnEnd);

          // Determine extension from original filename
          String ext = ".jpg";
          String origLower = origName;
          origLower.toLowerCase();
          if (origLower.endsWith(".png")) ext = ".png";
          else if (origLower.endsWith(".jpeg")) ext = ".jpg";

          // Use folder name as cover filename
          String folderName = gameDir;
          int sl = folderName.lastIndexOf('/');
          if (sl >= 0) folderName = folderName.substring(sl + 1);
          savePath = gameDir + "/" + folderName + ext;

          // Remove old cover if different extension
          String altExt = (ext == ".jpg") ? ".png" : ".jpg";
          String altPath = gameDir + "/" + folderName + altExt;
          if (SD_MMC.exists(altPath.c_str())) SD_MMC.remove(altPath.c_str());

          outFile = SD_MMC.open(savePath.c_str(), "w");
          // Skip remaining headers
          while (client.connected()) {
            String hdr = client.readStringUntil('\n');
            hdr.trim();
            if (hdr.length() == 0) break;
          }
          inFileData = true;
          totalWritten = 0;
        }
      }
    } else {
      uint8_t buf[1024];
      while (client.connected() && client.available()) {
        int avail = min((int)sizeof(buf), client.available());
        int n = client.readBytes(buf, avail);
        if (n <= 0) break;
        // Check for boundary in chunk
        String chunk((char *)buf, n);
        int bndIdx = chunk.indexOf(delim);
        if (bndIdx >= 0) {
          int writeLen = bndIdx;
          if (writeLen >= 2) writeLen -= 2;
          if (writeLen > 0 && outFile) { outFile.write(buf, writeLen); totalWritten += writeLen; }
          if (outFile) outFile.close();
          inFileData = false;
          if (chunk.indexOf(delimEnd) >= 0) done = true;
          break;
        } else {
          if (outFile) { outFile.write(buf, n); totalWritten += n; }
        }
        if (!client.connected()) break;
      }
      if (!client.available() && !client.connected()) break;
      if (client.available() == 0) delay(1);
    }
  }

  if (outFile) outFile.close();

  // Refresh to pick up new cover
  refreshGameList();

  sendJSON(client, 200,
    "{\"status\":\"ok\",\"path\":\"" + jsonEscape(savePath) +
    "\",\"bytes\":" + String(totalWritten) + "}");
  return true;
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
// POST /api/games/{mode}/{name}/cover-url — download cover from internet
// ============================================================================

void handleCoverDownload(WiFiClient &client, const String &mode, const String &name, const String &body) {
  if (!wifi_sta_connected) {
    sendJSON(client, 503, "{\"error\":\"No internet connection. Configure WiFi Client first.\"}");
    return;
  }

  String url = getFormValue(body, "url");
  if (url.length() == 0) {
    sendJSON(client, 400, "{\"error\":\"Missing url parameter\"}");
    return;
  }

  // Find actual game folder from game_list
  String gameDir = getGameFolder(name, mode);
  if (gameDir.length() == 0) {
    sendJSON(client, 404, "{\"error\":\"Game folder not found\"}");
    return;
  }

  // Determine extension from URL
  String ext = ".jpg";
  String urlLower = url;
  urlLower.toLowerCase();
  if (urlLower.indexOf(".png") >= 0) ext = ".png";

  // Use the actual folder name for the cover filename (may differ from URL name)
  String folderName = gameDir;
  int lastSl = folderName.lastIndexOf('/');
  if (lastSl >= 0) folderName = folderName.substring(lastSl + 1);
  String savePath = gameDir + "/" + folderName + ext;

  // Parse host and path from URL
  String host = "";
  String path = "/";
  int port = 80;
  bool useSSL = false;

  String work = url;
  if (work.startsWith("https://")) {
    work = work.substring(8);
    port = 443;
    useSSL = true;
  } else if (work.startsWith("http://")) {
    work = work.substring(7);
  }

  int slashIdx = work.indexOf('/');
  if (slashIdx > 0) {
    host = work.substring(0, slashIdx);
    path = work.substring(slashIdx);
  } else {
    host = work;
  }

  // Check for port in host
  int colonIdx = host.indexOf(':');
  if (colonIdx > 0) {
    port = host.substring(colonIdx + 1).toInt();
    host = host.substring(0, colonIdx);
  }

  Serial.println("Downloading cover: " + host + path);

  // Use WiFiClientSecure for HTTPS, WiFiClient for HTTP
  WiFiClient *httpClient;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;

  if (useSSL) {
    secureClient.setInsecure();  // skip cert validation (ESP32 has limited CA store)
    httpClient = &secureClient;
  } else {
    httpClient = &plainClient;
  }

  if (!httpClient->connect(host.c_str(), port)) {
    sendJSON(client, 502, "{\"error\":\"Cannot connect to " + jsonEscape(host) + "\"}");
    return;
  }

  // Send HTTP GET
  httpClient->println("GET " + path + " HTTP/1.1");
  httpClient->println("Host: " + host);
  httpClient->println("Connection: close");
  httpClient->println("User-Agent: Gotek-Touchscreen/" + String(FW_VERSION));
  httpClient->println();

  // Read response status
  String statusLine = httpClient->readStringUntil('\n');
  int statusCode = 0;
  int sp1 = statusLine.indexOf(' ');
  if (sp1 > 0) statusCode = statusLine.substring(sp1 + 1).toInt();

  if (statusCode < 200 || statusCode >= 400) {
    httpClient->stop();
    sendJSON(client, 502, "{\"error\":\"HTTP " + String(statusCode) + " from server\"}");
    return;
  }

  // Handle redirects (301, 302, 303, 307, 308)
  if (statusCode >= 300 && statusCode < 400) {
    String location = "";
    while (httpClient->connected()) {
      String hdr = httpClient->readStringUntil('\n');
      hdr.trim();
      if (hdr.length() == 0) break;
      String hdrLow = hdr;
      hdrLow.toLowerCase();
      if (hdrLow.startsWith("location:")) {
        location = hdr.substring(9);
        location.trim();
      }
    }
    httpClient->stop();
    // One redirect — not recursive to avoid loops
    sendJSON(client, 502, "{\"error\":\"Redirect to " + jsonEscape(location) + " — try that URL directly\"}");
    return;
  }

  // Skip response headers, get content length
  int contentLen = -1;
  while (httpClient->connected()) {
    String hdr = httpClient->readStringUntil('\n');
    hdr.trim();
    if (hdr.length() == 0) break;
    String hdrLow = hdr;
    hdrLow.toLowerCase();
    if (hdrLow.startsWith("content-length:")) {
      contentLen = hdr.substring(15).toInt();
    }
  }

  // Stream to SD card
  File outFile = SD_MMC.open(savePath.c_str(), "w");
  if (!outFile) {
    httpClient->stop();
    sendJSON(client, 500, "{\"error\":\"Cannot write to SD card\"}");
    return;
  }

  uint8_t buf[1024];
  size_t totalBytes = 0;
  unsigned long timeout = millis();

  while (httpClient->connected() || httpClient->available()) {
    if (httpClient->available()) {
      int n = httpClient->read(buf, sizeof(buf));
      if (n > 0) {
        outFile.write(buf, n);
        totalBytes += n;
        timeout = millis();
      }
    } else {
      if (millis() - timeout > 10000) break;  // 10s timeout
      yield();
      delay(1);
    }
    yield();  // feed watchdog during long downloads
  }

  outFile.close();
  httpClient->stop();

  Serial.println("Cover saved: " + savePath + " (" + String(totalBytes) + " bytes)");

  // Refresh game list to pick up new cover
  refreshGameList();

  sendJSON(client, 200,
    "{\"status\":\"ok\",\"path\":\"" + jsonEscape(savePath) +
    "\",\"bytes\":" + String(totalBytes) + "}");
}

// ============================================================================
// GET /api/wifi/status — WiFi connection status
// ============================================================================

void handleWiFiStatus(WiFiClient &client) {
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

// ============================================================================
// GET /api/wifi/scan — scan for available networks
// ============================================================================

void handleWiFiScan(WiFiClient &client) {
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
