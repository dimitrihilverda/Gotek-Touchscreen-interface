#pragma once
/*
  shared/http_utils.h
  ===================
  HTTP helpers shared by all Gotek device targets.
  Provides: JSON escaping, response senders, URL decode,
  form-value parser, query-string parser, and the HTTP
  request parser.

  Include ONCE from the device .ino before any API headers.
*/

#include <WiFi.h>

// ─────────────────────────────────────────────────────────────────────────────
// JSON escape
// ─────────────────────────────────────────────────────────────────────────────

inline String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 10);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else                out += c;
  }
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Response senders
// ─────────────────────────────────────────────────────────────────────────────

inline void sendResponse(WiFiClient &client, int code,
                         const String &contentType, const String &body) {
  client.println("HTTP/1.1 " + String(code) + " OK");
  client.println("Content-Type: " + contentType);
  client.println("Content-Length: " + String(body.length()));
  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Methods: GET,POST,DELETE,OPTIONS");
  client.println("Access-Control-Allow-Headers: Content-Type,X-Filename");
  client.println("Connection: close");
  client.println();
  client.print(body);
}

inline void sendJSON(WiFiClient &client, int code, const String &json) {
  sendResponse(client, code, "application/json", json);
}

inline void sendGzipResponse(WiFiClient &client, const String &contentType,
                              const uint8_t *data, size_t len) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: " + contentType);
  client.println("Content-Encoding: gzip");
  client.println("Content-Length: " + String(len));
  client.println("Connection: close");
  client.println();
  size_t sent = 0;
  while (sent < len) {
    size_t chunk = len - sent;
    if (chunk > 2048) chunk = 2048;
    client.write(&data[sent], chunk);
    sent += chunk;
    yield();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// URL decode
// ─────────────────────────────────────────────────────────────────────────────

inline String urlDecode(const String &in) {
  String out;
  out.reserve(in.length());
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.length()) {
      char hi = in[i + 1], lo = in[i + 2];
      int h = (hi >= '0' && hi <= '9') ? hi - '0'
            : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
            : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
      int l = (lo >= '0' && lo <= '9') ? lo - '0'
            : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
            : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
      if (h >= 0 && l >= 0) { out += (char)((h << 4) | l); i += 2; }
      else out += c;
    } else {
      out += c;
    }
  }
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Form value / query string helpers
// ─────────────────────────────────────────────────────────────────────────────

inline String getFormValue(const String &body, const String &key) {
  String search = key + "=";
  int start = body.indexOf(search);
  if (start < 0) return "";
  if (start > 0 && body[start - 1] != '&') {
    search = "&" + key + "=";
    start = body.indexOf(search);
    if (start < 0) return "";
    start += 1;
  }
  start += key.length() + 1;
  int end = body.indexOf('&', start);
  return urlDecode((end < 0) ? body.substring(start) : body.substring(start, end));
}

inline String getQueryParam(const String &query, const String &key) {
  String search = key + "=";
  int start = query.indexOf(search);
  if (start < 0) return "";
  if (start > 0 && query[start - 1] != '&') {
    search = "&" + key + "=";
    start = query.indexOf(search);
    if (start < 0) return "";
    start += 1;
  }
  start += key.length() + 1;
  int end = query.indexOf('&', start);
  return urlDecode((end < 0) ? query.substring(start) : query.substring(start, end));
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP request struct + parser
// ─────────────────────────────────────────────────────────────────────────────

struct HttpRequest {
  String method, path, query, body;
  int    contentLength;
  String filename;
  String contentType;
};

inline bool parseRequest(WiFiClient &client, HttpRequest &req) {
  req.method = req.path = req.query = req.body = req.filename = req.contentType = "";
  req.contentLength = 0;

  String line = client.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return false;

  int sp1 = line.indexOf(' ');
  int sp2 = line.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) return false;

  req.method   = line.substring(0, sp1);
  String full  = line.substring(sp1 + 1, sp2);
  int qIdx     = full.indexOf('?');
  if (qIdx >= 0) { req.path = full.substring(0, qIdx); req.query = full.substring(qIdx + 1); }
  else           { req.path = full; }

  while (client.connected()) {
    String hdr = client.readStringUntil('\n');
    hdr.trim();
    if (hdr.length() == 0) break;
    String lower = hdr; lower.toLowerCase();
    if      (lower.startsWith("content-length:")) { req.contentLength = hdr.substring(15).toInt(); }
    else if (lower.startsWith("x-filename:"))     { req.filename = hdr.substring(11); req.filename.trim(); }
    else if (lower.startsWith("content-type:"))   { req.contentType = hdr.substring(13); req.contentType.trim(); }
  }

  if (req.method == "POST" && req.contentLength > 0 && req.contentLength < 4096 &&
      req.contentType.indexOf("octet-stream") < 0) {
    unsigned long t = millis();
    while (client.available() < req.contentLength && millis() - t < 3000) { yield(); delay(1); }
    req.body = client.readString();
  }
  return true;
}
