#pragma once
//
// The dongle's HTTP layer.
//
// It serves the SAME web UI as the touchscreen — the identical gzipped webui.h,
// 35 KB in flash — rather than a second page with a second look. A dongle and a
// touchscreen should not feel like different products, and a separate page
// would drift the moment either was touched.
//
// What differs is the API behind it. The page already hides what a device does
// not report (that is how the FTP, WebDAV and Log tabs work), so the dongle
// answers /api/system/info honestly and the sections it cannot support simply
// are not there.

#include <Arduino.h>
#include <WiFi.h>
#include <Update.h>
#include "../Gotek_Touchscreen/webui.h"

// ── Tiny HTTP plumbing ───────────────────────────────────────────────────

// ── Request bits ─────────────────────────────────────────────────────────

static String urlDecode(const String &in) {
  String out;
  out.reserve(in.length());
  for (unsigned int i = 0; i < in.length(); i++) {
    const char c = in[i];
    if (c == '+') out += ' ';
    else if (c == '%' && i + 2 < in.length()) {
      const char h[3] = { in[i + 1], in[i + 2], 0 };
      out += (char)strtol(h, nullptr, 16);
      i += 2;
    } else out += c;
  }
  return out;
}

// One value out of "a=1&b=2". Matches whole keys only, so "path" does not also
// match "np_path".
static String pairValue(const String &blob, const String &key) {
  const String needle = key + "=";
  int at = -1;
  if (blob.startsWith(needle)) at = 0;
  else {
    const int i = blob.indexOf("&" + needle);
    if (i >= 0) at = i + 1;
  }
  if (at < 0) return "";
  const int from = at + needle.length();
  const int amp = blob.indexOf('&', from);
  return urlDecode(amp < 0 ? blob.substring(from) : blob.substring(from, amp));
}
static String queryValue(const String &q, const String &key) { return pairValue(q, key); }
static String formValue(const String &b, const String &key)  { return pairValue(b, key); }

static String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 16);
  for (unsigned int i = 0; i < in.length(); i++) {
    const char c = in[i];
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') { }
    else if ((uint8_t)c < 0x20) { }
    else out += c;
  }
  return out;
}

static void sendHeader(WiFiClient &c, int code, const char *type, int len = -1) {
  c.print("HTTP/1.1 ");
  c.print(code);
  c.println(code == 200 ? " OK" : (code == 404 ? " Not Found" : " Error"));
  c.print("Content-Type: ");
  c.println(type);
  if (len >= 0) { c.print("Content-Length: "); c.println(len); }
  c.println("Cache-Control: no-store");
  c.println("Connection: close");
  c.println();
}

static void sendJson(WiFiClient &c, int code, const String &body) {
  sendHeader(c, code, "application/json", body.length());
  c.print(body);
}

// The image goes straight into the RAM disk, so the Gotek must not be reading
// it while it arrives. Returns bytes written, or a negative error.
static long receiveImage(WiFiClient &client, const String &boundary,
                         long contentLength, String &nameOut) {
  const String delim = "--" + boundary;
  MultipartBody body;
  if (!body.begin(delim.c_str(), (int)delim.length())) return -1;

  // Headers of the first part: we only need the filename.
  bool inData = false;
  unsigned long deadline = millis() + 10000;
  while (client.connected() && !inData && millis() < deadline) {
    if (!client.available()) { delay(2); continue; }
    String line = client.readStringUntil('\n');
    line.trim();
    const int fn = line.indexOf("filename=\"");
    if (fn >= 0) {
      const int end = line.indexOf('"', fn + 10);
      nameOut = line.substring(fn + 10, end);
    }
    // A blank line after the part headers means the body starts here.
    if (line.length() == 0 && nameOut.length() > 0) inData = true;
  }
  if (!inData) return -2;

  tud_disconnect();     // no reads while the image is half-written
  delay(30);
  g_mountBytes = 0;     // stop save tracking against the outgoing image
  svReset();

  size_t written = 0;
  bool overflow = false;
  uint8_t rd[1024];
  unsigned long lastByte = millis();

  while (client.connected()) {
    if (millis() - lastByte > 10000) break;         // stalled
    const int avail = client.available();
    if (avail <= 0) { delay(2); continue; }
    const int n = client.readBytes(rd, min((int)sizeof(rd), avail));
    if (n <= 0) continue;
    lastByte = millis();

    const bool done = body.feed(rd, n, [&](const uint8_t *p, int len) {
      if (written + len > (size_t)MAX_IMAGE_BYTES) {
        overflow = true;
        return;
      }
      memcpy(&ram_disk[DATA_OFFSET + written], p, len);
      written += len;
    });
    if (done || overflow) break;
    yield();
  }

  if (overflow) return -3;
  if (written == 0) return -4;
  return (long)written;
}


