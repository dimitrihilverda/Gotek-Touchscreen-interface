// Fake WiFiClient that replays a canned byte stream, so the WebDAV streaming
// parser can be driven deterministically on the host.
//
// The important knob is `chunkSize`: it caps how many bytes a single
// readBytes()/available() call will yield. Setting it to 1 forces the parser
// to reassemble every XML tag across reads, which is exactly the condition a
// real TCP socket produces under load and the one most likely to break a
// naive state machine.
#pragma once
#include "Arduino.h"
#include <string>
#include <algorithm>

#define WL_CONNECTED 3

class WiFiClient {
public:
  std::string data;      // bytes still to deliver
  size_t pos = 0;
  size_t chunkSize = 512;   // max bytes handed over per read call
  bool   open = true;

  WiFiClient() {}
  virtual ~WiFiClient() {}

  void feed(const std::string &d) { data = d; pos = 0; open = true; }

  virtual bool connect(const char *, uint16_t) { return true; }
  // Overload with an explicit connect timeout, matching arduino-esp32.
  virtual bool connect(const char *, uint16_t, int32_t) { return true; }
  virtual void setTimeout(unsigned long) {}
  void stop() { open = false; }

  // Mirrors Arduino semantics: connected() stays true while unread bytes
  // remain, then goes false once the peer has closed.
  bool connected() { return open && pos < data.size(); }
  int  available() {
    if (pos >= data.size()) return 0;
    return (int)std::min(chunkSize, data.size() - pos);
  }
  int read() { return pos < data.size() ? (unsigned char)data[pos++] : -1; }
  size_t read(uint8_t *dst, size_t len) { return readBytes((char *)dst, len); }
  size_t readBytes(char *dst, size_t len) {
    size_t n = std::min({ len, chunkSize, data.size() - pos });
    memcpy(dst, data.data() + pos, n);
    pos += n;
    return n;
  }
  String readStringUntil(char term) {
    std::string out;
    while (pos < data.size()) {
      char c = data[pos++];
      if (c == term) break;
      out += c;
    }
    return String(out);
  }
  // Request side is discarded — tests only exercise response parsing.
  void println(const String &) {}
  void println() {}
  void print(const String &) {}
};

class WiFiClientSecure : public WiFiClient {
public:
  void setInsecure() {}
};

// Modem power-save levels, as the ESP32 core names them. The host cares only
// that the guard in webdav_client.h compiles and balances.
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;

struct _WiFiStub {
  int status() { return WL_CONNECTED; }
  wifi_ps_type_t _ps = WIFI_PS_NONE;
  wifi_ps_type_t getSleep() { return _ps; }
  void setSleep(wifi_ps_type_t p) { _ps = p; }
};
static _WiFiStub WiFi;
