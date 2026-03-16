#ifndef WEBDAV_CLIENT_H
#define WEBDAV_CLIENT_H

/*
  Gotek — Lightweight WebDAV Client (shared across devices)
  Uses WiFiClientSecure for HTTPS WebDAV (e.g. Stackstorage, Nextcloud, etc.)
  PROPFIND for directory listing, GET for file download.
  No external library dependencies.

  Performance features:
    - TLS connection reuse (persistent connection across multiple requests)
    - Chunked body reading via buffer (not char-by-char)
    - XML entity decoding for PROPFIND responses

  Device guards:
    DEVICE_HAS_SD_STORAGE  — enables downloadFile() (SD_MMC)
    DEVICE_HAS_SDLOG       — routes _log() to sdLog() on SD card
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

// WebDAV config variables are defined in the main .ino file:
// cfg_dav_enabled, cfg_dav_host, cfg_dav_port, cfg_dav_user, cfg_dav_pass, cfg_dav_path, cfg_dav_https

// ============================================================================
// WebDAV Types
// ============================================================================

struct DAVFileEntry {
  String name;
  bool   isDir;
  size_t size;
  bool   hasCover;        // true if folder contains a .jpg/.png
  bool   hasNfo;          // true if folder contains a .nfo
  String coverFile;       // name only — legacy, kept for compat
  String nfoFile;         // name only — legacy, kept for compat
  String href;            // full DAV path from PROPFIND href (URL-decoded)

  // Full DAV paths — populated by background indexer, empty until indexed
  String coverPath;
  String nfoPath;
  std::vector<String> diskPaths;
  int    diskCount = 0;
  bool   indexed = false;
};

// ============================================================================
// WebDAV Client Class
// ============================================================================

class GotekDAV {
public:
  GotekDAV() : _connected(false), _lastError(""), _debugLog(""),
               _httpStatus(0), _tcp(nullptr), _secure(nullptr),
               _persistHost(""), _persistPort(0), _persistHttps(false) {}

  ~GotekDAV() { _closeConnection(); }

  String lastError() { return _lastError; }
  int lastHttpStatus() { return _httpStatus; }
  String lastDebug() { return _debugLog; }
  bool isConnected() { return _connected; }

  // Connect to WebDAV server (validate connectivity)
  bool connect() {
    _lastError = "";
    if (cfg_dav_host.length() == 0) {
      _lastError = "No WebDAV host configured";
      _log("DAV connect: " + _lastError);
      return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
      _lastError = "WiFi not connected (status=" + String(WiFi.status()) + ")";
      _log("DAV connect: " + _lastError);
      return false;
    }

    // Sanitize host
    if (cfg_dav_host.startsWith("https://")) cfg_dav_host = cfg_dav_host.substring(8);
    if (cfg_dav_host.startsWith("http://"))  cfg_dav_host = cfg_dav_host.substring(7);
    int slashPos = cfg_dav_host.indexOf('/');
    if (slashPos > 0) cfg_dav_host = cfg_dav_host.substring(0, slashPos);
    cfg_dav_host.trim();

    _log("DAV: testing connection to " + String(cfg_dav_https ? "https://" : "http://") +
         cfg_dav_host + ":" + String(cfg_dav_port));

    std::vector<DAVFileEntry> test;
    if (listDir("/", test)) {
      _connected = true;
      _log("DAV: connected OK (" + String(test.size()) + " entries in root)");
      return true;
    }
    return false;
  }

  void disconnect() {
    _connected = false;
    _closeConnection();
    _log("DAV: disconnected");
  }

  // List directory contents via PROPFIND
  bool listDir(const String &path, std::vector<DAVFileEntry> &entries) {
    entries.clear();
    _lastError = "";

    // Build full path
    String fullPath;
    String base = cfg_dav_path;
    if (!base.endsWith("/")) base += "/";
    if (path.length() == 0 || path == "/") {
      fullPath = base;
    } else if (path.startsWith(base) || path.startsWith(cfg_dav_path)) {
      fullPath = path;
      if (!fullPath.endsWith("/")) fullPath += "/";
    } else {
      fullPath = base;
      if (path.startsWith("/")) fullPath += path.substring(1);
      else fullPath += path;
      if (!fullPath.endsWith("/")) fullPath += "/";
    }

    String encodedPath = _urlEncodePath(fullPath);
    _log("DAV: PROPFIND " + encodedPath);

    // Ensure we have a TCP connection (reuse if possible)
    if (!_ensureConnection()) return false;

    // Build PROPFIND request
    String auth = _basicAuth(cfg_dav_user, cfg_dav_pass);
    String body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                  "<D:propfind xmlns:D=\"DAV:\">"
                  "<D:prop><D:resourcetype/><D:getcontentlength/><D:displayname/></D:prop>"
                  "</D:propfind>";

    esp_task_wdt_reset();  // feed WDT before sending request
    _tcp->println("PROPFIND " + encodedPath + " HTTP/1.1");
    _tcp->println("Host: " + cfg_dav_host);
    _tcp->println("Authorization: Basic " + auth);
    _tcp->println("Depth: 1");
    _tcp->println("Content-Type: application/xml");
    _tcp->println("Content-Length: " + String(body.length()));
    _tcp->println("Connection: keep-alive");
    _tcp->println();
    _tcp->print(body);

    esp_task_wdt_reset();  // feed WDT before waiting for response
    // Read response — body ends up in PSRAM buffer (_psramBody)
    _readHTTPBody();
    esp_task_wdt_reset();  // feed WDT after reading response

    if (!_psramBody || _psramBodyLen == 0) {
      _lastError = "Empty response from server";
      _log("DAV: " + _lastError);
      _closeConnection();
      _freePsramBody();
      return false;
    }

    if (_httpStatus >= 400) {
      char excerpt[128];
      int n = _psramBodyLen > 120 ? 120 : _psramBodyLen;
      memcpy(excerpt, _psramBody, n);
      excerpt[n] = '\0';
      _lastError = "HTTP " + String(_httpStatus) + ": " + String(excerpt);
      _freePsramBody();
      return false;
    }

    // Parse XML directly from PSRAM buffer (no heap copy needed!)
    _parsePropfindResponseRaw(_psramBody, _psramBodyLen, fullPath, entries);
    _freePsramBody();
    _log("DAV: listed " + String(entries.size()) + " entries in " + fullPath);
    _connected = true;
    return true;
  }

  // ── SD card download — only available on devices with SD storage ──────────
#ifdef DEVICE_HAS_SD_STORAGE

  long downloadFile(const String &remotePath, const String &localPath) {
    _lastError = "";

    int lastSlash = localPath.lastIndexOf('/');
    if (lastSlash > 0) {
      String parentDir = localPath.substring(0, lastSlash);
      SD_MMC.mkdir(parentDir.c_str());
    }

    String fullRemote = _buildFullPath(remotePath);
    String encodedPath = _urlEncodePath(fullRemote);
    _log("DAV: GET " + encodedPath);

    if (!_ensureConnection()) return -1;

    String auth = _basicAuth(cfg_dav_user, cfg_dav_pass);
    _tcp->println("GET " + encodedPath + " HTTP/1.1");
    _tcp->println("Host: " + cfg_dav_host);
    _tcp->println("Authorization: Basic " + auth);
    _tcp->println("Connection: keep-alive");
    _tcp->println();

    // Skip HTTP headers
    long contentLength = -1;
    unsigned long timeout = millis();
    while (_tcp->connected() && millis() - timeout < 15000) {
      if (!_tcp->available()) { delay(1); continue; }
      String line = _tcp->readStringUntil('\n');
      line.trim();
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) {
          int code = line.substring(sp + 1, sp + 4).toInt();
          if (code >= 400) { _lastError = "HTTP " + String(code); return -1; }
        }
      }
      if (line.startsWith("Content-Length:") || line.startsWith("content-length:"))
        contentLength = line.substring(line.indexOf(':') + 1).toInt();
      if (line.length() == 0) break;
      timeout = millis();
    }

    File outFile = SD_MMC.open(localPath.c_str(), "w");
    if (!outFile) { _lastError = "Cannot create local file"; return -1; }

    long totalBytes = 0;
    uint8_t buf[4096];
    timeout = millis();
    while (_tcp->connected() || _tcp->available()) {
      if (millis() - timeout > 30000) { _lastError = "Download timeout"; break; }
      size_t avail = _tcp->available();
      if (avail == 0) { delay(5); continue; }
      size_t toRead = (avail > sizeof(buf)) ? sizeof(buf) : avail;
      size_t bytesRead = _tcp->read(buf, toRead);
      if (bytesRead > 0) {
        outFile.write(buf, bytesRead);
        totalBytes += bytesRead;
        timeout = millis();
        if (contentLength > 0 && totalBytes >= contentLength) break;
      }
    }

    outFile.close();

    if (totalBytes == 0) {
      SD_MMC.remove(localPath.c_str());
      if (_lastError.length() == 0) _lastError = "Zero bytes received";
      return -1;
    }

    _log("DAV: downloaded " + fullRemote + " -> " + localPath + " (" + String(totalBytes) + " bytes)");
    return totalBytes;
  }

#endif // DEVICE_HAS_SD_STORAGE

  // ── Stream to memory buffer — works on all devices ────────────────────────

  long streamToBuffer(const String &remotePath, uint8_t *buf, size_t bufSize) {
    _lastError = "";

    String fullRemote = _buildFullPath(remotePath);
    String encodedPath = _urlEncodePath(fullRemote);
    _log("DAV: GET->RAM " + encodedPath + " bufSize=" + String(bufSize));

    if (!_ensureConnection()) return -1;

    String auth = _basicAuth(cfg_dav_user, cfg_dav_pass);
    _tcp->println("GET " + encodedPath + " HTTP/1.1");
    _tcp->println("Host: " + cfg_dav_host);
    _tcp->println("Authorization: Basic " + auth);
    _tcp->println("Connection: keep-alive");
    _tcp->println();

    esp_task_wdt_reset();  // feed WDT before waiting for response
    // Read HTTP headers
    long contentLength = -1;
    bool chunked = false;
    unsigned long timeout = millis();
    while (_tcp->connected() && millis() - timeout < 15000) {
      if (!_tcp->available()) { yield(); delay(1); continue; }
      String line = _tcp->readStringUntil('\n');
      line.trim();
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) {
          int code = line.substring(sp + 1, sp + 4).toInt();
          if (code >= 400) { _lastError = "HTTP " + String(code); return -1; }
        }
      }
      if (line.startsWith("Content-Length:") || line.startsWith("content-length:"))
        contentLength = line.substring(line.indexOf(':') + 1).toInt();
      if (line.indexOf("chunked") >= 0) chunked = true;
      if (line.length() == 0) break;
      timeout = millis();
    }

    long totalBytes = 0;
    timeout = millis();

    if (chunked) {
      while (_tcp->connected() && millis() - timeout < 30000) {
        if (!_tcp->available()) { delay(1); continue; }
        String sizeLine = _tcp->readStringUntil('\n');
        sizeLine.trim();
        if (sizeLine.length() == 0) { timeout = millis(); continue; }
        long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
        if (chunkSize <= 0) break;
        long bytesRead = 0;
        while (bytesRead < chunkSize && _tcp->connected() && millis() - timeout < 30000) {
          if (_tcp->available()) {
            size_t avail = _tcp->available();
            size_t want = chunkSize - bytesRead;
            if (avail > want) avail = want;
            size_t space = bufSize - totalBytes;
            if (space == 0) { bytesRead += avail; continue; }
            if (avail > space) avail = space;
            size_t got = _tcp->read(&buf[totalBytes], avail);
            totalBytes += got;
            bytesRead += got;
            timeout = millis();
          } else { yield(); delay(1); }
        }
        while (bytesRead < chunkSize && _tcp->connected() && millis() - timeout < 30000) {
          if (_tcp->available()) { _tcp->read(); bytesRead++; timeout = millis(); }
          else { yield(); delay(1); }
        }
        if (_tcp->available()) _tcp->read();  // \r
        if (_tcp->available()) _tcp->read();  // \n
        timeout = millis();
        yield();
      }
    } else {
      while ((_tcp->connected() || _tcp->available()) && millis() - timeout < 30000) {
        size_t avail = _tcp->available();
        if (avail == 0) { yield(); delay(5); continue; }
        size_t space = bufSize - totalBytes;
        if (space == 0) break;
        if (avail > space) avail = space;
        size_t got = _tcp->read(&buf[totalBytes], avail);
        if (got > 0) { totalBytes += got; timeout = millis(); }
        if (contentLength > 0 && totalBytes >= contentLength) break;
      }
    }

    if (totalBytes == 0) {
      if (_lastError.length() == 0) _lastError = "Zero bytes received";
      return -1;
    }

    _log("DAV: streamed " + String(totalBytes) + " bytes to RAM");
    return totalBytes;
  }

private:
  bool   _connected;
  String _lastError;
  String _debugLog;
  int    _httpStatus;

  // ── Persistent connection state ───────────────────────────────────────────
  WiFiClient       *_tcp;
  WiFiClientSecure *_secure;
  String _persistHost;
  int    _persistPort;
  bool   _persistHttps;

  // ── Logging ───────────────────────────────────────────────────────────────

  void _log(const String &msg) {
    Serial.println(msg);
    _debugLog += msg + "\n";
    if (_debugLog.length() > 2048) {
      _debugLog = _debugLog.substring(_debugLog.length() - 1500);
    }
#ifdef DEVICE_HAS_SDLOG
    sdLog(msg);
#elif defined(DEVICE_WIFI_DONGLE)
    logAppend(msg);  // persist to SPIFFS on dongle
#endif
  }

  // ── Persistent TCP/TLS connection management ──────────────────────────────

  // Check if existing connection is still alive
  bool _isConnectionAlive() {
    if (!_tcp) return false;
    // Config changed?
    if (_persistHost != cfg_dav_host || _persistPort != cfg_dav_port || _persistHttps != cfg_dav_https) {
      _log("DAV: config changed, closing old connection");
      _closeConnection();
      return false;
    }
    // TCP still connected?
    if (!_tcp->connected()) {
      _log("DAV: persistent connection dropped");
      _closeConnection();
      return false;
    }
    return true;
  }

  // ── TLS connect on separate task (needs ~20KB+ stack, main loop may not have enough) ──

  struct TLSConnectParams {
    WiFiClient *tcp;
    const char *host;
    int port;
    bool success;
    volatile bool done;
  };

  static void _tlsConnectTask(void *param) {
    TLSConnectParams *p = (TLSConnectParams *)param;
    p->success = p->tcp->connect(p->host, p->port);
    p->done = true;
    vTaskDelete(NULL);  // self-delete
  }

  // Get or create a TCP connection (reuses existing TLS session when possible)
  bool _ensureConnection() {
    if (_isConnectionAlive()) {
      // Drain any stale data from previous response
      int drained = 0;
      while (_tcp->available() && drained < 4096) { _tcp->read(); drained++; }
      if (drained > 0) _log("DAV: drained " + String(drained) + " stale bytes");
      return true;
    }

    size_t freeHeap = ESP.getFreeHeap();
    size_t freePsram = ESP.getFreePsram();
    _log("DAV: opening new connection to " + cfg_dav_host + ":" + String(cfg_dav_port) +
         (cfg_dav_https ? " (TLS)" : ""));
    _log("DAV: free heap=" + String(freeHeap) + " PSRAM=" + String(freePsram));

    // TLS needs ~45KB free heap — bail out early instead of crashing
    if (cfg_dav_https && freeHeap < 50000) {
      _lastError = "Not enough heap for TLS (" + String(freeHeap) + " free, need 50000)";
      _log("DAV: " + _lastError);
      return false;
    }

    // Feed WDT before long-running TLS handshake
    esp_task_wdt_reset();

    if (cfg_dav_https) {
      _secure = new (std::nothrow) WiFiClientSecure();
      if (!_secure) { _lastError = "Out of memory allocating WiFiClientSecure"; _log("DAV: " + _lastError); return false; }
      _secure->setInsecure();
      _secure->setTimeout(15000);   // 15 seconds — TLS handshake needs time
      _tcp = _secure;
    } else {
      _tcp = new (std::nothrow) WiFiClient();
      if (!_tcp) { _lastError = "Out of memory allocating WiFiClient"; _log("DAV: " + _lastError); return false; }
      _tcp->setTimeout(15000);
    }

    _log("DAV: connecting... (heap after alloc=" + String(ESP.getFreeHeap()) + ")");
    esp_task_wdt_reset();

    unsigned long t0 = millis();
    bool connectOk = false;

    if (cfg_dav_https) {
      // Run TLS connect on a dedicated task with 32KB stack
      // mbedTLS handshake needs much more stack than the Arduino loop provides
      TLSConnectParams params;
      params.tcp = _tcp;
      params.host = cfg_dav_host.c_str();
      params.port = cfg_dav_port;
      params.success = false;
      params.done = false;

      _log("DAV: spawning TLS task (32KB stack)...");
      TaskHandle_t taskHandle = NULL;
      BaseType_t created = xTaskCreatePinnedToCore(
        _tlsConnectTask, "tls_conn", 32768, &params, 5, &taskHandle, 1
      );

      if (created != pdPASS) {
        _lastError = "Failed to create TLS connect task";
        _log("DAV: " + _lastError);
        _closeConnection();
        return false;
      }

      // Wait for task to complete (max 20 seconds)
      while (!params.done && millis() - t0 < 20000) {
        esp_task_wdt_reset();
        delay(100);
      }

      if (!params.done) {
        _lastError = "TLS connect timeout (20s)";
        _log("DAV: " + _lastError);
        // Task may still be running — can't safely delete it, just abandon
        _closeConnection();
        return false;
      }

      connectOk = params.success;
    } else {
      // Plain TCP — connect on main task is fine
      connectOk = _tcp->connect(cfg_dav_host.c_str(), cfg_dav_port);
    }

    if (!connectOk) {
      unsigned long dt = millis() - t0;
      _lastError = "TCP connect failed (" + String(dt) + "ms)";
      if (_secure) {
        int errCode = _secure->lastError(nullptr, 0);
        _lastError += " TLS err=" + String(errCode);
      }
      _log("DAV: " + _lastError);
      esp_task_wdt_reset();
      _closeConnection();
      return false;
    }

    esp_task_wdt_reset();  // feed WDT after successful connect
    _persistHost = cfg_dav_host;
    _persistPort = cfg_dav_port;
    _persistHttps = cfg_dav_https;
    _log("DAV: connected in " + String(millis() - t0) + "ms, heap=" + String(ESP.getFreeHeap()));
    return true;
  }

  void _closeConnection() {
    if (_tcp) {
      _tcp->stop();
      // If _secure is set, _tcp points to _secure, so only delete once
      if (_secure) { delete _secure; _secure = nullptr; _tcp = nullptr; }
      else { delete _tcp; _tcp = nullptr; }
    }
  }

  // ── Path helpers ──────────────────────────────────────────────────────────

  String _buildFullPath(const String &remotePath) {
    String base = cfg_dav_path;
    if (!base.endsWith("/")) base += "/";
    if (remotePath.startsWith(base) || remotePath.startsWith(cfg_dav_path))
      return remotePath;
    String full = base;
    if (remotePath.startsWith("/")) full += remotePath.substring(1);
    else full += remotePath;
    return full;
  }

  // ── Base64 for HTTP Basic Auth ────────────────────────────────────────────

  String _basicAuth(const String &user, const String &pass) {
    String credentials = user + ":" + pass;
    int len = credentials.length();
    const char *data = credentials.c_str();
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String result = "";
    result.reserve((len + 2) / 3 * 4);
    for (int i = 0; i < len; i += 3) {
      uint32_t n = ((uint8_t)data[i]) << 16;
      if (i + 1 < len) n |= ((uint8_t)data[i + 1]) << 8;
      if (i + 2 < len) n |= ((uint8_t)data[i + 2]);
      result += b64[(n >> 18) & 0x3F];
      result += b64[(n >> 12) & 0x3F];
      result += (i + 1 < len) ? b64[(n >> 6) & 0x3F] : '=';
      result += (i + 2 < len) ? b64[n & 0x3F] : '=';
    }
    return result;
  }

  // ── URL encoding/decoding ─────────────────────────────────────────────────

  String _urlEncodePath(const String &path) {
    String result = "";
    result.reserve(path.length() + 16);
    for (int i = 0; i < (int)path.length(); i++) {
      char c = path.charAt(i);
      if (c == '/' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
        result += c;
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
        result += hex;
      }
    }
    return result;
  }

  String _urlDecodePath(const String &s) {
    String result = "";
    result.reserve(s.length());
    for (int i = 0; i < (int)s.length(); i++) {
      if (s.charAt(i) == '%' && i + 2 < (int)s.length()) {
        char hex[3] = { s.charAt(i+1), s.charAt(i+2), 0 };
        result += (char)strtol(hex, nullptr, 16);
        i += 2;
      } else {
        result += s.charAt(i);
      }
    }
    return result;
  }

  // ── HTTP body reader — reads into PSRAM buffer to avoid heap fragmentation ──
  // Result stored in _psramBody / _psramBodyLen. Caller must call _freePsramBody().

  void _readHTTPBody() {
    _httpStatus = 0;
    long contentLength = -1;
    bool chunked = false;
    bool connectionClose = false;

    // Read headers
    unsigned long timeout = millis();
    while (_tcp->connected() && millis() - timeout < 15000) {
      if (!_tcp->available()) { yield(); delay(1); continue; }
      String line = _tcp->readStringUntil('\n');
      line.trim();

      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) _httpStatus = line.substring(sp + 1, sp + 4).toInt();
      }
      String lower = line;
      lower.toLowerCase();
      if (lower.startsWith("content-length:"))
        contentLength = line.substring(line.indexOf(':') + 1).toInt();
      if (lower.indexOf("chunked") >= 0) chunked = true;
      if (lower.indexOf("connection: close") >= 0) connectionClose = true;
      if (line.length() == 0) break;
      timeout = millis();
    }
    _log("DAV: HTTP " + String(_httpStatus) + " len=" + String(contentLength) +
         (chunked ? " chunked" : "") + (connectionClose ? " close" : " keep-alive"));

    // Allocate body buffer in PSRAM — keeps internal heap free for TLS etc.
    const long MAX_BODY = 524288;  // 512KB max
    _freePsramBody();  // clean up any previous buffer
    char *bodyBuf = (char *)ps_malloc(MAX_BODY + 1);
    if (!bodyBuf) {
      _log("DAV: FATAL — cannot allocate " + String(MAX_BODY / 1024) + "KB in PSRAM for body");
      _closeConnection();
      return;
    }
    long bodyLen = 0;
    bool bodyTruncated = false;

    timeout = millis();
    if (chunked) {
      while (_tcp->connected() && millis() - timeout < 30000) {
        if (!_tcp->available()) { yield(); delay(1); continue; }
        String sizeLine = _tcp->readStringUntil('\n');
        sizeLine.trim();
        if (sizeLine.length() == 0) { timeout = millis(); continue; }
        long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
        if (chunkSize <= 0) break;
        if (bodyLen + chunkSize > MAX_BODY) {
          _log("DAV: body too large, truncating at " + String(bodyLen) + " bytes");
          bodyTruncated = true;
          break;
        }

        long bytesRead = 0;
        while (bytesRead < chunkSize && _tcp->connected() && millis() - timeout < 30000) {
          size_t avail = _tcp->available();
          if (avail == 0) { yield(); delay(1); continue; }
          size_t want = (size_t)(chunkSize - bytesRead);
          if (avail > want) avail = want;
          size_t toRead = avail > 4096 ? 4096 : avail;  // read in bigger chunks from PSRAM buf
          size_t got = _tcp->read((uint8_t *)&bodyBuf[bodyLen], toRead);
          if (got > 0) {
            bodyLen += got;
            bytesRead += got;
            timeout = millis();
            esp_task_wdt_reset();
            yield();
          }
        }
        // Read trailing \r\n
        if (_tcp->available()) _tcp->read();
        if (_tcp->available()) _tcp->read();
        timeout = millis();
        esp_task_wdt_reset();
      }
    } else if (contentLength > 0) {
      long maxRead = contentLength > MAX_BODY ? MAX_BODY : contentLength;
      if (contentLength > MAX_BODY) {
        _log("DAV: content-length " + String(contentLength) + " exceeds max, limiting");
        bodyTruncated = true;
      }
      while (bodyLen < maxRead && millis() - timeout < 30000) {
        size_t avail = _tcp->available();
        if (avail == 0) { yield(); delay(1); continue; }
        size_t want = (size_t)(maxRead - bodyLen);
        if (avail > want) avail = want;
        size_t toRead = avail > 4096 ? 4096 : avail;
        size_t got = _tcp->read((uint8_t *)&bodyBuf[bodyLen], toRead);
        if (got > 0) {
          bodyLen += got;
          timeout = millis();
          esp_task_wdt_reset();
          yield();
        }
      }
    } else {
      while (_tcp->connected() && millis() - timeout < 10000) {
        size_t avail = _tcp->available();
        if (avail == 0) { yield(); delay(1); continue; }
        size_t space = MAX_BODY - bodyLen;
        if (space == 0) { bodyTruncated = true; break; }
        if (avail > space) avail = space;
        size_t toRead = avail > 4096 ? 4096 : avail;
        size_t got = _tcp->read((uint8_t *)&bodyBuf[bodyLen], toRead);
        if (got > 0) {
          bodyLen += got;
          timeout = millis();
          esp_task_wdt_reset();
        }
      }
    }

    bodyBuf[bodyLen] = '\0';
    _log("DAV: body read " + String(bodyLen) + " bytes (PSRAM), heap=" + String(ESP.getFreeHeap()));

    // If body was truncated, remaining data is still on the TCP stream.
    // We MUST close the connection to avoid corrupting the next request.
    if (bodyTruncated) {
      _log("DAV: closing connection after truncated body");
      _closeConnection();
    } else if (connectionClose) {
      _log("DAV: server closed connection");
      _closeConnection();
    }

    _psramBody = bodyBuf;
    _psramBodyLen = bodyLen;
  }

  // PSRAM body pointer for large responses (caller must call _freePsramBody after use)
  char *_psramBody = nullptr;
  long  _psramBodyLen = 0;

  void _freePsramBody() {
    if (_psramBody) { free(_psramBody); _psramBody = nullptr; _psramBodyLen = 0; }
  }

  // ── XML parsing ───────────────────────────────────────────────────────────

  // Raw char* version: finds each <response> block in the PSRAM buffer,
  // extracts it as a small String (~200-500 bytes), and reuses the
  // existing String-based tag extraction on each block individually.
  // This avoids copying the entire 122KB+ XML into a heap String.
  void _parsePropfindResponseRaw(const char *xml, long xmlLen, const String &basePath,
                                  std::vector<DAVFileEntry> &entries) {
    bool firstEntry = true;
    const char *p = xml;
    const char *end = xml + xmlLen;

    while (p < end) {
      // Find <D:response>, <d:response>, or <response>
      const char *respStart = nullptr;
      const char *tags[] = { "<D:response", "<d:response", "<response" };
      for (int t = 0; t < 3 && !respStart; t++) {
        const char *f = strstr(p, tags[t]);
        if (f && (!respStart || f < respStart)) respStart = f;
      }
      if (!respStart) break;

      // Find closing </D:response>, </d:response>, or </response>
      const char *respEnd = nullptr;
      const char *closeTags[] = { "</D:response>", "</d:response>", "</response>" };
      for (int t = 0; t < 3 && !respEnd; t++) {
        const char *f = strstr(respStart + 1, closeTags[t]);
        if (f && (!respEnd || f < respEnd)) respEnd = f;
      }
      if (!respEnd) respEnd = end;
      else respEnd += 14;  // skip past closing tag

      // Extract this one <response> block as a small heap String
      int blockLen = respEnd - respStart;
      if (blockLen > 8192) blockLen = 8192;  // safety cap per entry
      String block;
      block.reserve(blockLen);
      for (int i = 0; i < blockLen; i++) block += respStart[i];

      if (firstEntry) {
        firstEntry = false;
        p = respEnd;
        continue;
      }

      // Parse this individual block using existing String helpers
      String href = _extractTagValue(block, "href");
      href = _urlDecodePath(href);

      String displayName = _extractTagValue(block, "displayname");
      if (displayName.length() == 0 && href.length() > 0) {
        String h = href;
        if (h.endsWith("/")) h = h.substring(0, h.length() - 1);
        int ls = h.lastIndexOf('/');
        if (ls >= 0) displayName = h.substring(ls + 1);
        else displayName = h;
      }

      if (displayName.length() == 0 || displayName == "." || displayName == "..") {
        p = respEnd;
        continue;
      }

      bool isDir = (block.indexOf("collection") >= 0);
      size_t fileSize = 0;
      String sizeStr = _extractTagValue(block, "getcontentlength");
      if (sizeStr.length() > 0) fileSize = sizeStr.toInt();

      DAVFileEntry entry;
      entry.name = displayName;
      entry.href = href;
      entry.isDir = isDir;
      entry.size = fileSize;
      entry.hasCover = false;
      entry.hasNfo = false;

      if (!entry.isDir) {
        String lname = displayName;
        lname.toLowerCase();
        bool isDiskImage = lname.endsWith(".adf") || lname.endsWith(".dsk") ||
                           lname.endsWith(".adz") || lname.endsWith(".img") ||
                           lname.endsWith(".zip");
        bool isCover = lname.endsWith(".jpg") || lname.endsWith(".jpeg") || lname.endsWith(".png");
        bool isNfo = lname.endsWith(".nfo");
        if (!isDiskImage && !isCover && !isNfo) { p = respEnd; continue; }
        if (isCover) { entry.coverFile = displayName; }
        if (isNfo)   { entry.nfoFile = displayName; }
      }

      entries.push_back(entry);
      p = respEnd;
      // Periodically yield to prevent WDT
      if (entries.size() % 50 == 0) { esp_task_wdt_reset(); yield(); }
    }
  }

  void _parsePropfindResponse(const String &xml, const String &basePath,
                               std::vector<DAVFileEntry> &entries) {
    int pos = 0;
    bool firstEntry = true;

    while (pos < (int)xml.length()) {
      int respStart = _findTagCI(xml, "response", pos);
      if (respStart < 0) break;
      int respEnd = _findTagCI(xml, "/response", respStart);
      if (respEnd < 0) respEnd = xml.length();

      String block = xml.substring(respStart, respEnd);

      String href = _extractTagValue(block, "href");
      href = _urlDecodePath(href);

      if (firstEntry) {
        firstEntry = false;
        pos = respEnd + 1;
        continue;
      }

      String displayName = _extractTagValue(block, "displayname");
      if (displayName.length() == 0 && href.length() > 0) {
        String h = href;
        if (h.endsWith("/")) h = h.substring(0, h.length() - 1);
        int ls = h.lastIndexOf('/');
        if (ls >= 0) displayName = h.substring(ls + 1);
        else displayName = h;
      }

      if (displayName.length() == 0 || displayName == "." || displayName == "..") {
        pos = respEnd + 1;
        continue;
      }

      bool isDir = (block.indexOf("collection") >= 0);
      size_t fileSize = 0;
      String sizeStr = _extractTagValue(block, "getcontentlength");
      if (sizeStr.length() > 0) fileSize = sizeStr.toInt();

      DAVFileEntry entry;
      entry.name = displayName;
      entry.href = href;
      entry.isDir = isDir;
      entry.size = fileSize;
      entry.hasCover = false;
      entry.hasNfo = false;

      if (!entry.isDir) {
        String lname = displayName;
        lname.toLowerCase();
        bool isDiskImage = lname.endsWith(".adf") || lname.endsWith(".dsk") ||
                           lname.endsWith(".adz") || lname.endsWith(".img") ||
                           lname.endsWith(".zip");
        bool isCover = lname.endsWith(".jpg") || lname.endsWith(".jpeg") || lname.endsWith(".png");
        bool isNfo = lname.endsWith(".nfo");
        if (!isDiskImage && !isCover && !isNfo) { pos = respEnd + 1; continue; }
        if (isCover) { entry.coverFile = displayName; }
        if (isNfo)   { entry.nfoFile = displayName; }
      }

      entries.push_back(entry);
      pos = respEnd + 1;
    }
  }

  int _findTagCI(const String &xml, const String &tagName, int startPos, bool isClose = false) {
    String actualTag = tagName;
    if (actualTag.startsWith("/")) { actualTag = actualTag.substring(1); isClose = true; }
    String prefix = isClose ? "</" : "<";
    String variants[] = { prefix + "D:" + actualTag, prefix + "d:" + actualTag, prefix + actualTag };
    int earliest = -1;
    for (int v = 0; v < 3; v++) {
      int found = xml.indexOf(variants[v], startPos);
      if (found >= 0 && (earliest < 0 || found < earliest)) earliest = found;
    }
    return earliest;
  }

  String _xmlDecode(const String &s) {
    String r = "";
    r.reserve(s.length());
    for (int i = 0; i < (int)s.length(); i++) {
      if (s.charAt(i) == '&') {
        int semi = s.indexOf(';', i + 1);
        if (semi > i && semi - i < 12) {
          String ent = s.substring(i + 1, semi);
          if (ent == "amp")  { r += '&'; i = semi; continue; }
          if (ent == "lt")   { r += '<'; i = semi; continue; }
          if (ent == "gt")   { r += '>'; i = semi; continue; }
          if (ent == "quot") { r += '"'; i = semi; continue; }
          if (ent == "apos") { r += '\''; i = semi; continue; }
          if (ent.startsWith("#x") || ent.startsWith("#X")) {
            char c = (char)strtol(ent.c_str() + 2, nullptr, 16);
            if (c) { r += c; i = semi; continue; }
          }
          if (ent.startsWith("#")) {
            char c = (char)atoi(ent.c_str() + 1);
            if (c) { r += c; i = semi; continue; }
          }
        }
      }
      r += s.charAt(i);
    }
    return r;
  }

  String _extractTagValue(const String &xml, const String &tagName) {
    String variants[] = { "D:" + tagName, "d:" + tagName, tagName };
    for (int v = 0; v < 3; v++) {
      String openTag = "<" + variants[v];
      int start = xml.indexOf(openTag);
      if (start < 0) continue;
      int gt = xml.indexOf('>', start);
      if (gt < 0) continue;
      if (xml.charAt(gt - 1) == '/') return "";
      String closeTag = "</" + variants[v] + ">";
      int end = xml.indexOf(closeTag, gt + 1);
      if (end < 0) continue;
      return _xmlDecode(xml.substring(gt + 1, end));
    }
    return "";
  }
};

// Global WebDAV client instance
GotekDAV davClient;

#endif // WEBDAV_CLIENT_H