// Stream an uploaded firmware straight into the spare OTA slot.
//
// Same shape as receiveImage() above — the multipart scanner does the framing,
// we just change where the bytes land. There is no room to buffer 1.1 MB and
// no reason to: Update writes it to flash as it arrives.
//
// The magic-byte check matters more here than anywhere else. This board has no
// screen and no card; if it reboots into a slot holding something that is not
// firmware, the only way back is the cable this feature exists to retire.
static long receiveFirmware(WiFiClient &client, const String &boundary,
                            String &nameOut) {
  const String delim = "--" + boundary;
  MultipartBody body;
  if (!body.begin(delim.c_str(), (int)delim.length())) return -1;

  bool inData = false;
  unsigned long deadline = millis() + 10000;
  while (client.connected() && !inData && millis() < deadline) {
    if (!client.available()) { delay(2); continue; }
    String line = client.readStringUntil('\n');
    line.trim();
    const int fn = line.indexOf("filename=\"");
    if (fn >= 0) {
      const int end = line.indexOf('"', fn + 10);
      nameOut = line.substring(fn + 10, end);
    }
    if (line.length() == 0 && nameOut.length() > 0) inData = true;
  }
  if (!inData) return -2;

  if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    dlog("OTA: cannot start: " + String(Update.errorString()));
    Update.abort();
    return -3;
  }

  size_t written = 0;
  bool failed = false, badMagic = false;
  uint8_t rd[1024];
  unsigned long lastByte = millis();

  while (client.connected()) {
    if (millis() - lastByte > 15000) { failed = true; break; }
    const int avail = client.available();
    if (avail <= 0) { delay(2); continue; }
    const int n = client.readBytes(rd, min((int)sizeof(rd), avail));
    if (n <= 0) continue;
    lastByte = millis();

    const bool done = body.feed(rd, n, [&](const uint8_t *p, int len) {
      if (failed || badMagic) return;
      if (written == 0 && len > 0 && p[0] != 0xE9) { badMagic = true; return; }
      if (Update.write((uint8_t *)p, len) != (size_t)len) { failed = true; return; }
      written += len;
    });
    if (done || failed || badMagic) break;
    yield();
  }

  if (badMagic) { Update.abort(); dlog("OTA: refused, not a firmware image"); return -4; }
  if (failed || written == 0) {
    Update.abort();
    dlog("OTA: FAILED after " + String((uint32_t)written) + " bytes");
    return -5;
  }
  if (!Update.end(true) || !Update.isFinished()) {
    dlog("OTA: could not finalise: " + String(Update.errorString()));
    return -6;
  }
  dlog("OTA: wrote " + String((uint32_t)written) + " bytes");
  return (long)written;
}


// Pull the mounted game's cover and notes into PSRAM.
//
// One extra round trip each, on insert only. Timed separately so the cost shows
// up in the summary rather than hiding inside "insert took a while".
static void cacheArtFor(const String &remoteDiskPath, Perf &perf) {
  String dir = remoteDiskPath;
  const int sl = dir.lastIndexOf('/');
  if (sl <= 0) return;
  dir = dir.substring(0, sl);
  String folder = dir;
  const int sl2 = folder.lastIndexOf('/');
  if (sl2 >= 0) folder = folder.substring(sl2 + 1);

  g_coverLen = 0; g_coverPath = "";
  g_nfoText  = ""; g_nfoPath   = "";
  if (!g_coverBuf) g_coverBuf = (uint8_t *)ps_malloc(COVER_MAX_BYTES);

  if (g_coverBuf) {
    // The convention the rest of the project uses: <Folder>/<Folder>.jpg
    for (int i = 0; i < 2 && g_coverLen == 0; i++) {
      const String cp = dir + "/" + folder + (i == 0 ? ".jpg" : ".png");
      const long n = davClient.streamToBuffer(cp, g_coverBuf, COVER_MAX_BYTES);
      if (n > 0 && !davClient.lastTruncated()) {
        g_coverLen = (size_t)n;
        g_coverPath = cp;
      }
    }
  }
  perf.mark("cover");

  static uint8_t nfoBuf[2048];
  const String np = dir + "/" + folder + ".nfo";
  const long nn = davClient.streamToBuffer(np, nfoBuf, sizeof(nfoBuf) - 1);
  if (nn > 0) {
    nfoBuf[nn] = 0;
    g_nfoText = String((char *)nfoBuf);
    g_nfoPath = np;
  }
  perf.mark("notes");
}


