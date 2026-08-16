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

// Get game folder — reuses findGameByName for lookup
// For root-level files (no subfolder), auto-creates /ADF|DSK/{name}/ and moves the file
String getGameFolder(const String &name, const String &mode = "") {
  int idx = findGameByName(name);
  if (idx >= 0) {
    int fi = game_list[idx].first_file_index;
    if (fi >= 0 && fi < (int)file_list.size()) {
      String fp = file_list[fi];
      int sl = fp.lastIndexOf('/');
      if (sl > 0) return fp.substring(0, sl);  // normal subfolder

      // File is in root (e.g. /AlienBreed.adf) — create subfolder and move it
      const char *md = (mode == "adf") ? "/ADF" : (mode == "dsk") ? "/DSK" :
                       (g_mode == MODE_ADF) ? "/ADF" : "/DSK";
      String newDir = String(md) + "/" + name;
      String fname = fp.substring(sl + 1);  // filename only
      String baseName = name;  // without extension

      SD_MMC.mkdir(newDir.c_str());

      // Move the disk image
      String newPath = newDir + "/" + fname;
      if (SD_MMC.rename(fp.c_str(), newPath.c_str())) {
        Serial.println("Moved " + fp + " -> " + newPath);
        file_list[fi] = newPath;
      }

      // Also move associated .nfo, .jpg, .jpeg, .png from same directory
      const char *exts[] = { ".nfo", ".jpg", ".jpeg", ".png", ".NFO", ".JPG", ".JPEG", ".PNG" };
      String srcDir = (sl == 0) ? "/" : fp.substring(0, sl);
      for (int x = 0; x < 8; x++) {
        String srcFile = srcDir + "/" + baseName + exts[x];
        if (SD_MMC.exists(srcFile.c_str())) {
          String dstFile = newDir + "/" + baseName + exts[x];
          if (SD_MMC.rename(srcFile.c_str(), dstFile.c_str())) {
            Serial.println("Moved " + srcFile + " -> " + dstFile);
          }
        }
        // Also try with the original filename base (might differ from game name)
        String fnBase = fname;
        int dp = fnBase.lastIndexOf('.');
        if (dp > 0) fnBase = fnBase.substring(0, dp);
        if (fnBase != baseName) {
          String srcFile2 = srcDir + "/" + fnBase + exts[x];
          if (SD_MMC.exists(srcFile2.c_str())) {
            String dstFile2 = newDir + "/" + fnBase + exts[x];
            if (SD_MMC.rename(srcFile2.c_str(), dstFile2.c_str())) {
              Serial.println("Moved " + srcFile2 + " -> " + dstFile2);
            }
          }
        }
      }

      // Refresh game list to reflect new paths
      refreshGameList();
      return newDir;
    }
  }

  // Fallback: construct path and check SD
  const char *md = (mode == "adf") ? "/ADF" : (mode == "dsk") ? "/DSK" :
                   (g_mode == MODE_ADF) ? "/ADF" : "/DSK";
  String fb = String(md) + "/" + name;
  if (SD_MMC.exists(fb.c_str())) return fb;

  // Last resort: scan SD folders
  String nl = name;
  nl.toLowerCase();
  File root = SD_MMC.open(md);
  if (root && root.isDirectory()) {
    File e;
    while ((e = root.openNextFile())) {
      if (e.isDirectory()) {
        String fn = e.name();
        int sl = fn.lastIndexOf('/');
        if (sl >= 0) fn = fn.substring(sl + 1);
        String fl = fn;
        fl.toLowerCase();
        if (fl == nl) { String r = String(md) + "/" + fn; e.close(); root.close(); return r; }
      }
      e.close();
      yield();
    }
    root.close();
  }
  return "";
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

  // nowPlaying is the authority on what is actually mounted, and unlike
  // loaded_disk_index it covers WebDAV too (which uses a -2 sentinel there).
  // The web UI polls these three fields to keep its NOW PLAYING in step with
  // the touchscreen instead of only noticing on a reload.
  const char *srcName = (nowPlaying.source == NP_SD)  ? "SD"
                      : (nowPlaying.source == NP_DAV) ? "DAV" : "";
  json += "\"source\":\"" + String(srcName) + "\",";
  json += "\"name\":\"" + jsonEscape(nowPlaying.name) + "\",";
  json += "\"np_path\":\"" + jsonEscape(nowPlaying.path) + "\",";
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
  int targetIdx = g.first_file_index;

  if (g.disk_count > 1 && diskNum > 1) {
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

  Serial.println("Web load (deferred): " + file_list[targetIdx]);

  // Defer the actual load to the main loop — send response immediately
  // so the web UI doesn't hang and the touch stays responsive
  web_pending_sd_load = targetIdx;

  String loadedFile = filenameOnly(file_list[targetIdx]);
  sendJSON(client, 200,
    "{\"status\":\"ok\",\"file\":\"" + jsonEscape(loadedFile) +
    "\",\"game\":\"" + jsonEscape(name) +
    "\",\"disk\":" + String(diskNum) + "}");
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
  json += "\"internet_ssid\":\"" + jsonEscape(cfg_wifi_client_ssid) + "\",";
  json += "\"ftp_enabled\":" + String(cfg_ftp_enabled ? "true" : "false") + ",";
  json += "\"dav_enabled\":" + String(cfg_dav_enabled ? "true" : "false") + ",";
  json += "\"remote_enabled\":" + String(cfg_remote_enabled ? "true" : "false") + ",";
  json += "\"log_enabled\":" + String(cfg_log_enabled ? "true" : "false");
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
  json += "\"WALLPAPER_PCT\":\"" + String(cfg_wallpaper_pct) + "\",";
  json += "\"LOG_ENABLED\":\"" + String(cfg_log_enabled ? "1" : "0") + "\",";
  json += "\"WIFI_ENABLED\":\"" + String(cfg_wifi_enabled ? "1" : "0") + "\",";
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
  json += "\"DAV_HTTPS\":\"" + String(cfg_dav_https ? "1" : "0") + "\"";
  json += "}";

  sendJSON(client, 200, json);
}

// ============================================================================
// POST /api/config
// ============================================================================

void handleConfigPost(WiFiClient &client, const String &body) {
  String val;

  // Presence test, not emptiness: "0" is a meaningful value here and
  // getFormValue returns "" both for absent and for empty.
  if (body.indexOf("WALLPAPER_PCT=") >= 0) {
    int v = getFormValue(body, "WALLPAPER_PCT").toInt();
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    cfg_wallpaper_pct = v;
  }

  if (body.indexOf("LOG_ENABLED=") >= 0) {
    val = getFormValue(body, "LOG_ENABLED");
    cfg_log_enabled = (val == "1" || val == "true");
  }

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

  // FTP config
  val = getFormValue(body, "FTP_ENABLED");
  if (val.length() > 0) {
    cfg_ftp_enabled = (val == "1" || val == "true");
  }

  val = getFormValue(body, "FTP_HOST");
  if (val.length() > 0) cfg_ftp_host = val;
  else if (body.indexOf("FTP_HOST=") >= 0) cfg_ftp_host = "";  // allow clearing

  val = getFormValue(body, "FTP_PORT");
  if (val.length() > 0) {
    cfg_ftp_port = val.toInt();
    if (cfg_ftp_port <= 0) cfg_ftp_port = 21;
  }

  val = getFormValue(body, "FTP_USER");
  if (val.length() > 0) cfg_ftp_user = val;

  if (body.indexOf("FTP_PASS=") >= 0) {
    cfg_ftp_pass = getFormValue(body, "FTP_PASS");
  }

  val = getFormValue(body, "FTP_PATH");
  if (val.length() > 0) cfg_ftp_path = val;

  // WebDAV config
  val = getFormValue(body, "DAV_ENABLED");
  if (val.length() > 0) {
    cfg_dav_enabled = (val == "1" || val == "true");
  }

  val = getFormValue(body, "DAV_HOST");
  if (val.length() > 0) cfg_dav_host = val;
  else if (body.indexOf("DAV_HOST=") >= 0) cfg_dav_host = "";

  val = getFormValue(body, "DAV_PORT");
  if (val.length() > 0) {
    cfg_dav_port = val.toInt();
    if (cfg_dav_port <= 0) cfg_dav_port = 443;
  }

  val = getFormValue(body, "DAV_USER");
  if (val.length() > 0) cfg_dav_user = val;

  if (body.indexOf("DAV_PASS=") >= 0) {
    cfg_dav_pass = getFormValue(body, "DAV_PASS");
  }

  val = getFormValue(body, "DAV_PATH");
  if (val.length() > 0) cfg_dav_path = val;

  val = getFormValue(body, "DAV_HTTPS");
  if (val.length() > 0) {
    cfg_dav_https = (val == "1" || val == "true");
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

void handleCoverServe(WiFiClient &client, const String &mode, const String &name, const String &query = "") {
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
  if (req.contentLength > 256 * 1024) {
    unsigned long t = millis();
    while (client.available() && millis() - t < 3000) { client.read(); yield(); }
    sendJSON(client, 413, "{\"error\":\"Image too large. Max 256 KB.\"}");
    return true;
  }

  // Try folder from query string first, then lookup
  String gameDir = getFormValue(req.query, "folder");
  if (gameDir.length() == 0) gameDir = getGameFolder(name, mode);
  if (gameDir.length() == 0) {
    unsigned long t = millis();
    while (client.available() && millis() - t < 3000) { client.read(); yield(); }
    sendJSON(client, 404, "{\"error\":\"Game folder not found\"}");
    return true;
  }

  // Read all multipart data into buffer (max ~100KB after browser resize)
  int toRead = req.contentLength;
  uint8_t *buf = (uint8_t *)malloc(toRead);
  if (!buf) {
    unsigned long t = millis();
    while (client.available() && millis() - t < 3000) { client.read(); yield(); }
    sendJSON(client, 500, "{\"error\":\"Out of memory\"}");
    return true;
  }

  int pos = 0;
  unsigned long timeout = millis();
  while (pos < toRead && millis() - timeout < 10000) {
    if (client.available()) {
      int n = client.read(buf + pos, toRead - pos);
      if (n > 0) { pos += n; timeout = millis(); }
    } else {
      yield();
      delay(1);
    }
  }

  // Find file data boundaries in the buffer
  String delim = "\r\n--" + req.boundary;
  String headerEnd = "\r\n\r\n";
  size_t totalWritten = 0;
  String savePath = "";

  // Find first boundary (starts without leading \r\n)
  String firstDelim = "--" + req.boundary;
  int hdrStart = -1;
  for (int i = 0; i <= pos - (int)firstDelim.length(); i++) {
    if (memcmp(buf + i, firstDelim.c_str(), firstDelim.length()) == 0) {
      hdrStart = i + firstDelim.length();
      break;
    }
  }

  if (hdrStart >= 0) {
    // Find end of headers (\r\n\r\n)
    int dataStart = -1;
    for (int i = hdrStart; i <= pos - 4; i++) {
      if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
        dataStart = i + 4;
        break;
      }
    }

    if (dataStart >= 0) {
      // Find end boundary
      int dataEnd = pos;
      for (int i = dataStart; i <= pos - (int)delim.length(); i++) {
        if (memcmp(buf + i, delim.c_str(), delim.length()) == 0) {
          dataEnd = i;
          break;
        }
      }

      // Always save as .jpg (browser already converts to JPEG)
      String folderName = gameDir;
      int sl = folderName.lastIndexOf('/');
      if (sl >= 0) folderName = folderName.substring(sl + 1);
      savePath = gameDir + "/" + folderName + ".jpg";

      // Remove old .png cover if present
      String pngPath = gameDir + "/" + folderName + ".png";
      if (SD_MMC.exists(pngPath.c_str())) SD_MMC.remove(pngPath.c_str());

      // Write to SD
      File outFile = SD_MMC.open(savePath.c_str(), "w");
      if (outFile) {
        totalWritten = dataEnd - dataStart;
        outFile.write(buf + dataStart, totalWritten);
        outFile.close();
      }
    }
  }

  free(buf);

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
  Serial.println("CoverDL: " + name);
  if (!wifi_sta_connected) {
    sendJSON(client, 503, "{\"error\":\"No internet connection. Configure WiFi Client first.\"}");
    return;
  }

  String url = getFormValue(body, "url");

  if (url.length() == 0) {
    sendJSON(client, 400, "{\"error\":\"Missing url parameter\"}");
    return;
  }

  // Try folder from body first, then lookup
  String gameDir = getFormValue(body, "folder");
  if (gameDir.length() == 0) gameDir = getGameFolder(name, mode);
  if (gameDir.length() == 0) {
    sendJSON(client, 404, "{\"error\":\"Game folder not found\"}");
    return;
  }

  // Always save as .jpg — standardized format for cover art
  String folderName = gameDir;
  int lastSl = folderName.lastIndexOf('/');
  if (lastSl >= 0) folderName = folderName.substring(lastSl + 1);
  String savePath = gameDir + "/" + folderName + ".jpg";

  // Remove old .png cover if present
  String pngPath = gameDir + "/" + folderName + ".png";
  if (SD_MMC.exists(pngPath.c_str())) SD_MMC.remove(pngPath.c_str());

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

  // Reject files > 512KB to avoid memory issues
  if (contentLen > 512 * 1024) {
    httpClient->stop();
    sendJSON(client, 413, "{\"error\":\"Image too large (" + String(contentLen / 1024) + " KB). Max 512 KB.\"}");
    return;
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
// Theme authoring — used by the theme editor in the web UI
// ============================================================================

// Every BTN_* asset the firmware draws. A theme missing one falls back to a
// plain rect + label, which is what the whole UI looked like before the
// default theme covered the full set.
static const char *kThemeAssets[] = {
  "BTN_ADF", "BTN_BACK", "BTN_DAV", "BTN_DOWN", "BTN_DSK", "BTN_INFO",
  "BTN_LOAD", "BTN_SD", "BTN_THEME", "BTN_UNLOAD", "BTN_UP", "BTN_WIFI",
};
static const int kThemeAssetCount = sizeof(kThemeAssets) / sizeof(kThemeAssets[0]);

// Theme names become directory names on the card and are echoed into paths,
// so they get the strictest validation in the API: uppercase, digits and
// underscore only. No dots, no separators, nothing that can escape /THEMES/.
static bool validThemeName(const String &n) {
  if (n.length() == 0 || n.length() > 16) return false;
  for (unsigned i = 0; i < n.length(); i++) {
    const char c = n[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

static bool validThemeAsset(const String &a) {
  for (int i = 0; i < kThemeAssetCount; i++) {
    if (a == kThemeAssets[i]) return true;
  }
  return false;
}

// Drain a request body we are about to reject, so the socket stays in sync
// and the browser sees our status code instead of a connection reset.
static void drainBody(WiFiClient &client, int bytes) {
  unsigned long t = millis();
  int seen = 0;
  while (seen < bytes && millis() - t < 3000) {
    if (client.available()) { client.read(); seen++; }
    else { yield(); delay(1); }
  }
}

// GET /api/themes/font
//
// The firmware's 6x8 glyph table, base64'd. The theme editor renders its
// preview with the device's OWN font rather than a browser font, so what you
// see while designing is what the panel draws — and so a generated button's
// label matches the fallback label the firmware prints when an asset is
// missing. Serving it beats duplicating 570 bytes of glyphs in the web UI,
// where the two copies would drift.
void handleThemeFont(WiFiClient &client) {
  static const char *b64 =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t *src = (const uint8_t *)font6x8;
  const size_t len = sizeof(font6x8);

  String out;
  out.reserve((len + 2) / 3 * 4 + 32);
  out = "{\"cols\":6,\"rows\":8,\"first\":32,\"data\":\"";
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t n = ((uint32_t)pgm_read_byte(src + i) << 16) |
                       ((i + 1 < len ? (uint32_t)pgm_read_byte(src + i + 1) : 0) << 8) |
                       ((i + 2 < len ? (uint32_t)pgm_read_byte(src + i + 2) : 0));
    out += b64[(n >> 18) & 0x3F];
    out += b64[(n >> 12) & 0x3F];
    out += (i + 1 < len) ? b64[(n >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? b64[n & 0x3F] : '=';
  }
  out += "\"}";
  sendJSON(client, 200, out);
}

// GET /api/themes/geometry
//
// The button sizes this panel actually draws, so the editor renders assets at
// the right dimensions instead of hardcoding one display's numbers. This is
// what makes a theme portable across panel sizes without needing SVG on the
// device: the style is the theme, the PNGs are output for a given geometry.
void handleThemeGeometry(WiFiClient &client) {
  const int bar = (gW - 20 - 3 * 8) / 4;
  const int det = (148 < gW - 20) ? 148 : (gW - 20);
  String json = "{\"gW\":" + String(gW) + ",\"gH\":" + String(gH) + ",\"buttons\":[";
  struct { const char *n; int w; int h; const char *label; const char *glyph; } spec[] = {
    { "BTN_ADF",    bar, 36, "ADF",    "" },
    { "BTN_DSK",    bar, 36, "DSK",    "" },
    { "BTN_THEME",  bar, 36, "THEME",  "" },
    { "BTN_WIFI",   bar, 36, "WIFI",   "" },
    { "BTN_BACK",   bar, 36, "BACK",   "" },
    { "BTN_LOAD",   det, 36, "INSERT", "" },
    { "BTN_UNLOAD", det, 36, "EJECT",  "" },
    { "BTN_DAV",     44, 36, "DAV",    "" },
    { "BTN_SD",      40, 36, "SD",     "" },
    { "BTN_INFO",    40, 36, "i",      "" },
    { "BTN_UP",      44, 36, "",       "up" },
    { "BTN_DOWN",    44, 36, "",       "down" },
  };
  for (unsigned i = 0; i < sizeof(spec) / sizeof(spec[0]); i++) {
    if (i) json += ",";
    json += "{\"n\":\"" + String(spec[i].n) + "\",\"w\":" + String(spec[i].w) +
            ",\"h\":" + String(spec[i].h) +
            ",\"label\":\"" + String(spec[i].label) + "\"" +
            ",\"glyph\":\"" + String(spec[i].glyph) + "\"}";
  }
  json += "]}";
  sendJSON(client, 200, json);
}

// Decode base64url (RFC 4648 §5: '-' and '_' instead of '+' and '/', padding
// optional) into `out`. Returns the byte count, or -1 on a bad character.
//
// base64url specifically, not plain base64: request bodies go through
// urlDecode() before a handler sees them, so a '+' would arrive as a space
// and quietly corrupt every fourth byte.
static int b64urlDecode(const String &in, uint8_t *out, size_t outCap) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
  };
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  for (unsigned i = 0; i < in.length(); i++) {
    const char c = in[i];
    if (c == '=' || c == '\r' || c == '\n') continue;
    const int v = val(c);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= outCap) return -1;
      out[n++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return (int)n;
}

// POST /api/themes/{name}/asset
// Body: file=BTN_ADF&data=<base64url PNG>
//
// The PNG arrives base64'd in a normal form body rather than as a raw binary
// body, because the request reader in webserver.h consumes the body into
// req.body for every non-multipart POST before any handler runs — a handler
// reading the socket itself finds it already drained. Encoding costs a third
// more bytes on an asset of a couple of hundred, which is nothing, and it
// keeps this working with the server as it is instead of special-casing it.
//
// One asset per request: twelve small sequential writes are less code and
// less risk than extending the multipart parser, and the editor gets natural
// per-asset progress.
void handleThemeAssetUpload(WiFiClient &client, const HttpRequest &req, const String &name) {
  if (!validThemeName(name)) {
    sendJSON(client, 400, "{\"error\":\"Invalid theme name (A-Z 0-9 _ , max 16)\"}");
    return;
  }
  const String asset = getFormValue(req.body, "file");
  if (!validThemeAsset(asset)) {
    sendJSON(client, 400, "{\"error\":\"Unknown asset name\"}");
    return;
  }
  const String data = getFormValue(req.body, "data");
  // Generated buttons are ~200 bytes, so ~270 base64 chars; 64 KB of encoded
  // data is already absurdly generous.
  if (data.length() == 0 || data.length() > 64 * 1024) {
    sendJSON(client, 413, "{\"error\":\"Missing or oversized asset data\"}");
    return;
  }

  const size_t cap = (data.length() / 4) * 3 + 4;
  uint8_t *buf = (uint8_t *)malloc(cap);
  if (!buf) {
    sendJSON(client, 500, "{\"error\":\"Out of memory\"}");
    return;
  }
  const int pos = b64urlDecode(data, buf, cap);
  if (pos <= 0) {
    free(buf);
    sendJSON(client, 400, "{\"error\":\"Malformed base64 payload\"}");
    return;
  }
  // Reject anything that isn't actually a PNG before it reaches the card —
  // the firmware's decoder would just fail later, on screen, with no clue why.
  if (pos < 8 || buf[0] != 0x89 || buf[1] != 'P' || buf[2] != 'N' || buf[3] != 'G') {
    free(buf);
    sendJSON(client, 400, "{\"error\":\"Not a PNG\"}");
    return;
  }

  if (!SD_MMC.exists("/THEMES")) SD_MMC.mkdir("/THEMES");
  const String dir = "/THEMES/" + name;
  if (!SD_MMC.exists(dir.c_str())) SD_MMC.mkdir(dir.c_str());

  const String path = dir + "/" + asset + ".png";
  File f = SD_MMC.open(path.c_str(), "w");
  if (!f) {
    free(buf);
    sendJSON(client, 500, "{\"error\":\"Cannot write asset\"}");
    return;
  }
  const size_t written = f.write(buf, pos);
  f.close();
  free(buf);

  if ((int)written != pos) {
    SD_MMC.remove(path.c_str());
    sendJSON(client, 500, "{\"error\":\"Short write — card full?\"}");
    return;
  }

  scanThemes();          // a brand-new theme must appear in the list
  clearButtonCache();    // cached tiles may belong to the asset just replaced
  sdLog("Theme asset written: " + path + " (" + String((unsigned)written) + " B)");
  sendJSON(client, 200, "{\"status\":\"ok\",\"asset\":\"" + jsonEscape(asset) +
                        "\",\"bytes\":" + String((unsigned)written) + "}");
}

// DELETE /api/themes/{name}
void handleThemeDelete(WiFiClient &client, const String &name) {
  if (!validThemeName(name)) {
    sendJSON(client, 400, "{\"error\":\"Invalid theme name\"}");
    return;
  }
  if (name == cfg_theme) {
    sendJSON(client, 409, "{\"error\":\"Cannot delete the active theme\"}");
    return;
  }
  const String dir = "/THEMES/" + name;
  if (!SD_MMC.exists(dir.c_str())) {
    sendJSON(client, 404, "{\"error\":\"Theme not found\"}");
    return;
  }
  // Only ever removes the known asset filenames, so a mis-routed request can
  // never turn into a recursive delete of something else.
  for (int i = 0; i < kThemeAssetCount; i++) {
    const String p = dir + "/" + kThemeAssets[i] + ".png";
    if (SD_MMC.exists(p.c_str())) SD_MMC.remove(p.c_str());
  }
  SD_MMC.rmdir(dir.c_str());
  scanThemes();
  sdLog("Theme deleted: " + name);
  sendJSON(client, 200, "{\"status\":\"ok\"}");
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

// ============================================================================
// FTP API Handlers
// ============================================================================

// GET /api/ftp/status — FTP connection status and config
void handleFTPStatus(WiFiClient &client) {
  String json = "{";
  json += "\"enabled\":" + String(cfg_ftp_enabled ? "true" : "false");
  json += ",\"host\":\"" + jsonEscape(cfg_ftp_host) + "\"";
  json += ",\"port\":" + String(cfg_ftp_port);
  json += ",\"user\":\"" + jsonEscape(cfg_ftp_user) + "\"";
  json += ",\"path\":\"" + jsonEscape(cfg_ftp_path) + "\"";
  json += ",\"connected\":" + String(ftpClient.isConnected() ? "true" : "false");
  json += ",\"wifi_connected\":" + String((WiFi.status() == WL_CONNECTED) ? "true" : "false");
  json += "}";
  sendJSON(client, 200, json);
}

// POST /api/ftp/connect — Connect to FTP server
void handleFTPConnect(WiFiClient &client) {
  if (!cfg_ftp_enabled) {
    sendJSON(client, 400, "{\"error\":\"FTP not enabled in config\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendJSON(client, 503, "{\"error\":\"WiFi client not connected to network\"}");
    return;
  }
  if (ftpClient.isConnected()) {
    ftpClient.disconnect();
  }
  if (ftpClient.connect()) {
    sendJSON(client, 200, "{\"status\":\"connected\"}");
  } else {
    sendJSON(client, 503, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}");
  }
}

// POST /api/ftp/disconnect — Disconnect from FTP server
void handleFTPDisconnect(WiFiClient &client) {
  ftpClient.disconnect();
  sendJSON(client, 200, "{\"status\":\"disconnected\"}");
}

// GET /api/ftp/list?path=/subdir — List FTP directory
void handleFTPList(WiFiClient &client, const String &queryPath) {
  if (!ftpClient.isConnected()) {
    // Auto-connect if configured
    if (cfg_ftp_enabled && WiFi.status() == WL_CONNECTED) {
      if (!ftpClient.connect()) {
        sendJSON(client, 503, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}");
        return;
      }
    } else {
      sendJSON(client, 503, "{\"error\":\"FTP not connected\"}");
      return;
    }
  }

  String path = queryPath;
  if (path.length() == 0) path = "/";

  std::vector<FTPFileEntry> entries;
  if (!ftpClient.listDir(path, entries)) {
    sendJSON(client, 500, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}");
    return;
  }

  // Build JSON response
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
}

// POST /api/ftp/download — Download file from FTP to SD card
// Body: path=/subdir/game.adf
void handleFTPDownload(WiFiClient &client, const String &body) {
  if (!ftpClient.isConnected()) {
    sendJSON(client, 503, "{\"error\":\"FTP not connected\"}");
    return;
  }

  // Parse path from body
  String remotePath = "";
  int pathIdx = body.indexOf("path=");
  if (pathIdx >= 0) {
    remotePath = body.substring(pathIdx + 5);
    int ampIdx = remotePath.indexOf("&");
    if (ampIdx >= 0) remotePath = remotePath.substring(0, ampIdx);
    remotePath = urlDecode(remotePath);
  }

  if (remotePath.length() == 0) {
    sendJSON(client, 400, "{\"error\":\"Missing path parameter\"}");
    return;
  }

  // Determine filename and local destination
  String filename = remotePath;
  int lastSlash = filename.lastIndexOf('/');
  if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);

  // Extract game name (strip extension and disk number)
  String gameName = filename;
  int dotIdx = gameName.lastIndexOf('.');
  if (dotIdx > 0) gameName = gameName.substring(0, dotIdx);
  // Remove disk numbers like "-1", "-2", "(Disk 1)" etc
  gameName.replace("(Disk 1)", "");
  gameName.replace("(Disk 2)", "");
  gameName.replace("(Disk 3)", "");
  gameName.replace("(Disk 4)", "");
  gameName.trim();

  // Save to /ADF/{GameName}/ or /DSK/{GameName}/
  String modeDir = (g_mode == MODE_ADF) ? "/ADF" : "/DSK";
  String lowerName = filename;
  lowerName.toLowerCase();
  // Auto-detect mode from extension
  if (lowerName.endsWith(".dsk")) modeDir = "/DSK";
  else if (lowerName.endsWith(".adf") || lowerName.endsWith(".adz")) modeDir = "/ADF";

  String gameDir = modeDir + "/" + gameName;
  SD_MMC.mkdir(gameDir.c_str());
  String localPath = gameDir + "/" + filename;

  Serial.println("FTP: downloading " + remotePath + " -> " + localPath);

  long bytes = ftpClient.downloadFile(remotePath, localPath);
  if (bytes < 0) {
    sendJSON(client, 500, "{\"error\":\"" + jsonEscape(ftpClient.lastError()) + "\"}");
    return;
  }

  // Rescan game list
  file_list = listImages();
  buildDisplayNames(file_list);
  sortByDisplay();
  buildGameList();

  sendJSON(client, 200, "{\"status\":\"ok\",\"file\":\"" + jsonEscape(filename) + "\",\"bytes\":" + String(bytes) +
    ",\"game\":\"" + jsonEscape(gameName) + "\"}");
}

// ============================================================================
// WebDAV API Handlers
// ============================================================================

// GET /api/dav/status — WebDAV connection status and config
void handleDAVStatus(WiFiClient &client) {
  String json = "{";
  json += "\"enabled\":" + String(cfg_dav_enabled ? "true" : "false");
  json += ",\"host\":\"" + jsonEscape(cfg_dav_host) + "\"";
  json += ",\"port\":" + String(cfg_dav_port);
  json += ",\"user\":\"" + jsonEscape(cfg_dav_user) + "\"";
  json += ",\"path\":\"" + jsonEscape(cfg_dav_path) + "\"";
  json += ",\"https\":" + String(cfg_dav_https ? "true" : "false");
  json += ",\"connected\":" + String(davClient.isConnected() ? "true" : "false");
  json += ",\"wifi_connected\":" + String((WiFi.status() == WL_CONNECTED) ? "true" : "false");
  String dbg = davClient.lastDebug();
  if (dbg.length() > 0) {
    json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
  }
  String err = davClient.lastError();
  if (err.length() > 0) {
    json += ",\"error\":\"" + jsonEscape(err) + "\"";
  }
  // Tell web UI if a cache exists (so it can show games without connecting first)
  bool hasCache = (dav_entries.size() > 0) || SD_MMC.exists(DAV_CACHE_FILE);
  json += ",\"has_cache\":" + String(hasCache ? "true" : "false");

  // Include global now-playing state so web UI knows what's loaded
  if (nowPlaying.source != NP_NONE) {
    json += ",\"now_playing\":{";
    json += "\"source\":\"" + String(nowPlaying.source == NP_DAV ? "dav" : "sd") + "\"";
    json += ",\"name\":\"" + jsonEscape(nowPlaying.name) + "\"";
    json += ",\"path\":\"" + jsonEscape(nowPlaying.path) + "\"";
    json += "}";
  }
  json += "}";
  sendJSON(client, 200, json);
}

// POST /api/dav/connect — Connect to WebDAV server
void handleDAVConnect(WiFiClient &client) {
  sdLog("API: DAV connect request");
  if (!cfg_dav_enabled) {
    sdLog("API: DAV not enabled in config");
    sendJSON(client, 400, "{\"error\":\"WebDAV not enabled in config\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sdLog("API: WiFi not connected (status=" + String(WiFi.status()) + ")");
    sendJSON(client, 503, "{\"error\":\"WiFi client not connected to network\"}");
    return;
  }
  sdLog("API: DAV connecting to " + cfg_dav_host + ":" + String(cfg_dav_port) +
        " user=" + cfg_dav_user + " https=" + String(cfg_dav_https) +
        " path=" + cfg_dav_path);
  if (davClient.connect()) {
    sdLog("API: DAV connected OK");
    String json = "{\"status\":\"connected\"";
    String dbg = davClient.lastDebug();
    if (dbg.length() > 0) json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
    json += "}";
    sendJSON(client, 200, json);
  } else {
    sdLog("API: DAV connect FAILED: " + davClient.lastError());
    String json = "{\"error\":\"" + jsonEscape(davClient.lastError()) + "\"";
    String dbg = davClient.lastDebug();
    if (dbg.length() > 0) json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
    json += "}";
    sendJSON(client, 503, json);
  }
}

// POST /api/dav/disconnect — Disconnect from WebDAV server
void handleDAVDisconnect(WiFiClient &client) {
  davClient.disconnect();
  sendJSON(client, 200, "{\"status\":\"disconnected\"}");
}

// GET /api/dav/list?path=/subdir&refresh=1 — List WebDAV directory
// Uses SD card cache for root listing unless refresh=1 is specified
void handleDAVList(WiFiClient &client, const String &queryPath, bool forceRefresh) {
  sdLog("API: DAV list path=" + queryPath + " refresh=" + String(forceRefresh));
  if (!cfg_dav_enabled) {
    sendJSON(client, 400, "{\"error\":\"WebDAV not enabled\"}");
    return;
  }

  String path = queryPath;
  if (path.length() == 0) path = "/";

  // For root path: try returning cached data first (unless forced refresh)
  if (path == "/" && !forceRefresh) {
    // Check in-memory cache first
    if (dav_entries.size() > 0) {
      // Build JSON from in-memory entries
      String json = "{\"path\":\"/\",\"cached\":true,\"entries\":[";
      bool first = true;
      for (int i = 0; i < (int)dav_entries.size(); i++) {
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + jsonEscape(dav_entries[i].name()) + "\"";
        json += ",\"dir\":" + String(dav_entries[i].isDir ? "true" : "false");
        json += ",\"size\":" + String(dav_entries[i].size);
        json += "}";
      }
      json += "]}";
      sendJSON(client, 200, json);
      return;
    }
    // Try SD card cache
    if (davLoadCache()) {
      String json = "{\"path\":\"/\",\"cached\":true,\"entries\":[";
      bool first = true;
      for (int i = 0; i < (int)dav_entries.size(); i++) {
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + jsonEscape(dav_entries[i].name()) + "\"";
        json += ",\"dir\":" + String(dav_entries[i].isDir ? "true" : "false");
        json += ",\"size\":" + String(dav_entries[i].size);
        json += "}";
      }
      json += "]}";
      sendJSON(client, 200, json);
      return;
    }
  }

  // No cache available or forced refresh — do PROPFIND
  if (WiFi.status() != WL_CONNECTED) {
    sendJSON(client, 503, "{\"error\":\"WiFi not connected\"}");
    return;
  }

  sdLog("API: DAV PROPFIND for path=" + path);
  DAVEntryList entries;
  bool listOk;
  {
    // Web-triggered PROPFIND is the exact same power stack as the
    // device-triggered one — dim the LCD for the WiFi RX burst so the
    // Amiga 5V rail doesn't dip while we hold TLS + SD state.
    BacklightDip _dip;
    listOk = davClient.listDir(path, entries);
  }
  if (!listOk) {
    sdLog("API: DAV list FAILED: " + davClient.lastError());
    sendJSON(client, 500, "{\"error\":\"" + jsonEscape(davClient.lastError()) + "\"}");
    return;
  }
  sdLog("API: DAV list OK, " + String(entries.size()) + " entries");

  // Separate cover/nfo metadata from browsable entries. A file entry carries
  // hasCover/hasNfo meaning "this file IS the cover / the NFO", so its own
  // name is the filename we advertise.
  String coverFile = "", nfoFile = "";
  for (int i = 0; i < (int)entries.size(); i++) {
    if (entries[i].hasCover && coverFile.length() == 0) coverFile = entries[i].name();
    if (entries[i].hasNfo   && nfoFile.length()   == 0) nfoFile   = entries[i].name();
  }

  // Build JSON response — skip cover/nfo files from browsable list
  String json = "{\"path\":\"" + jsonEscape(path) + "\"";
  if (coverFile.length() > 0) json += ",\"cover\":\"" + jsonEscape(coverFile) + "\"";
  if (nfoFile.length() > 0)   json += ",\"nfo\":\"" + jsonEscape(nfoFile) + "\"";
  json += ",\"entries\":[";
  bool first = true;
  for (int i = 0; i < (int)entries.size(); i++) {
    if (!entries[i].isDir && (entries[i].hasCover || entries[i].hasNfo)) continue;
    if (!first) json += ",";
    first = false;
    json += "{\"name\":\"" + jsonEscape(entries[i].name()) + "\"";
    json += ",\"dir\":" + String(entries[i].isDir ? "true" : "false");
    json += ",\"size\":" + String(entries[i].size);
    json += "}";
  }
  json += "]";
  String dbg = davClient.lastDebug();
  if (dbg.length() > 0) json += ",\"debug\":\"" + jsonEscape(dbg) + "\"";
  json += "}";
  sendJSON(client, 200, json);

  // Update in-memory and SD cache for root listing. Moving rather than
  // copying matters here: the element-by-element copy this replaces held two
  // complete 3000-entry listings live at the same time, doubling the peak for
  // no reason — `entries` is a local about to go out of scope.
  if (path == "/") {
    dav_entries = std::move(entries);
    davSaveCache();
    buildDAVActiveLetters();
    // Start background cover pre-caching
    davStartCoverPrecache();
  }
}

// POST /api/dav/download — Download file from WebDAV to SD card
// Body: path=/subdir/game.adf
void handleDAVDownload(WiFiClient &client, const String &body) {
  if (!cfg_dav_enabled) {
    sendJSON(client, 400, "{\"error\":\"WebDAV not enabled\"}");
    return;
  }

  // Parse path from body
  String remotePath = "";
  int pathIdx = body.indexOf("path=");
  if (pathIdx >= 0) {
    remotePath = body.substring(pathIdx + 5);
    int ampIdx = remotePath.indexOf("&");
    if (ampIdx >= 0) remotePath = remotePath.substring(0, ampIdx);
    remotePath = urlDecode(remotePath);
  }

  if (remotePath.length() == 0) {
    sendJSON(client, 400, "{\"error\":\"Missing path parameter\"}");
    return;
  }

  // Determine filename and local destination
  String filename = remotePath;
  int lastSlash = filename.lastIndexOf('/');
  if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);

  // Extract game name (strip extension and disk number)
  String gameName = filename;
  int dotIdx = gameName.lastIndexOf('.');
  if (dotIdx > 0) gameName = gameName.substring(0, dotIdx);
  gameName.replace("(Disk 1)", "");
  gameName.replace("(Disk 2)", "");
  gameName.replace("(Disk 3)", "");
  gameName.replace("(Disk 4)", "");
  gameName.trim();

  // Save to /ADF/{GameName}/ or /DSK/{GameName}/
  String modeDir = (g_mode == MODE_ADF) ? "/ADF" : "/DSK";
  String lowerName = filename;
  lowerName.toLowerCase();
  if (lowerName.endsWith(".dsk")) modeDir = "/DSK";
  else if (lowerName.endsWith(".adf") || lowerName.endsWith(".adz")) modeDir = "/ADF";

  String gameDir = modeDir + "/" + gameName;
  SD_MMC.mkdir(gameDir.c_str());
  String localPath = gameDir + "/" + filename;

  Serial.println("DAV: downloading " + remotePath + " -> " + localPath);

  long bytes = davClient.downloadFile(remotePath, localPath);
  if (bytes < 0) {
    sendJSON(client, 500, "{\"error\":\"" + jsonEscape(davClient.lastError()) + "\"}");
    return;
  }

  // Rescan game list
  file_list = listImages();
  buildDisplayNames(file_list);
  sortByDisplay();
  buildGameList();

  sendJSON(client, 200, "{\"status\":\"ok\",\"file\":\"" + jsonEscape(filename) + "\",\"bytes\":" + String(bytes) +
    ",\"game\":\"" + jsonEscape(gameName) + "\"}");
}

// ============================================================================
// POST /api/dav/load — Load disk image from WebDAV directly into RAM
// Body: path=/subdir/game.adf
// ============================================================================

void handleDAVLoad(WiFiClient &client, const String &body) {
  if (!cfg_dav_enabled) {
    sendJSON(client, 400, "{\"error\":\"WebDAV not enabled\"}");
    return;
  }

  // Parse path from body
  String remotePath = "";
  int pathIdx = body.indexOf("path=");
  if (pathIdx >= 0) {
    remotePath = body.substring(pathIdx + 5);
    int ampIdx = remotePath.indexOf("&");
    if (ampIdx >= 0) remotePath = remotePath.substring(0, ampIdx);
    remotePath = urlDecode(remotePath);
  }

  if (remotePath.length() == 0) {
    sendJSON(client, 400, "{\"error\":\"Missing path parameter\"}");
    return;
  }

  // Extract display name from path
  String displayName = remotePath;
  int lastSlash = displayName.lastIndexOf('/');
  if (lastSlash >= 0) displayName = displayName.substring(lastSlash + 1);
  int dotIdx = displayName.lastIndexOf('.');
  if (dotIdx > 0) displayName = displayName.substring(0, dotIdx);

  Serial.println("Web DAV load (deferred): " + remotePath);

  // Defer the actual DAV streaming to the main loop — send response immediately
  web_pending_dav_path = remotePath;
  web_pending_dav_name = displayName;

  sendJSON(client, 200,
    "{\"status\":\"ok\",\"file\":\"" + jsonEscape(remotePath) +
    "\",\"name\":\"" + jsonEscape(displayName) + "\"}");
}

// ============================================================================
// GET /api/dav/cover?path=/folder/cover.jpg — Proxy cover image from WebDAV
// ============================================================================

void handleDAVCover(WiFiClient &client, const String &queryPath) {
  if (!cfg_dav_enabled || queryPath.length() == 0) {
    sendJSON(client, 400, "{\"error\":\"Invalid request\"}");
    return;
  }

  String lp = queryPath; lp.toLowerCase();
  String ct = lp.endsWith(".png") ? "image/png" : "image/jpeg";

  // Fast path — cover is on the SD cache. Stream 4 KB chunks straight from
  // the file to the socket; no ps_malloc, no 150 KB copy, no double buffer.
  // Also sets ETag + immutable so a warmed browser cache serves subsequent
  // requests without ever hitting us.
  String cachePath = davCoverCachePath(queryPath);
  if (SD_MMC.exists(cachePath.c_str())) {
    File f = SD_MMC.open(cachePath.c_str(), "r");
    if (f) {
      size_t sz = f.size();
      char etag[24];
      snprintf(etag, sizeof(etag), "\"%08lx-%lx\"",
               (unsigned long)sz,
               (unsigned long)(cachePath.length()));
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: " + ct);
      client.println("Content-Length: " + String((uint32_t)sz));
      client.println("Cache-Control: max-age=604800, immutable");
      client.print("ETag: "); client.println(etag);
      client.println("Connection: close");
      client.println();
      // static, not a stack local: the loop task has one 8 KB stack shared by
    // the HTTP handlers, the TLS client, SD I/O and the whole render path,
    // and a 4 KB frame here left an mbedTLS handshake with ~2 KB. The web
    // server is polled from loop() and never reentered, so static is safe.
      static uint8_t chunk[1024];
      while (f.available()) {
        int n = f.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        client.write(chunk, n);
        yield();
      }
      f.close();
      return;
    }
  }

  // Persisted "known to have no cover" marker from davPrecacheOneCover.
  // Answer immediately without touching the network — otherwise every web
  // grid render hits TLS again.
  if (SD_MMC.exists((cachePath + ".miss").c_str())) {
    sendJSON(client, 404, "{\"error\":\"Cover not found (cached miss)\"}");
    return;
  }

  // Not cached. Hand it to the background fetcher and answer NOW.
  //
  // This used to download it here — a TLS handshake plus transfer inside the
  // request handler, on the loop task. Opening a letter in the web UI renders
  // dozens of <img> tags at once, so that was dozens of sequential network
  // round trips with the touchscreen, the USB service and every other request
  // frozen behind them. A log from a real session showed a single one of these
  // blocking for 362 seconds, and an ADF download timing out at 374 KB of 880
  // because it was competing with the flood.
  //
  // The browser gets a 404 now and shows its "No Art" placeholder; the cover
  // appears on the next render once the idle pump has fetched it. Nothing in
  // a request handler is allowed to touch the network.
  davQueueCoverFetch(queryPath);
  client.println("HTTP/1.1 404 Not Found");
  client.println("Content-Type: application/json");
  // Briefly cacheable: the browser should retry soon (the fetch is queued),
  // but not on every single scroll.
  client.println("Cache-Control: max-age=30");
  client.println("Connection: close");
  client.println();
  client.println("{\"error\":\"Cover queued\",\"queued\":true}");
}

// ============================================================================
// GET /api/dav/nfo?path=/folder/game.nfo — Proxy NFO text from WebDAV
// ============================================================================

void handleDAVNfo(WiFiClient &client, const String &queryPath) {
  if (!cfg_dav_enabled || queryPath.length() == 0) {
    sendJSON(client, 400, "{\"error\":\"Invalid request\"}");
    return;
  }

  // Small buffer for NFO text (max 2KB)
  uint8_t buf[2048];
  long bytes = davClient.streamToBuffer(queryPath, buf, sizeof(buf) - 1);
  if (bytes <= 0) {
    sendJSON(client, 404, "{\"error\":\"NFO not found\"}");
    return;
  }
  buf[bytes] = 0;  // null-terminate

  String nfoText = String((char *)buf);
  sendJSON(client, 200, "{\"nfo\":\"" + jsonEscape(nfoText) + "\"}");
}

// ============================================================================
// GET /api/log — Read LOG.TXT from SD card
// ============================================================================

void handleLogGet(WiFiClient &client) {
  if (!cfg_log_enabled) {
    sendJSON(client, 200, "{\"log\":\"Logging is disabled. Set LOG_ENABLED=1 in CONFIG.TXT\",\"enabled\":false}");
    return;
  }
  if (!SD_MMC.exists("/LOG.TXT")) {
    sendJSON(client, 200, "{\"log\":\"No log file found.\",\"enabled\":true}");
    return;
  }
  File f = SD_MMC.open("/LOG.TXT", "r");
  if (!f) {
    sendJSON(client, 200, "{\"log\":\"Could not open log file.\",\"enabled\":true}");
    return;
  }
  // Read up to 32KB of log (tail if larger)
  size_t sz = f.size();
  size_t maxRead = 32768;
  String logText = "";
  if (sz > maxRead) {
    f.seek(sz - maxRead);
    logText = "... (truncated, showing last 32KB) ...\n";
  }
  while (f.available()) {
    logText += f.readStringUntil('\n') + "\n";
  }
  f.close();
  sendJSON(client, 200, "{\"log\":\"" + jsonEscape(logText) + "\",\"enabled\":true}");
}

// GET /api/log/clear — Clear LOG.TXT
void handleLogClear(WiFiClient &client) {
  if (SD_MMC.exists("/LOG.TXT")) {
    File f = SD_MMC.open("/LOG.TXT", "w");
    if (f) {
      f.println("=== Log cleared from web interface ===");
      f.close();
    }
  }
  sendJSON(client, 200, "{\"ok\":true}");
}

#endif // API_HANDLERS_H
