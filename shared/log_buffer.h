#pragma once
/*
  shared/log_buffer.h
  ===================
  Logging with SPIFFS persistence (for dongle) + in-memory ring buffer.
  Accessible via GET /api/log.

  On devices without SD card (DEVICE_WIFI_DONGLE), logs are written to
  SPIFFS flash file /log.txt so they survive reboots. The ring buffer
  is also kept in RAM for fast access from the web UI.

  Usage:
    logAppend("some message");
*/

#include <SPIFFS.h>

#define LOG_BUF_MAX 4096
#define LOG_FILE    "/log.txt"
#define LOG_FILE_MAX 16384   // max log file size before truncation

static char log_buf[LOG_BUF_MAX];
static int  log_buf_len     = 0;
static bool log_buf_wrapped = false;

// cfg_log_enabled must be declared by the device .ino before including this header.

// Write a single line to SPIFFS log file (append mode)
inline void _logToFlash(const String &entry) {
#ifdef DEVICE_WIFI_DONGLE
  File f = SPIFFS.open(LOG_FILE, FILE_APPEND);
  if (f) {
    f.print(entry);
    f.close();
    // Truncate if file too large — keep last half
    File check = SPIFFS.open(LOG_FILE, FILE_READ);
    if (check) {
      size_t sz = check.size();
      check.close();
      if (sz > LOG_FILE_MAX) {
        File rd = SPIFFS.open(LOG_FILE, FILE_READ);
        if (rd) {
          rd.seek(sz - (LOG_FILE_MAX / 2));
          String tail = rd.readString();
          rd.close();
          // Find first newline to start on a clean line
          int nl = tail.indexOf('\n');
          if (nl >= 0) tail = tail.substring(nl + 1);
          File wr = SPIFFS.open(LOG_FILE, FILE_WRITE);
          if (wr) {
            wr.print("[log truncated]\n");
            wr.print(tail);
            wr.close();
          }
        }
      }
    }
  }
#endif
}

// Load log from SPIFFS into ring buffer at startup
inline void logLoadFromFlash() {
#ifdef DEVICE_WIFI_DONGLE
  File f = SPIFFS.open(LOG_FILE, FILE_READ);
  if (!f) return;
  size_t sz = f.size();
  if (sz == 0) { f.close(); return; }
  // Read last LOG_BUF_MAX-1 bytes into ring buffer
  if (sz > LOG_BUF_MAX - 1) {
    f.seek(sz - (LOG_BUF_MAX - 1));
  }
  log_buf_len = f.readBytes(log_buf, LOG_BUF_MAX - 1);
  log_buf[log_buf_len] = 0;
  f.close();
  // Start on a clean line
  if (sz > LOG_BUF_MAX - 1) {
    char *nl = strchr(log_buf, '\n');
    if (nl) {
      int skip = (nl - log_buf) + 1;
      memmove(log_buf, log_buf + skip, log_buf_len - skip);
      log_buf_len -= skip;
      log_buf[log_buf_len] = 0;
    }
    log_buf_wrapped = true;
  }
#endif
}

inline void logAppend(const String &line) {
  if (!cfg_log_enabled) return;
  String entry = line + "\n";
  int n = (int)entry.length();
  if (n >= LOG_BUF_MAX) return;

  // Write to SPIFFS flash (persistent)
  _logToFlash(entry);

  // Write to RAM ring buffer (fast access)
  if (log_buf_len + n < LOG_BUF_MAX) {
    memcpy(log_buf + log_buf_len, entry.c_str(), n);
    log_buf_len += n;
  } else {
    int excess = (log_buf_len + n) - LOG_BUF_MAX + 1;
    memmove(log_buf, log_buf + excess, log_buf_len - excess);
    log_buf_len -= excess;
    log_buf_wrapped = true;
    memcpy(log_buf + log_buf_len, entry.c_str(), n);
    log_buf_len += n;
  }
  log_buf[log_buf_len] = 0;
  Serial.println(line);
}

inline void logClear() {
  log_buf_len = 0;
  log_buf[0]  = 0;
  log_buf_wrapped = false;
#ifdef DEVICE_WIFI_DONGLE
  SPIFFS.remove(LOG_FILE);
#endif
}

// Returns the current log buffer contents as a String
inline String logGetContents() {
  return String(log_buf);
}