// ── Remembering the last folder ──────────────────────────────────────────
//
// The page asks for the same folder several times in a row — after a load it
// re-renders the now-playing bar and the list, and each of those wants the
// listing again. Measured on a real server that is 300-480 ms apiece, almost
// all of it TCP and TLS setup rather than the four kilobytes of XML.
//
// So the answer is kept for a few seconds. Long enough to collapse one burst of
// identical questions, short enough that a folder you changed on the server
// looks stale for a moment at worst. There is no card to keep it on, so it
// lives in the heap and holds exactly one folder.
#define DAV_LIST_CACHE_MS 15000
// Only small listings are worth keeping. A 1009-entry root serialises to about
// 55 KB, and holding that in the internal heap is what made an insert panic:
// TLS then wants ~46 KB on top and the transfer has nothing left to allocate
// from. The repeated-request problem this cache solves is a folder view, which
// is a handful of entries — so cap it and let big listings cost their PROPFIND.
#define DAV_LIST_CACHE_MAX 8192
static String   g_listCachePath = "";
static String   g_listCacheJson = "";
static uint32_t g_listCacheAt   = 0;

static bool listCacheHit(const String &path) {
  return g_listCachePath == path && g_listCacheJson.length() > 0 &&
         (millis() - g_listCacheAt) < DAV_LIST_CACHE_MS;
}

