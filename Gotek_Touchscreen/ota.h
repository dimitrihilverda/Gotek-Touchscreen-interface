#pragma once
//
// Firmware update over the web interface.
//
// The reason this exists: getting a new build onto a board meant a USB cable,
// the right COM port, and often the BOOT+RST dance — and on a machine where
// something quietly claims the serial port the moment the firmware enumerates,
// that turns a thirty-second job into a reboot of the laptop. The device
// already has WiFi, a web server and two OTA slots in its partition table.
// It can flash itself.
//
// The image is streamed straight into the inactive slot as it arrives; there
// is no room to buffer 2.5 MB anywhere else. On success the running slot is
// switched and the board restarts into the new firmware. On any failure the
// slot is abandoned and the board keeps running exactly what it was running,
// which is the whole point of having two.

#include <Update.h>

// An ESP32 application image starts with the magic byte 0xE9. Checking it is
// the difference between "wrong file, nothing happened" and a board that
// reboots into garbage and needs the very cable this feature exists to avoid.
#define OTA_IMAGE_MAGIC 0xE9

bool handleOTAUpload(WiFiClient &client, const HttpRequest &req) {
  if (req.boundary.length() == 0 || req.contentLength <= 0) {
    sendJSON(client, 400, "{\"error\":\"expected a multipart firmware upload\"}");
    return true;
  }

  const String delim = "--" + req.boundary;

  // Part headers. All we want is the filename, for the log.
  String filename;
  bool inData = false;
  unsigned long deadline = millis() + 10000;
  while (client.connected() && !inData && millis() < deadline) {
    if (!client.available()) { delay(2); continue; }
    String line = client.readStringUntil('\n');
    line.trim();
    const int fn = line.indexOf("filename=\"");
    if (fn >= 0) {
      const int end = line.indexOf('"', fn + 10);
      filename = line.substring(fn + 10, end);
    }
    if (line.length() == 0 && filename.length() > 0) inData = true;
  }
  if (!inData) {
    sendJSON(client, 400, "{\"error\":\"no file in the upload\"}");
    return true;
  }

  sdLog("OTA: receiving " + filename);

#if HAS_DISPLAY
  // Say what is happening on the panel, and drop the backlight for the same
  // reason every other heavy operation does: sustained WiFi RX plus a flash
  // erase is not the moment to also be lighting an LCD at full brightness.
  BacklightDip _dip("UPDATING FIRMWARE...");
#endif

  if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    const String err = Update.errorString();
    sdLog("OTA: cannot start: " + err);
    Update.abort();
    sendJSON(client, 500, "{\"error\":\"" + jsonEscape(err) + "\"}");
    return true;
  }

  MultipartBody body;
  if (!body.begin(delim.c_str(), (int)delim.length())) {
    Update.abort();
    sendJSON(client, 400, "{\"error\":\"boundary too long\"}");
    return true;
  }

  size_t written = 0;
  bool   failed  = false;
  bool   badMagic = false;
  String writeErr;

  uint8_t rd[1024];
  unsigned long lastByteAt = millis();
  const unsigned long STALL_MS = 15000;   // a flash erase can be slow
  bool done = false;

  while (client.connected() && !done) {
    const int avail = client.available();
    if (avail <= 0) {
      if (millis() - lastByteAt > STALL_MS) {
        sdLog("OTA: upload stalled after " + String(written) + " bytes");
        failed = true;
        break;
      }
      delay(2);
      yield();
      continue;
    }

    const int n = client.readBytes(rd, min((int)sizeof(rd), avail));
    if (n <= 0) continue;
    lastByteAt = millis();

    const bool hitBoundary = body.feed(rd, n, [&](const uint8_t *p, int len) {
      if (failed || badMagic) return;
      if (written == 0 && len > 0 && p[0] != OTA_IMAGE_MAGIC) {
        badMagic = true;
        return;
      }
      if (Update.write((uint8_t *)p, len) != (size_t)len) {
        failed = true;
        writeErr = Update.errorString();
        return;
      }
      written += len;
    });

    if (badMagic || failed) break;
    if (hitBoundary) { done = true; break; }
    yield();
  }

  if (badMagic) {
    Update.abort();
    sdLog("OTA: refused, " + filename + " is not an ESP32 firmware image");
    sendJSON(client, 400,
             "{\"error\":\"that is not an ESP32 firmware image\"}");
    return true;
  }

  if (failed || written == 0) {
    Update.abort();
    const String err = writeErr.length() ? writeErr : String("upload did not complete");
    sdLog("OTA: FAILED after " + String(written) + " bytes: " + err);
    sendJSON(client, 500, "{\"error\":\"" + jsonEscape(err) + "\"}");
    return true;
  }

  if (!Update.end(true) || !Update.isFinished()) {
    const String err = Update.errorString();
    sdLog("OTA: could not finalise: " + err);
    sendJSON(client, 500, "{\"error\":\"" + jsonEscape(err) + "\"}");
    return true;
  }

  sdLog("OTA: wrote " + String(written) + " bytes, restarting into it");

  // Answer BEFORE restarting, and make sure it has actually gone out — a
  // browser left waiting on a socket that dies mid-reply shows a network
  // error, which looks like a failed update when it in fact succeeded.
  sendJSON(client, 200,
           "{\"status\":\"ok\",\"bytes\":" + String(written) +
           ",\"file\":\"" + jsonEscape(filename) + "\"}");
  client.flush();
  delay(250);
  client.stop();
  delay(250);

  ESP.restart();
  return true;   // never reached
}
