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
#include "../Gotek_Touchscreen/webui.h"

// ── Tiny HTTP plumbing ───────────────────────────────────────────────────

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
  const String path   = reqLine.substring(sp1 + 1, sp2);

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
    j += "\"internet\":false,";
    j += "\"ftp_enabled\":false,\"dav_enabled\":false,\"log_enabled\":true,";
    // Board shape, so the page can adapt rather than guess.
    j += "\"has_sd\":false,\"has_display\":false,";
    j += "\"max_image_bytes\":" + String((uint32_t)MAX_IMAGE_BYTES) + ",";
    j += "\"supports_hd\":" + String(SUPPORTS_HD ? "true" : "false");
    j += "}";
    sendJson(client, 200, j);
  }
  else if (method == "GET" && path == "/api/config") {
    // No CONFIG.TXT without a card; these are the running values.
    String j = "{\"THEME\":\"AMIGA_WB2\",\"DISPLAY\":\"" BOARD_NAME "\",";
    j += "\"WIFI_AP_SSID\":\"" + String(AP_SSID) + "\",";
    j += "\"WIFI_TX_DBM\":\"" + String(WIFI_TX_DBM) + "\",";
    j += "\"LOG_ENABLED\":\"1\",\"SAVES\":\"OFF\"}";
    sendJson(client, 200, j);
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
  else if (method == "POST" && path == "/api/games/upload") {
    if (boundary.length() == 0) {
      sendJson(client, 400, "{\"error\":\"Not a multipart upload\"}");
    } else {
      String name = "";
      const long n = receiveImage(client, boundary, contentLength, name);
      if (n > 0) {
        // Rebuild the volume around the bytes already in place, then mount.
        build_boot_sector(&ram_disk[0]);
        build_fat(&ram_disk[FAT1_OFFSET]);
        build_fat(&ram_disk[FAT2_OFFSET]);
        g_mountFilename = name;
        build_root(&ram_disk[ROOTDIR_OFFSET]);
        mountImage(name, (uint32_t)n);
        sendJson(client, 200, "{\"name\":\"" + name + "\",\"bytes\":" + String(n) + "}");
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
  else {
    sendJson(client, 404, "{\"error\":\"Not available on this device\"}");
  }

  client.flush();
  delay(2);
  client.stop();
}
