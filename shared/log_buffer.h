#pragma once
/*
  shared/log_buffer.h
  ===================
  In-memory ring buffer for web-accessible log output.
  Accessible via GET /api/log.

  Usage:
    logAppend("some message");   // replaces Serial.println where capture needed
*/

#define LOG_BUF_MAX 4096

static char log_buf[LOG_BUF_MAX];
static int  log_buf_len     = 0;
static bool log_buf_wrapped = false;

// cfg_log_enabled must be declared by the device .ino before including this header.
// Declaration: extern bool cfg_log_enabled;

inline void logAppend(const String &line) {
  if (!cfg_log_enabled) return;
  String entry = line + "\n";
  int n = (int)entry.length();
  if (n >= LOG_BUF_MAX) return;
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
}

// Returns the current log buffer contents as a String
inline String logGetContents() {
  return String(log_buf);
}