static void handleClient(WiFiClient &client) {
  const unsigned long deadline = millis() + 5000;
  while (!client.available() && millis() < deadline) delay(2);
  if (!client.available()) { client.stop(); return; }

  String reqLine = client.readStringUntil('\n');
  reqLine.trim();
  const int sp1 = reqLine.indexOf(' ');
  const int sp2 = reqLine.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) { client.stop(); return; }
  const String method = reqLine.substring(0, sp1);
  String path = reqLine.substring(sp1 + 1, sp2);
  String query = "";
  const int qm = path.indexOf('?');
  if (qm >= 0) { query = path.substring(qm + 1); path = path.substring(0, qm); }

  String boundary = "";
  long contentLength = 0;
  while (client.connected()) {
    String h = client.readStringUntil('\n');
    h.trim();
    if (h.length() == 0) break;
    String lower = h;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) contentLength = h.substring(15).toInt();
    if (lower.startsWith("content-type:")) {
      const int b = lower.indexOf("boundary=");
      if (b >= 0) boundary = h.substring(b + 9);
      boundary.trim();
    }
  }

  // A form body, for everything that is not a file upload. Small by nature —
  // config fields and a remote path — so reading it here is safe. A multipart
  // upload is left on the socket for receiveImage() to stream.
  String body = "";
  if (method == "POST" && boundary.length() == 0 && contentLength > 0 &&
      contentLength < 4096) {
    const unsigned long until = millis() + 3000;
    while ((long)body.length() < contentLength && millis() < until) {
      if (client.available()) body += (char)client.read();
      else delay(1);
    }
  }

  // The OS connectivity probes. Redirecting them is what makes the phone's
  // captive sign-in sheet appear with our page in it the moment the AP is
  // joined — the whole setup flow on a device with no screen and no card.
  if (method == "GET" &&
      (path == "/generate_204" || path == "/gen_204" ||
       path == "/hotspot-detect.html" || path == "/library/test/success.html" ||
       path == "/ncsi.txt" || path == "/connecttest.txt" ||
       path == "/success.txt" || path == "/canonical.html" ||
       path == "/redirect")) {
    client.println("HTTP/1.1 302 Found");
    client.println("Location: http://192.168.4.1/");
    client.println("Connection: close");
    client.println();
    client.flush(); delay(2); client.stop();
    return;
  }

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    // The identical gzipped page the touchscreen serves.
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Content-Encoding: gzip");
    client.print("Content-Length: "); client.println(webui_gz_len);
    client.println("Cache-Control: no-cache");
    client.println("Connection: close");
    client.println();
    // Straight out of PROGMEM in chunks; the blob never sits in RAM twice.
    uint8_t buf[512];
    for (unsigned int off = 0; off < webui_gz_len; off += sizeof(buf)) {
      const unsigned int n = min((unsigned int)sizeof(buf), webui_gz_len - off);
      memcpy_P(buf, webui_gz + off, n);
      client.write(buf, n);
      yield();
    }
  }
  else if (method == "GET" && path == "/api/system/info") {
    // The flags matter as much as the numbers: this is how the page knows not
    // to offer a library, themes or a card that do not exist here.
    String j = "{";
    j += "\"firmware\":\"" DONGLE_VERSION " (" BOARD_NAME ")\",";
    j += "\"heap_free\":" + String((uint32_t)ESP.getFreeHeap()) + ",";
    j += "\"psram_free\":" + String((uint32_t)ESP.getFreePsram()) + ",";
    j += "\"sd_used_mb\":0,\"sd_total_mb\":0,";
    j += "\"game_count\":0,\"file_count\":0,";
    j += "\"loaded_game\":\"" + (g_mountLabel.length() ? g_mountLabel : String("none")) + "\",";
    j += "\"mode\":\"ADF\",\"theme\":\"AMIGA_WB2\",";
    j += "\"wifi_clients\":" + String(WiFi.softAPgetStationNum()) + ",";
    j += "\"wifi_ip\":\"" + WiFi.softAPIP().toString() + "\",";
    // The real link state. Reporting a flat false made the dashboard say
    // Offline while the WiFi check on the config page said connected.
    const bool sta = (WiFi.status() == WL_CONNECTED);
    j += "\"internet\":" + String(sta ? "true" : "false") + ",";
    j += "\"internet_ip\":\"" + (sta ? WiFi.localIP().toString() : String("")) + "\",";
    j += "\"internet_ssid\":\"" + jsonEscape(cfg_wifi_client_ssid) + "\",";
    j += "\"ftp_enabled\":false,\"dav_enabled\":true,\"log_enabled\":true,";
    // Board shape, so the page can adapt rather than guess.
    j += "\"has_sd\":false,\"has_display\":false,\"has_ota\":true,";
    j += "\"max_image_bytes\":" + String((uint32_t)MAX_IMAGE_BYTES) + ",";
    j += "\"supports_hd\":" + String(SUPPORTS_HD ? "true" : "false");
    j += "}";
    sendJson(client, 200, j);
  }
  else if (path == "/api/config") {
    if (method == "POST") {
      // Only fields that actually arrived are touched, so the page can save one
      // card without blanking the rest.
      if (body.indexOf("DAV_HOST=")  >= 0) cfg_dav_host = formValue(body, "DAV_HOST");
      if (body.indexOf("DAV_PORT=")  >= 0) cfg_dav_port = (uint16_t)formValue(body, "DAV_PORT").toInt();
      if (body.indexOf("DAV_USER=")  >= 0) cfg_dav_user = formValue(body, "DAV_USER");
      if (body.indexOf("DAV_PASS=")  >= 0) cfg_dav_pass = formValue(body, "DAV_PASS");
      if (body.indexOf("DAV_PATH=")  >= 0) cfg_dav_path = formValue(body, "DAV_PATH");
      if (body.indexOf("DAV_HTTPS=") >= 0) cfg_dav_https = (formValue(body, "DAV_HTTPS") == "1");
      if (body.indexOf("DAV_ENABLED=") >= 0) cfg_dav_enabled = (formValue(body, "DAV_ENABLED") == "1");
      if (body.indexOf("WIFI_AP_SSID=") >= 0) cfg_wifi_ssid = formValue(body, "WIFI_AP_SSID");
      if (body.indexOf("WIFI_AP_PASS=") >= 0) cfg_wifi_pass = formValue(body, "WIFI_AP_PASS");
      if (body.indexOf("WIFI_CLIENT_ENABLED=") >= 0)
        cfg_wifi_client_enabled = (formValue(body, "WIFI_CLIENT_ENABLED") == "1");
      if (body.indexOf("WIFI_CLIENT_SSID=") >= 0) cfg_wifi_client_ssid = formValue(body, "WIFI_CLIENT_SSID");
      if (body.indexOf("WIFI_CLIENT_PASS=") >= 0) cfg_wifi_client_pass = formValue(body, "WIFI_CLIENT_PASS");
      if (body.indexOf("MDNS_NAME=") >= 0) {
        String v = formValue(body, "MDNS_NAME");
        v.trim();
        v.replace(" ", "-");
        if (v.length() > 0 && v != cfg_mdns_name) {
          cfg_mdns_name = v;
          MDNS.end();
          if (MDNS.begin(cfg_mdns_name.c_str())) {
            MDNS.addService("http", "tcp", 80);
            dlog("Reachable at " + cfg_mdns_name + ".local");
          }
        }
      }
      if (body.indexOf("WIFI_TX_DBM=") >= 0) {
        int v = formValue(body, "WIFI_TX_DBM").toInt();
        cfg_wifi_tx_dbm = (v < 2) ? 2 : (v > 20 ? 20 : v);
      }
      davApplyConfig();
      saveConfig();
      // WiFi-client changes only take hold at boot. Announce the reboot in
      // the answer so the page can wait for us instead of the user guessing,
      // then schedule it for after this response has actually left.
      const bool wifiChanged = body.indexOf("WIFI_CLIENT_SSID=") >= 0 ||
                               body.indexOf("WIFI_CLIENT_PASS=") >= 0 ||
                               body.indexOf("WIFI_CLIENT_ENABLED=") >= 0;
      if (wifiChanged) g_rebootAtMs = millis() + 1500;
      sendJson(client, 200, wifiChanged
               ? "{\"status\":\"ok\",\"reboot\":true}"
               : "{\"status\":\"ok\"}");
    } else {
      String j = "{\"THEME\":\"AMIGA_WB2\",\"DISPLAY\":\"" BOARD_NAME "\",";
      j += "\"WIFI_AP_SSID\":\"" + jsonEscape(cfg_wifi_ssid) + "\",";
      j += "\"WIFI_AP_PASS\":\"" + jsonEscape(cfg_wifi_pass) + "\",";
      j += "\"WIFI_CLIENT_ENABLED\":\"" + String(cfg_wifi_client_enabled ? "1" : "0") + "\",";
      j += "\"WIFI_CLIENT_SSID\":\"" + jsonEscape(cfg_wifi_client_ssid) + "\",";
      j += "\"WIFI_CLIENT_PASS\":\"" + jsonEscape(cfg_wifi_client_pass) + "\",";
      j += "\"WIFI_TX_DBM\":\"" + String(cfg_wifi_tx_dbm) + "\",";
      j += "\"MDNS_NAME\":\"" + jsonEscape(cfg_mdns_name) + "\",";
      j += "\"DAV_ENABLED\":\"" + String(cfg_dav_enabled ? "1" : "0") + "\",";
      j += "\"DAV_HOST\":\"" + jsonEscape(cfg_dav_host) + "\",";
      j += "\"DAV_PORT\":\"" + String(cfg_dav_port) + "\",";
      j += "\"DAV_HTTPS\":\"" + String(cfg_dav_https ? "1" : "0") + "\",";
      j += "\"DAV_USER\":\"" + jsonEscape(cfg_dav_user) + "\",";
      j += "\"DAV_PASS\":\"" + jsonEscape(cfg_dav_pass) + "\",";
      j += "\"DAV_PATH\":\"" + jsonEscape(cfg_dav_path) + "\",";
      j += "\"LOG_ENABLED\":\"1\",\"SAVES\":\"OFF\"}";
      sendJson(client, 200, j);
    }
  }
  else if (method == "GET" && path == "/api/games/list") {
    // No card, so no library — but an empty list keeps the page's own logic
    // identical to the touchscreen's instead of forking it around a 404.
    sendJson(client, 200,
             "{\"mode\":\"ADF\",\"loaded_game\":\"" + g_mountLabel +
             "\",\"loaded_file\":\"" + g_mountLabel + "\",\"games\":[]}");
  }
  else if (method == "GET" && path == "/api/disk/status") {
    String j = "{\"loaded\":" + String(g_mountBytes > 0 ? "true" : "false") + ",";
    j += "\"file\":\"" + g_mountLabel + "\",\"path\":\"\",";
    j += "\"game\":\"" + g_mountLabel + "\",";
    // USB truth, live: is a host holding us configured, and when did it last
    // read a sector. A day of guessing at three different Goteks taught us
    // this is the first question, so now every port answers it.
    j += "\"usb_host\":" + String(tud_mounted() ? "true" : "false") + ",";
    j += "\"last_host_read_ms_ago\":" + String(g_lastHostReadMs ? (millis() - g_lastHostReadMs) : 0) + ",";
    j += "\"disk_num\":" + String(g_mountBytes > 0 ? 1 : 0) + ",";
    j += "\"disk_total\":" + String(g_mountBytes > 0 ? 1 : 0) + ",";
    j += "\"source\":\"" + String(g_mountBytes > 0 ? "SD" : "") + "\",";
    j += "\"name\":\"" + g_mountLabel + "\",\"np_path\":\"\",";
    j += "\"mode\":\"ADF\"}";
    sendJson(client, 200, j);
  }
  else if (method == "GET" && path == "/api/log") {
    String out = "";
    for (uint8_t i = 0; i < g_logCount; i++) {
      const uint8_t idx = (g_logHead + LOG_LINES - g_logCount + i) % LOG_LINES;
      out += g_log[idx];
      out += "\n";
    }
    sendJson(client, 200, "{\"log\":\"" + jsonEscape(out) + "\"}");
  }
  else if (method == "POST" && path == "/api/disk/unload") {
    ejectImage();
    sendJson(client, 200, "{\"status\":\"ok\"}");
  }
  else if (method == "POST" && path == "/api/system/ota") {
    if (boundary.length() == 0) {
      sendJson(client, 400, "{\"error\":\"Not a multipart upload\"}");
    } else {
      String name = "";
      const long n = receiveFirmware(client, boundary, name);
      if (n > 0) {
        sendJson(client, 200, "{\"status\":\"ok\",\"bytes\":" + String(n) + "}");
        client.flush();
        delay(250);
        client.stop();
        delay(250);
        ESP.restart();
      } else if (n == -4) {
        sendJson(client, 400, "{\"error\":\"that is not an ESP32 firmware image\"}");
      } else {
        sendJson(client, 500, "{\"error\":\"firmware update failed\"}");
      }
    }
  }
  else if (method == "POST" && path == "/api/games/upload") {
    if (boundary.length() == 0) {
      sendJson(client, 400, "{\"error\":\"Not a multipart upload\"}");
    } else {
      String name = "";
      Perf perf("Upload");
      const long n = receiveImage(client, boundary, contentLength, name);
      perf.mark("receive");
      if (n > 0) {
        // Rebuild the volume around the bytes already in place, then mount.
        build_boot_sector(&ram_disk[0]);
        build_fat(&ram_disk[FAT1_OFFSET]);
        build_fat(&ram_disk[FAT2_OFFSET]);
        g_mountFilename = name;
        build_root(&ram_disk[ROOTDIR_OFFSET]);
        mountImage(name, (uint32_t)n);
        perf.mark("mount");
        perf.bytes((uint32_t)n);
        dlog(perf.summary());
        sendJson(client, 200, "{\"name\":\"" + name + "\",\"bytes\":" + String(n) +
                              ",\"debug\":\"" + jsonEscape(perf.summary()) + "\"}");
      } else {
        tud_connect();
        const char *why =
            n == -3 ? "Image is larger than this board's volume"
          : n == -2 ? "Could not find the file in the upload"
          : n == -4 ? "Upload was empty"
                    : "Upload failed";
        dlog(String("Upload failed: ") + why);
        sendJson(client, 400, String("{\"error\":\"") + why + "\"}");
      }
    }
  }
  // ── WiFi ────────────────────────────────────────────────────────────
  //
  // Scanning is not a luxury here. Without a screen or a card there is no other
  // way to discover a network, and typing an SSID blind is how a wrong
  // character becomes twenty minutes of confusion.
  else if (method == "GET" && path == "/api/wifi/scan") {
    // A scan briefly interrupts the AP link this request arrived on, so keep it
    // short and let the page retry rather than holding the socket for seconds.
    const int n = WiFi.scanNetworks(false, true, false, 200);
    String jj = "{\"networks\":[";
    for (int i = 0; i < n && i < 30; i++) {
      if (i) jj += ",";
      jj += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\"";
      jj += ",\"rssi\":" + String(WiFi.RSSI(i));
      jj += ",\"encrypted\":" +
            String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
      yield();
    }
    jj += "]}";
    WiFi.scanDelete();
    sendJson(client, 200, jj);
  }
  else if (method == "GET" && path == "/api/wifi/status") {
    String jj = "{\"ap_active\":true";
    jj += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
    jj += ",\"ap_clients\":" + String(WiFi.softAPgetStationNum());
    const bool sta = (WiFi.status() == WL_CONNECTED);
    jj += ",\"sta_connected\":" + String(sta ? "true" : "false");
    jj += ",\"sta_ip\":\"" + (sta ? WiFi.localIP().toString() : String("")) + "\"";
    jj += ",\"sta_ssid\":\"" + jsonEscape(cfg_wifi_client_ssid) + "\"}";
    sendJson(client, 200, jj);
  }
  // ── WebDAV ──────────────────────────────────────────────────────────
  //
  // The same client the touchscreen uses. What is missing here is the caching:
  // with no card there is nowhere to keep listings, covers or notes, so every
  // browse is a live PROPFIND and covers simply are not offered. The page
  // handles both absences already — it shows "No Art" and moves on.
  else if (method == "GET" && path == "/api/dav/status") {
    // Field for field what the page reads. Guessing this shape is why the tab
    // reported "not configured" no matter what was set: it looks at .enabled,
    // .host, .wifi_connected and .now_playing, and got none of them.
    String jj = "{\"enabled\":" + String(cfg_dav_enabled ? "true" : "false");
    jj += ",\"host\":\"" + jsonEscape(cfg_dav_host) + "\"";
    jj += ",\"port\":" + String(cfg_dav_port);
    jj += ",\"user\":\"" + jsonEscape(cfg_dav_user) + "\"";
    jj += ",\"path\":\"" + jsonEscape(cfg_dav_path) + "\"";
    jj += ",\"https\":" + String(cfg_dav_https ? "true" : "false");
    jj += ",\"connected\":" + String(davClient.isConnected() ? "true" : "false");
    jj += ",\"wifi_connected\":" +
          String((WiFi.status() == WL_CONNECTED) ? "true" : "false");
    // No card, so no cached listing to browse before connecting.
    jj += ",\"has_cache\":false";
    const String err = davClient.lastError();
    if (err.length() > 0) jj += ",\"error\":\"" + jsonEscape(err) + "\"";
    if (g_mountLabel.length() > 0) {
      jj += ",\"now_playing\":{\"source\":\"" +
            String(g_davLoadedPath.length() ? "dav" : "sd") + "\"";
      jj += ",\"name\":\"" + jsonEscape(g_mountLabel) + "\"";
      jj += ",\"path\":\"" + jsonEscape(g_davLoadedPath) + "\"}";
    }
    jj += "}";
    sendJson(client, 200, jj);
  }
  else if (method == "POST" && path == "/api/dav/connect") {
    if (!cfg_dav_enabled || cfg_dav_host.length() == 0) {
      sendJson(client, 400, "{\"error\":\"WebDAV is not configured yet\"}");
    } else if (WiFi.status() != WL_CONNECTED) {
      sendJson(client, 503, "{\"error\":\"Not on a network - set the WiFi client under Config\"}");
    } else {
      davConnected = davClient.connect();
      if (davConnected) sendJson(client, 200, "{\"status\":\"ok\"}");
      else sendJson(client, 502, "{\"error\":\"" + jsonEscape(davClient.lastError()) + "\"}");
    }
  }
  else if (method == "POST" && path == "/api/dav/disconnect") {
    davConnected = false;
    sendJson(client, 200, "{\"status\":\"ok\"}");
  }
  else if (method == "GET" && path == "/api/dav/list") {
    String want = queryValue(query, "path");
    if (want.length() == 0) want = "/";
    if (WiFi.status() != WL_CONNECTED) {
      sendJson(client, 503, "{\"error\":\"Not on a network\"}");
    } else {
      if (listCacheHit(want)) {
        dlog("DAV list: served from memory (" + want + ")");
        sendJson(client, 200, g_listCacheJson);
        client.flush(); delay(2); client.stop();
        return;
      }
      Perf perf("DAV list");
      DAVEntryList entries;
      if (!davClient.listDir(want, entries)) {
        sendJson(client, 502, "{\"error\":\"" + jsonEscape(davClient.lastError()) + "\"}");
      } else {
        String jj = "{\"path\":\"" + jsonEscape(want) + "\",\"entries\":[";
        bool first = true;
        for (size_t i = 0; i < entries.size(); i++) {
          if (!entries[i].isDir && (entries[i].hasCover || entries[i].hasNfo)) continue;
          if (!first) jj += ",";
          first = false;
          jj += "{\"name\":\"" + jsonEscape(entries[i].name()) + "\"";
          jj += ",\"dir\":" + String(entries[i].isDir ? "true" : "false");
          jj += ",\"size\":" + String(entries[i].size) + "}";
          yield();
        }
        perf.mark("propfind+parse");
        const String timing = perf.summary() + ", " + String(entries.size()) + " entries";
        dlog(timing);
        jj += "],\"debug\":\"" + jsonEscape(timing) + "\"}";
        if (jj.length() <= DAV_LIST_CACHE_MAX) {
          g_listCachePath = want;
          g_listCacheJson = jj;
          g_listCacheAt   = millis();
        } else {
          g_listCachePath = "";
          g_listCacheJson = "";
          dlog("DAV list: " + String(jj.length()) + " B is too big to cache");
        }
        sendJson(client, 200, jj);
      }
    }
  }
  else if (method == "GET" && path == "/api/dav/rowmeta") {
    // Per-folder facts come from the on-SD cache on a touchscreen. No card, no
    // cache: an empty answer, which the page reads as "unknown" and shows
    // nothing for, rather than guessing wrong.
    sendJson(client, 200, "{\"meta\":[],\"capped\":false}");
  }
  else if (method == "GET" && path == "/api/dav/cover") {
    const String want = queryValue(query, "path");
    if (g_coverLen > 0 && want == g_coverPath) {
      sendHeader(client, 200, "image/jpeg", (int)g_coverLen);
      for (size_t off = 0; off < g_coverLen; off += 512) {
        const size_t n = (g_coverLen - off < 512) ? (g_coverLen - off) : 512;
        client.write(g_coverBuf + off, n);
        yield();
      }
    } else {
      // Only the mounted game's art is kept; everything else would need a
      // fetch per row over TLS, with nowhere to put the result.
      sendJson(client, 404, "{\"error\":\"Only the mounted game cover is cached\"}");
    }
  }
  else if (method == "GET" && path == "/api/dav/nfo") {
    const String want = queryValue(query, "path");
    if (g_nfoText.length() > 0 && want == g_nfoPath) {
      sendJson(client, 200, "{\"nfo\":\"" + jsonEscape(g_nfoText) + "\"}");
      client.flush(); delay(2); client.stop();
      return;
    }
    static uint8_t nfoBuf[2048];
    const long n = davClient.streamToBuffer(want, nfoBuf, sizeof(nfoBuf) - 1);
    if (n <= 0) {
      sendJson(client, 404, "{\"error\":\"No notes there\"}");
    } else {
      nfoBuf[n] = 0;
      sendJson(client, 200, "{\"nfo\":\"" + jsonEscape(String((char *)nfoBuf)) + "\"}");
    }
  }
  else if (method == "POST" && path == "/api/dav/load") {
    // Queue it; the load happens in loop(). Answering only when the transfer
    // finished meant the device served nothing else for the whole seven
    // seconds an HD image takes — including /api/log, so the one tool for
    // finding out what went wrong was unreachable exactly when it was needed.
    // The touchscreen has always deferred this, and the page is built for it.
    const String remote = formValue(body, "path");
    if (remote.length() == 0) {
      sendJson(client, 400, "{\"error\":\"No path given\"}");
    } else if (WiFi.status() != WL_CONNECTED) {
      sendJson(client, 503, "{\"error\":\"Not on a network\"}");
    } else if (g_pendingDavPath.length() > 0) {
      sendJson(client, 409, "{\"error\":\"Already loading something\"}");
    } else {
      String name = remote;
      const int sl = name.lastIndexOf('/');
      if (sl >= 0) name = name.substring(sl + 1);
      g_pendingDavPath = remote;
      dlog("DAV load queued: " + name);
      sendJson(client, 200, "{\"status\":\"ok\",\"name\":\"" + jsonEscape(name) +
                            "\",\"file\":\"" + jsonEscape(remote) + "\"}");
    }
  }
  else {
    sendJson(client, 404, "{\"error\":\"Not available on this device\"}");
  }

  client.flush();
  delay(2);
  client.stop();
}
