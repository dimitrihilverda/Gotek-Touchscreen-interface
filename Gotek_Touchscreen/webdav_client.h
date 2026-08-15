#ifndef WEBDAV_CLIENT_H
#define WEBDAV_CLIENT_H

/*
  Gotek Touchscreen — Lightweight WebDAV Client
  Uses WiFiClientSecure for HTTPS WebDAV (e.g. Stackstorage, Nextcloud, etc.)
  PROPFIND for directory listing, GET for file download.
  No external library dependencies.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <vector>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// WebDAV config variables are defined in the main .ino file:
// cfg_dav_enabled, cfg_dav_host, cfg_dav_port, cfg_dav_user, cfg_dav_pass, cfg_dav_path, cfg_dav_https

// ============================================================================
// WebDAV Types
// ============================================================================

struct DAVFileEntry {
  String name;
  bool   isDir;
  size_t size;
  bool   hasCover;   // true if folder contains a .jpg/.png
  bool   hasNfo;     // true if folder contains a .nfo
  String coverFile;  // name of cover file (e.g. "cover.jpg")
  String nfoFile;    // name of nfo file
};

// ---------------------------------------------------------------------------
// PSRAM-backed vector for entry lists.
//
// sizeof(DAVFileEntry) is ~56 bytes. A 3000-folder library therefore needs a
// ~170 KB contiguous array — and std::vector grows by doubling, so the moment
// it reallocates from 2048 to 4096 slots it wants old+new = ~340 KB live at
// once. The ESP32-S3's internal heap is ~300 KB total, so that allocation
// simply cannot succeed and the failure surfaces as an ESP_RST_PANIC reboot
// partway through a listing. The 8 MB of OPI PSRAM has room to spare, so the
// entry array lives there instead. Falls back to internal heap if ps_malloc
// fails, which keeps small listings working on a board without PSRAM.
//
// Arduino String contents still allocate from the internal heap, but for a
// folder listing only `name` is populated (cover/nfo stay empty and empty
// Strings never allocate), so that is ~25 bytes per entry rather than 56.
// ---------------------------------------------------------------------------
template <class T>
struct PsramAlloc {
  using value_type = T;
  PsramAlloc() noexcept {}
  template <class U> PsramAlloc(const PsramAlloc<U> &) noexcept {}
  T *allocate(size_t n) {
    void *p = ps_malloc(n * sizeof(T));
    if (!p) p = malloc(n * sizeof(T));   // no PSRAM (or exhausted) — try internal
    return (T *)p;
  }
  void deallocate(T *p, size_t) noexcept { free(p); }
  template <class U> bool operator==(const PsramAlloc<U> &) const noexcept { return true; }
  template <class U> bool operator!=(const PsramAlloc<U> &) const noexcept { return false; }
};

// Entry list type used everywhere a DAV listing is passed around. Declared as
// a typedef so call sites (`[]`, `.size()`, `.push_back()`, range-for) are
// unchanged — only the declarations needed touching.
using DAVEntryList = std::vector<DAVFileEntry, PsramAlloc<DAVFileEntry>>;

// ============================================================================
// WebDAV Client Class
// ============================================================================

class GotekDAV {
public:
  GotekDAV() : _connected(false), _lastError(""), _debugLog(""),
               _byteProgressCb(nullptr) {}

  String lastError() { return _lastError; }
  String lastDebug() { return _debugLog; }
  bool isConnected() { return _connected; }

  // Progress callback — invoked from within _readHTTPBody / streamToBuffer
  // as bytes arrive, throttled to ~10 Hz so it can't slow the transfer.
  // `total` is 0 if Content-Length was not advertised. Passing nullptr
  // clears any previous callback. Runs on the same task as the DAV call
  // (loop task); no threading concerns.
  using ByteProgressCb = void (*)(size_t received, size_t total);
  void setProgressCallback(ByteProgressCb cb) { _byteProgressCb = cb; }

  // Connect to WebDAV server (just validate connectivity)
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

    // Sanitize host: strip protocol prefix and trailing path/slash
    if (cfg_dav_host.startsWith("https://")) cfg_dav_host = cfg_dav_host.substring(8);
    if (cfg_dav_host.startsWith("http://"))  cfg_dav_host = cfg_dav_host.substring(7);
    int slashPos = cfg_dav_host.indexOf('/');
    if (slashPos > 0) cfg_dav_host = cfg_dav_host.substring(0, slashPos);
    cfg_dav_host.trim();

    _log("DAV: testing connection to " + String(cfg_dav_https ? "https://" : "http://") +
         cfg_dav_host + ":" + String(cfg_dav_port));

    // Test with a PROPFIND on the base path
    DAVEntryList test;
    if (listDir("/", test)) {
      _connected = true;
      _log("DAV: connected OK (" + String(test.size()) + " entries in root)");
      return true;
    }
    // _lastError already set by listDir
    return false;
  }

  void disconnect() {
    _connected = false;
    _log("DAV: disconnected");
  }

  // List directory contents via PROPFIND
  bool listDir(const String &path, DAVEntryList &entries) {
    entries.clear();
    _lastError = "";

    // Build full path
    String fullPath = cfg_dav_path;
    if (!fullPath.endsWith("/")) fullPath += "/";
    if (path.length() > 0 && path != "/") {
      if (path.startsWith("/")) fullPath += path.substring(1);
      else fullPath += path;
    }
    if (!fullPath.endsWith("/")) fullPath += "/";

    // URL-encode spaces in path but keep slashes
    String encodedPath = _urlEncodePath(fullPath);

    _log("DAV: listDir enter path='" + encodedPath + "' heap=" +
         String(ESP.getFreeHeap()) + " psram=" + String(ESP.getFreePsram()));

    // Create HTTPS or HTTP client on heap
    WiFiClient *tcp = nullptr;
    WiFiClientSecure *secure = nullptr;
    if (cfg_dav_https) {
      secure = new WiFiClientSecure();
      if (!secure) { _lastError = "Out of memory"; return false; }
      secure->setInsecure();  // Skip cert validation (ESP32 has no CA store)
      // Arduino Client::setTimeout takes MILLISECONDS on ESP32-Arduino. The
      // old value of 15 (= 15 ms) was effectively no timeout and let hung
      // TLS/PROPFIND requests block the loop task indefinitely. 15 s is
      // enough for a slow server to answer without freezing the UI forever.
      secure->setTimeout(15000);
      tcp = secure;
    } else {
      tcp = new WiFiClient();
      if (!tcp) { _lastError = "Out of memory"; return false; }
      tcp->setTimeout(15000);
    }

    if (!tcp->connect(cfg_dav_host.c_str(), cfg_dav_port)) {
      _lastError = "TCP connect failed to " + cfg_dav_host + ":" + String(cfg_dav_port);
      _log("DAV: " + _lastError);
      delete tcp;
      return false;
    }
    _log("DAV: TCP up heap=" + String(ESP.getFreeHeap()));

    // Build PROPFIND request
    String auth = _basicAuth(cfg_dav_user, cfg_dav_pass);
    String body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                  "<D:propfind xmlns:D=\"DAV:\">"
                  "<D:prop><D:resourcetype/><D:getcontentlength/><D:displayname/></D:prop>"
                  "</D:propfind>";

    _log("DAV: -> PROPFIND " + encodedPath + " Host: " + cfg_dav_host);

    tcp->println("PROPFIND " + encodedPath + " HTTP/1.1");
    tcp->println("Host: " + cfg_dav_host);
    tcp->println("Authorization: Basic " + auth);
    tcp->println("Depth: 1");
    tcp->println("Content-Type: application/xml");
    tcp->println("Content-Length: " + String(body.length()));
    tcp->println("Connection: close");
    tcp->println();
    tcp->print(body);
    _log("DAV: PROPFIND sent, awaiting headers...");

    // Read just the headers — the document itself is consumed by the
    // streaming parser below, which never materialises it in RAM.
    long contentLength = -1;
    bool chunked = false;
    _readHTTPHeaders(tcp, contentLength, chunked);
    _log("DAV: hdrs HTTP " + String(_httpStatus) + " len=" + String(contentLength) +
         (chunked ? " chunked" : "") + " heap=" + String(ESP.getFreeHeap()));

    if (_httpStatus >= 400) {
      // Read a small excerpt of the error body for the web UI, capped so a
      // verbose error page can't blow the heap on the way to reporting it.
      char errBuf[161];
      size_t n = 0;
      unsigned long t0 = millis();
      while (n < sizeof(errBuf) - 1 && millis() - t0 < 2000) {
        if (tcp->available()) errBuf[n++] = (char)tcp->read();
        else if (!tcp->connected()) break;
        else delay(1);
      }
      errBuf[n] = 0;
      for (size_t i = 0; i < n; i++) {
        if (errBuf[i] == '"')  errBuf[i] = '\'';
        if (errBuf[i] == '\n' || errBuf[i] == '\r') errBuf[i] = ' ';
      }
      tcp->stop();
      delete tcp;
      _lastError = "HTTP " + String(_httpStatus) + ": " + String(errBuf);
      _log("DAV: " + _lastError);
      return false;
    }

    // Reserve up front so the vector doesn't spend the listing doubling.
    // ~230 bytes of XML per <response> is typical for Nextcloud/Stack with
    // the four properties we request; over-reserving slightly is far cheaper
    // than a reallocation storm.
    if (contentLength > 0) {
      size_t est = (size_t)(contentLength / 230) + 16;
      if (est > 8192) est = 8192;
      entries.reserve(est);
    }

    const bool ok = _streamParsePropfind(tcp, contentLength, chunked, entries);
    tcp->stop();
    delete tcp;

    if (!ok) return false;
    if (entries.empty() && _httpStatus == 0) {
      _lastError = "Empty response from server";
      _log("DAV: " + _lastError);
      return false;
    }

    _log("DAV: listed " + String(entries.size()) + " entries in " + fullPath);
    _connected = true;
    return true;
  }

  // Download a file via GET
  // Returns bytes written, or -1 on error
  long downloadFile(const String &remotePath, const String &localPath) {
    _lastError = "";

    // Ensure parent directory exists on SD
    int lastSlash = localPath.lastIndexOf('/');
    if (lastSlash > 0) {
      String parentDir = localPath.substring(0, lastSlash);
      SD_MMC.mkdir(parentDir.c_str());
    }

    // Build full remote path
    String fullRemote = cfg_dav_path;
    if (!fullRemote.endsWith("/")) fullRemote += "/";
    if (remotePath.startsWith("/")) fullRemote += remotePath.substring(1);
    else fullRemote += remotePath;

    String encodedPath = _urlEncodePath(fullRemote);

    _log("DAV: GET " + encodedPath);

    // Create HTTPS or HTTP client on heap
    WiFiClient *tcp = nullptr;
    WiFiClientSecure *secure = nullptr;
    if (cfg_dav_https) {
      secure = new WiFiClientSecure();
      if (!secure) { _lastError = "Out of memory"; return -1; }
      secure->setInsecure();
      // setTimeout is milliseconds; 30 was almost-instant. 30 s here (the
      // GET/HEAD paths handle whole ADFs/covers and need a fair while).
      secure->setTimeout(30000);
      tcp = secure;
    } else {
      tcp = new WiFiClient();
      if (!tcp) { _lastError = "Out of memory"; return -1; }
      tcp->setTimeout(30000);
    }

    if (!tcp->connect(cfg_dav_host.c_str(), cfg_dav_port)) {
      _lastError = "TCP connect failed for download";
      _log("DAV: " + _lastError);
      delete tcp;
      return -1;
    }

    String auth = _basicAuth(cfg_dav_user, cfg_dav_pass);

    tcp->println("GET " + encodedPath + " HTTP/1.1");
    tcp->println("Host: " + cfg_dav_host);
    tcp->println("Authorization: Basic " + auth);
    tcp->println("Connection: close");
    tcp->println();

    // Skip HTTP headers, get content length
    long contentLength = -1;
    unsigned long timeout = millis();
    while (tcp->connected() && millis() - timeout < 15000) {
      if (!tcp->available()) { delay(1); continue; }
      String line = tcp->readStringUntil('\n');
      line.trim();

      // Check status line
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) {
          int code = line.substring(sp + 1, sp + 4).toInt();
          if (code >= 400) {
            _lastError = "HTTP " + String(code);
            tcp->stop();
            delete tcp;
            return -1;
          }
        }
      }

      if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
        contentLength = line.substring(line.indexOf(':') + 1).toInt();
      }
      if (line.length() == 0) break;  // End of headers
      timeout = millis();
    }

    // Stream to file
    File outFile = SD_MMC.open(localPath.c_str(), "w");
    if (!outFile) {
      tcp->stop();
      delete tcp;
      _lastError = "Cannot create local file";
      return -1;
    }

    long totalBytes = 0;
    uint8_t buf[4096];
    timeout = millis();

    while (tcp->connected() || tcp->available()) {
      if (millis() - timeout > 30000) {
        _lastError = "Download timeout";
        break;
      }
      size_t avail = tcp->available();
      if (avail == 0) { delay(5); continue; }

      size_t toRead = (avail > sizeof(buf)) ? sizeof(buf) : avail;
      size_t bytesRead = tcp->read(buf, toRead);
      if (bytesRead > 0) {
        outFile.write(buf, bytesRead);
        totalBytes += bytesRead;
        timeout = millis();
      }
    }

    outFile.close();
    tcp->stop();
    delete tcp;

    if (totalBytes == 0) {
      SD_MMC.remove(localPath.c_str());
      if (_lastError.length() == 0) _lastError = "Zero bytes received";
      return -1;
    }

    _log("DAV: downloaded " + fullRemote + " -> " + localPath + " (" + String(totalBytes) + " bytes)");
    return totalBytes;
  }

  // Stream a file directly into a memory buffer via GET
  // Returns bytes written, or -1 on error
  long streamToBuffer(const String &remotePath, uint8_t *buf, size_t bufSize) {
    _lastError = "";

    // Build full remote path
    String fullRemote = cfg_dav_path;
    if (!fullRemote.endsWith("/")) fullRemote += "/";
    if (remotePath.startsWith("/")) fullRemote += remotePath.substring(1);
    else fullRemote += remotePath;

    String encodedPath = _urlEncodePath(fullRemote);
    _log("DAV: GET->RAM " + encodedPath + " bufSize=" + String(bufSize));

    // Create HTTPS or HTTP client on heap
    WiFiClient *tcp = nullptr;
    WiFiClientSecure *secure = nullptr;
    if (cfg_dav_https) {
      secure = new WiFiClientSecure();
      if (!secure) { _lastError = "Out of memory"; return -1; }
      secure->setInsecure();
      // setTimeout is milliseconds; 30 was almost-instant. 30 s here (the
      // GET/HEAD paths handle whole ADFs/covers and need a fair while).
      secure->setTimeout(30000);
      tcp = secure;
    } else {
      tcp = new WiFiClient();
      if (!tcp) { _lastError = "Out of memory"; return -1; }
      tcp->setTimeout(30000);
    }

    if (!tcp->connect(cfg_dav_host.c_str(), cfg_dav_port)) {
      _lastError = "TCP connect failed for stream";
      _log("DAV: " + _lastError);
      delete tcp;
      return -1;
    }

    String auth = _basicAuth(cfg_dav_user, cfg_dav_pass);

    tcp->println("GET " + encodedPath + " HTTP/1.1");
    tcp->println("Host: " + cfg_dav_host);
    tcp->println("Authorization: Basic " + auth);
    tcp->println("Connection: close");
    tcp->println();

    // Read HTTP headers
    long contentLength = -1;
    bool chunked = false;
    unsigned long timeout = millis();
    while (tcp->connected() && millis() - timeout < 15000) {
      if (!tcp->available()) { delay(1); continue; }
      String line = tcp->readStringUntil('\n');
      line.trim();

      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) {
          int code = line.substring(sp + 1, sp + 4).toInt();
          if (code >= 400) {
            _lastError = "HTTP " + String(code);
            tcp->stop();
            delete tcp;
            return -1;
          }
        }
      }
      if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
        contentLength = line.substring(line.indexOf(':') + 1).toInt();
      }
      if (line.indexOf("chunked") >= 0) chunked = true;
      if (line.length() == 0) break;
      timeout = millis();
    }

    _log("DAV: stream contentLen=" + String(contentLength) + (chunked ? " chunked" : ""));

    long totalBytes = 0;
    timeout = millis();

    // Same freeze story as _readHTTPBody: reads were already buffered but
    // never yielded when the socket had data flowing steadily. Add a
    // yield() after each read burst so WiFi bookkeeping, touch polling
    // and the USB MSC task get scheduling time during multi-second ADF
    // downloads. Also refuses to spin if the socket is stalled with no
    // data — the delay(1)/delay(5) branch was the only implicit yield
    // and only fired on empty reads.
    if (chunked) {
      // Chunked transfer: read each chunk into buffer
      while (tcp->connected() && millis() - timeout < 30000) {
        if (!tcp->available()) { delay(1); continue; }
        String sizeLine = tcp->readStringUntil('\n');
        sizeLine.trim();
        if (sizeLine.length() == 0) { timeout = millis(); continue; }
        long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
        if (chunkSize <= 0) break;
        long bytesRead = 0;
        while (bytesRead < chunkSize && tcp->connected() && millis() - timeout < 30000) {
          if (tcp->available()) {
            size_t avail = tcp->available();
            size_t want = chunkSize - bytesRead;
            if (avail > want) avail = want;
            size_t space = bufSize - totalBytes;
            if (space == 0) { bytesRead += avail; continue; }  // buffer full, drain
            if (avail > space) avail = space;
            size_t got = tcp->read(&buf[totalBytes], avail);
            totalBytes += got;
            bytesRead += got;
            timeout = millis();
            _fireProgress((size_t)totalBytes, contentLength > 0 ? (size_t)contentLength : 0);
            yield();
          } else {
            delay(1);
          }
        }
        // Drain any overflow if buffer was full
        while (bytesRead < chunkSize && tcp->connected() && millis() - timeout < 30000) {
          if (tcp->available()) { tcp->read(); bytesRead++; timeout = millis(); }
          else delay(1);
        }
        if (tcp->available()) tcp->read();  // \r
        if (tcp->available()) tcp->read();  // \n
        timeout = millis();
        yield();
      }
    } else {
      // Content-Length or read-till-close
      while ((tcp->connected() || tcp->available()) && millis() - timeout < 30000) {
        size_t avail = tcp->available();
        if (avail == 0) { delay(5); continue; }
        size_t space = bufSize - totalBytes;
        if (space == 0) break;  // buffer full
        if (avail > space) avail = space;
        size_t got = tcp->read(&buf[totalBytes], avail);
        if (got > 0) {
          totalBytes += got;
          timeout = millis();
          _fireProgress((size_t)totalBytes, contentLength > 0 ? (size_t)contentLength : 0);
        }
        yield();
      }
    }

    tcp->stop();
    delete tcp;

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
  ByteProgressCb _byteProgressCb;

  // Fire progress callback (if set) at most once per 100 ms so the UI
  // gets a live counter without slowing the transfer to render speed.
  void _fireProgress(size_t received, size_t total) {
    if (!_byteProgressCb) return;
    static uint32_t lastFireMs = 0;
    uint32_t now = millis();
    if (now - lastFireMs < 100 && received != total) return;
    lastFireMs = now;
    _byteProgressCb(received, total);
  }

  void _log(const String &msg) {
    _debugLog += msg + "\n";
    // Keep last 2KB only
    if (_debugLog.length() > 2048) {
      _debugLog = _debugLog.substring(_debugLog.length() - 1500);
    }
    // Also write to SD card log file (if logging enabled)
    sdLog(msg);
  }

  // Base64 encode for HTTP Basic Auth
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

  // URL-encode path segments (keep slashes, encode spaces etc.)
  String _urlEncodePath(const String &path) {
    String result = "";
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

  // Consume the HTTP status line + headers, leaving the socket positioned at
  // the first body byte. Sets _httpStatus and reports framing to the caller.
  // Split out of _readHTTPBody so the streaming parser can take over from
  // here without the body ever being buffered.
  void _readHTTPHeaders(WiFiClient *tcp, long &contentLength, bool &chunked) {
    _httpStatus   = 0;
    contentLength = -1;
    chunked       = false;
    unsigned long timeout = millis();
    while (tcp->connected() && millis() - timeout < 15000) {
      if (!tcp->available()) { delay(1); continue; }
      String line = tcp->readStringUntil('\n');
      line.trim();
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) _httpStatus = line.substring(sp + 1, sp + 4).toInt();
      }
      if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
        contentLength = line.substring(line.indexOf(':') + 1).toInt();
      }
      if (line.indexOf("chunked") >= 0) chunked = true;
      if (line.length() == 0) break;      // blank line ends the header section
      timeout = millis();
    }
  }

  // Read HTTP response body (skip headers, handle chunked/content-length).
  // Only the small GET paths use this now — listDir streams instead.
  String _readHTTPBody(WiFiClient *tcp) {
    _httpStatus = 0;
    String body = "";
    long contentLength = -1;
    bool chunked = false;
    // v0.11.3: log body progress at every 64-KB boundary. Populated by the
    // three read paths below so we can watch heap fall as the body grows.
    size_t lastLoggedKB = 0;

    // Read headers
    unsigned long timeout = millis();
    while (tcp->connected() && millis() - timeout < 15000) {
      if (!tcp->available()) { delay(1); continue; }
      String line = tcp->readStringUntil('\n');
      line.trim();
      // Per-header sdLog removed — used to fire ~10 SD writes per response
      // and contended with the loop task's other SD work. The batched
      // "hdrs done" log below has status+length+chunked+heap in one line.
      if (line.startsWith("HTTP/")) {
        int sp = line.indexOf(' ');
        if (sp > 0) _httpStatus = line.substring(sp + 1, sp + 4).toInt();
      }
      if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
        contentLength = line.substring(line.indexOf(':') + 1).toInt();
      }
      if (line.indexOf("chunked") >= 0) chunked = true;
      if (line.length() == 0) break;
      timeout = millis();
    }
    _log("DAV: hdrs done HTTP " + String(_httpStatus) + " len=" + String(contentLength) +
         (chunked ? " chunked" : "") + " heap=" + String(ESP.getFreeHeap()));

    // Read body
    timeout = millis();
    // Chunked buffer-based reads. The previous implementation grew `body`
    // one byte at a time (`body += (char)tcp->read()`) which is O(N²) for
    // a String (each append checks capacity + realloc on growth) and
    // never yielded when the socket had steady data available. Large
    // PROPFIND responses (100+ KB) would freeze the loop task for several
    // seconds and eventually trip the task watchdog. We now readBytes()
    // into a stack buffer, append via null-terminated concat, and yield()
    // every chunk so WiFi / touch / USB MSC all get their turn.
    static const size_t READ_CHUNK = 256;
    char cbuf[READ_CHUNK + 1];

    if (chunked) {
      // Chunked transfer encoding: each chunk starts with hex size + \r\n,
      // followed by data, followed by \r\n. Final chunk is "0\r\n\r\n".
      int chunkCount = 0;
      while (tcp->connected() && millis() - timeout < 15000) {
        if (!tcp->available()) { delay(1); continue; }
        // Read chunk size line
        String sizeLine = tcp->readStringUntil('\n');
        sizeLine.trim();
        if (sizeLine.length() == 0) { timeout = millis(); continue; }
        long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
        if (chunkSize <= 0) break;  // Final chunk
        chunkCount++;
        // Per-chunk sdLog removed — fired ~150 SD writes for a 750-KB
        // library (mid-transfer + USB-MSC bus contention). 64-KB body
        // breadcrumbs below are enough to watch heap decay.
        // Pre-grow body so the concat below doesn't realloc mid-loop
        body.reserve(body.length() + chunkSize);
        long bytesRead = 0;
        while (bytesRead < chunkSize && tcp->connected() && millis() - timeout < 15000) {
          if (tcp->available()) {
            size_t want = chunkSize - bytesRead;
            if (want > READ_CHUNK) want = READ_CHUNK;
            size_t got = tcp->readBytes(cbuf, want);
            if (got > 0) {
              cbuf[got] = '\0';
              body.concat(cbuf);
              bytesRead += got;
              timeout = millis();
              _fireProgress(body.length(), contentLength > 0 ? (size_t)contentLength : 0);
              size_t curKB = body.length() >> 16;  // 64-KB units
              if (curKB > lastLoggedKB) {
                lastLoggedKB = curKB;
                _log("DAV: body @" + String((unsigned)(curKB * 64)) +
                     "K heap=" + String(ESP.getFreeHeap()));
              }
            }
            yield();
          } else {
            delay(1);
          }
        }
        // Read trailing \r\n after chunk data
        if (tcp->available()) tcp->read();  // \r
        if (tcp->available()) tcp->read();  // \n
        timeout = millis();
      }
    } else if (contentLength > 0) {
      body.reserve(contentLength);
      while ((int)body.length() < contentLength && millis() - timeout < 15000) {
        if (tcp->available()) {
          size_t want = contentLength - body.length();
          if (want > READ_CHUNK) want = READ_CHUNK;
          size_t got = tcp->readBytes(cbuf, want);
          if (got > 0) {
            cbuf[got] = '\0';
            body.concat(cbuf);
            timeout = millis();
            _fireProgress(body.length(), contentLength > 0 ? (size_t)contentLength : 0);
            size_t curKB = body.length() >> 16;
            if (curKB > lastLoggedKB) {
              lastLoggedKB = curKB;
              _log("DAV: body @" + String((unsigned)(curKB * 64)) +
                   "K heap=" + String(ESP.getFreeHeap()));
            }
          }
          yield();
        } else {
          delay(1);
        }
      }
    } else {
      // Read until connection closes (or timeout)
      while (tcp->connected() && millis() - timeout < 10000) {
        if (tcp->available()) {
          size_t got = tcp->readBytes(cbuf, READ_CHUNK);
          if (got > 0) {
            cbuf[got] = '\0';
            body.concat(cbuf);
            timeout = millis();
            _fireProgress(body.length(), 0);
            size_t curKB = body.length() >> 16;
            if (curKB > lastLoggedKB) {
              lastLoggedKB = curKB;
              _log("DAV: body @" + String((unsigned)(curKB * 64)) +
                   "K heap=" + String(ESP.getFreeHeap()));
            }
          }
          yield();
        } else {
          delay(1);
        }
      }
    }
    return body;
  }

  // ==========================================================================
  // Streaming PROPFIND parser
  //
  // The old path read the whole multistatus document into an Arduino String
  // and then walked it with indexOf/substring. For a 3000-folder library that
  // document is 600-800 KB, which does not fit in the ESP32-S3's ~300 KB
  // internal heap — the listing died with an OOM that surfaced as a PANIC
  // reboot. Nothing about the format requires random access, so we now parse
  // as the bytes arrive and never hold more than one <response> block.
  //
  // Peak memory is the 8 KB sliding window below, regardless of library size.
  // ==========================================================================

  static const size_t PARSE_WIN = 8192;   // sliding window over the XML stream

  // Case-insensitive scan for an XML tag, tolerating any namespace prefix
  // (<D:response>, <d:response>, <lp1:response>, <response>). Returns the
  // index of the opening '<', or -1.
  //
  // A tag whose delimiter would fall outside the buffer is reported as
  // not-found so the caller pulls more bytes and retries — that is what makes
  // it safe to run over a partial window.
  static int _scanTag(const char *p, size_t len, const char *tagName, bool closing) {
    const size_t tlen = strlen(tagName);
    if (len < tlen + 2) return -1;
    for (size_t i = 0; i + tlen + 2 <= len; i++) {
      if (p[i] != '<') continue;
      size_t j = i + 1;
      if (closing) {
        if (p[j] != '/') continue;
        j++;
      } else if (p[j] == '/') {
        continue;
      }
      // Optional namespace prefix: [A-Za-z0-9_-]{1,12} ':'
      size_t nsStart = j;
      while (j < len && (j - nsStart) < 12 &&
             (isalnum((unsigned char)p[j]) || p[j] == '-' || p[j] == '_')) j++;
      if (j < len && p[j] == ':') j++;   // prefix consumed
      else                        j = nsStart;   // no prefix — rewind
      if (j + tlen >= len) return -1;    // tag + delimiter not fully buffered
      if (strncasecmp(p + j, tagName, tlen) != 0) continue;
      const char d = p[j + tlen];
      if (d != '>' && d != ' ' && d != '/' && d != '\t' && d != '\r' && d != '\n') continue;
      return (int)i;
    }
    return -1;
  }

  // Decode the XML entities WebDAV servers actually emit, in place. Without
  // this a game called "Tom & Jerry" listed as "Tom &amp; Jerry" — a
  // long-standing display bug in the old parser, which never decoded at all.
  static void _decodeEntities(char *s) {
    char *r = s, *w = s;
    while (*r) {
      if (*r != '&') { *w++ = *r++; continue; }
      if      (!strncmp(r, "&amp;",  5)) { *w++ = '&';  r += 5; }
      else if (!strncmp(r, "&lt;",   4)) { *w++ = '<';  r += 4; }
      else if (!strncmp(r, "&gt;",   4)) { *w++ = '>';  r += 4; }
      else if (!strncmp(r, "&quot;", 6)) { *w++ = '"';  r += 6; }
      else if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; }
      else if (r[1] == '#') {
        // Numeric entity. Anything above U+00FF can't be shown by the 8-bit
        // font, so it becomes '?' rather than mangled bytes.
        int base = (r[2] == 'x' || r[2] == 'X') ? 16 : 10;
        const char *numStart = r + (base == 16 ? 3 : 2);
        char *endp = nullptr;
        long cp = strtol(numStart, &endp, base);
        if (endp && *endp == ';' && cp > 0) {
          *w++ = (cp < 256) ? (char)cp : '?';
          r = endp + 1;
        } else {
          *w++ = *r++;
        }
      } else {
        *w++ = *r++;
      }
    }
    *w = 0;
  }

  // Extract the text content of <ns:tag>...</ns:tag> from a response block
  // into `out`. Returns true if the tag was present (an empty or self-closing
  // tag counts as present with an empty value).
  static bool _extractTagBuf(const char *p, size_t len, const char *tagName,
                             char *out, size_t outCap) {
    out[0] = 0;
    int s = _scanTag(p, len, tagName, false);
    if (s < 0) return false;
    // Advance to the end of the opening tag
    size_t gt = (size_t)s;
    while (gt < len && p[gt] != '>') gt++;
    if (gt >= len) return false;
    if (p[gt - 1] == '/') return true;          // self-closing: present, empty
    size_t vStart = gt + 1;
    int e = _scanTag(p + vStart, len - vStart, tagName, true);
    if (e < 0) return true;                     // unterminated: treat as empty
    size_t vLen = (size_t)e;
    if (vLen >= outCap) vLen = outCap - 1;
    memcpy(out, p + vStart, vLen);
    out[vLen] = 0;
    _decodeEntities(out);
    return true;
  }

  // Percent-decode a URL path in place (href values arrive encoded).
  static void _urlDecodeBuf(char *s) {
    char *r = s, *w = s;
    while (*r) {
      if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
        char hex[3] = { r[1], r[2], 0 };
        *w++ = (char)strtol(hex, nullptr, 16);
        r += 3;
      } else if (*r == '+') {
        *w++ = ' '; r++;
      } else {
        *w++ = *r++;
      }
    }
    *w = 0;
  }

  // Turn one <response>...</response> block into an entry and append it.
  // `skip` is set for the very first block, which is always the collection
  // being listed rather than one of its children.
  void _parseResponseBlock(const char *blk, size_t len, bool skip,
                           DAVEntryList &entries) {
    if (skip) return;

    char nameBuf[192];
    // displayname is authoritative when present; otherwise fall back to the
    // last path segment of href.
    bool haveName = _extractTagBuf(blk, len, "displayname", nameBuf, sizeof(nameBuf));
    if (!haveName || nameBuf[0] == 0) {
      char hrefBuf[320];
      if (!_extractTagBuf(blk, len, "href", hrefBuf, sizeof(hrefBuf))) return;
      _urlDecodeBuf(hrefBuf);
      size_t hl = strlen(hrefBuf);
      while (hl > 0 && hrefBuf[hl - 1] == '/') hrefBuf[--hl] = 0;
      const char *slash = strrchr(hrefBuf, '/');
      const char *base  = slash ? slash + 1 : hrefBuf;
      strncpy(nameBuf, base, sizeof(nameBuf) - 1);
      nameBuf[sizeof(nameBuf) - 1] = 0;
    }
    if (nameBuf[0] == 0) return;
    if (!strcmp(nameBuf, ".") || !strcmp(nameBuf, "..")) return;

    const bool isDir = (_scanTag(blk, len, "collection", false) >= 0);

    size_t fileSize = 0;
    char sizeBuf[24];
    if (_extractTagBuf(blk, len, "getcontentlength", sizeBuf, sizeof(sizeBuf)) && sizeBuf[0]) {
      fileSize = (size_t)strtoul(sizeBuf, nullptr, 10);
    }

    DAVFileEntry entry;
    entry.isDir    = isDir;
    entry.size     = fileSize;
    entry.hasCover = false;
    entry.hasNfo   = false;

    if (!isDir) {
      // Classify by extension; anything that isn't a disk image, cover or NFO
      // is noise as far as the browser is concerned.
      String lname(nameBuf);
      lname.toLowerCase();
      const bool isDisk  = _isDiskImageName(lname);
      const bool isCover = lname.endsWith(".jpg") || lname.endsWith(".jpeg") ||
                           lname.endsWith(".png");
      const bool isNfo   = lname.endsWith(".nfo") || lname.endsWith(".txt");
      if (!isDisk && !isCover && !isNfo) return;
      entry.name = nameBuf;
      if (isCover) entry.coverFile = entry.name;
      if (isNfo)   entry.nfoFile   = entry.name;
    } else {
      entry.name = nameBuf;
    }

    entries.push_back(entry);
  }

  // Disk-image extensions we surface. Kept in one place so adding a new
  // platform's format (Atari .st/.msa, Spectrum +3 .dsk, HxC .hfe, raw .img)
  // is a one-line change rather than a hunt through the parser.
  static bool _isDiskImageName(const String &lower) {
    static const char *kExts[] = {
      ".adf", ".adz", ".dms",            // Amiga
      ".dsk", ".cpc", ".edsk",           // Amstrad CPC / Spectrum +3
      ".st",  ".msa", ".stx",            // Atari ST
      ".img", ".ima", ".dmf",            // PC / generic raw
      ".hfe",                            // HxC / FlashFloppy native
      ".d64", ".d81",                    // Commodore
      ".mgt", ".sad",                    // SAM Coupe
      ".fdi", ".scp",                    // generic flux / disk image
      ".zip",                            // archives (handled downstream)
    };
    for (unsigned i = 0; i < sizeof(kExts) / sizeof(kExts[0]); i++) {
      if (lower.endsWith(kExts[i])) return true;
    }
    return false;
  }

  // Pull the multistatus document off the socket, emitting entries as blocks
  // complete. Handles both chunked and Content-Length framing.
  bool _streamParsePropfind(WiFiClient *tcp, long contentLength, bool chunked,
                            DAVEntryList &entries) {
    char *win = (char *)malloc(PARSE_WIN + 1);
    if (!win) { _lastError = "Out of memory (parse window)"; return false; }

    size_t used = 0;             // bytes currently in the window
    size_t totalBytes = 0;       // bytes pulled off the socket overall
    size_t lastLogged = 0;
    bool   firstBlock = true;    // first <response> is the collection itself
    long   chunkRemaining = chunked ? 0 : -1;   // -1 = not chunked
    bool   eof = false;
    unsigned long timeout = millis();

    // Loop until the socket is drained AND the window has been parsed out.
    // EOF must never short-circuit straight to the exit: the final reads
    // almost always leave one or more complete <response> blocks sitting in
    // the window, and breaking early would silently drop the last entries of
    // every listing.
    while (millis() - timeout < 30000) {
      // ---- refill -------------------------------------------------------
      if (!eof && used < PARSE_WIN) {
        size_t want = PARSE_WIN - used;
        bool   canRead = true;

        if (chunked) {
          if (chunkRemaining == 0) {
            // Between chunks: read the hex size line.
            if (tcp->available()) {
              String sizeLine = tcp->readStringUntil('\n');
              sizeLine.trim();
              if (sizeLine.length() > 0) {
                chunkRemaining = strtol(sizeLine.c_str(), nullptr, 16);
                if (chunkRemaining <= 0) eof = true;   // terminal chunk
                timeout = millis();
              }
              canRead = false;   // re-evaluate framing on the next pass
            } else if (!tcp->connected()) {
              eof = true;
              canRead = false;
            } else {
              delay(1);
              canRead = false;
            }
          }
          if (canRead && (long)want > chunkRemaining) want = (size_t)chunkRemaining;
        } else if (contentLength > 0) {
          size_t remain = (size_t)contentLength - totalBytes;
          if (remain == 0) { eof = true; canRead = false; }
          else if (want > remain) want = remain;
        }

        if (canRead && !eof) {
          if (tcp->available()) {
            size_t got = tcp->readBytes(win + used, want);
            if (got > 0) {
              used       += got;
              totalBytes += got;
              if (chunked) {
                chunkRemaining -= got;
                if (chunkRemaining == 0) {
                  // Consume the CRLF that terminates the chunk body.
                  if (tcp->available()) tcp->read();
                  if (tcp->available()) tcp->read();
                }
              }
              timeout = millis();
              _fireProgress(totalBytes, contentLength > 0 ? (size_t)contentLength : 0);
              if (totalBytes - lastLogged >= 65536) {
                lastLogged = totalBytes;
                _log("DAV: parsed @" + String((unsigned)(totalBytes / 1024)) +
                     "K entries=" + String(entries.size()) +
                     " heap=" + String(ESP.getFreeHeap()));
              }
            }
          } else if (!tcp->connected()) {
            eof = true;
          } else {
            delay(1);
          }
        }
      }

      // ---- drain complete <response> blocks -----------------------------
      size_t consumed = 0;
      while (true) {
        int s = _scanTag(win + consumed, used - consumed, "response", false);
        if (s < 0) break;
        size_t bs = consumed + (size_t)s;
        int e = _scanTag(win + bs, used - bs, "response", true);
        if (e < 0) break;                       // block not complete yet
        size_t be = bs + (size_t)e;
        _parseResponseBlock(win + bs, be - bs, firstBlock, entries);
        firstBlock = false;
        // Skip past the closing tag so the next scan starts cleanly.
        size_t after = be;
        while (after < used && win[after] != '>') after++;
        consumed = (after < used) ? after + 1 : used;
        yield();
      }

      // ---- compact ------------------------------------------------------
      if (consumed > 0) {
        memmove(win, win + consumed, used - consumed);
        used -= consumed;
      } else if (used >= PARSE_WIN) {
        // A single response block larger than the window. Real servers never
        // do this; rather than stall forever, resync on the last opening tag
        // we can see and drop everything before it.
        int last = -1, from = 0;
        while (true) {
          int s = _scanTag(win + from, used - from, "response", false);
          if (s < 0) break;
          last = from + s;
          from = last + 1;
        }
        if (last > 0) {
          memmove(win, win + last, used - last);
          used -= last;
        } else {
          used = 0;    // nothing recognisable — start over
        }
        _log("DAV: parse window overflow, resynced");
      }

      // ---- termination --------------------------------------------------
      // Once the socket is drained, keep going only while the drain step is
      // still making progress. `consumed == 0` at EOF means what's left in
      // the window is a trailing fragment (</multistatus>, whitespace) with
      // no further complete block in it.
      if (eof && consumed == 0) break;
      yield();
    }

    free(win);
    _log("DAV: stream parse done, " + String(entries.size()) + " entries, " +
         String((unsigned)(totalBytes / 1024)) + " KB, heap=" +
         String(ESP.getFreeHeap()));
    return true;
  }

  // Parse PROPFIND multistatus XML response (legacy whole-document path).
  // Retained for reference/fallback; the live path is _streamParsePropfind.
  void _parsePropfindResponse(const String &xml, const String &basePath,
                               DAVEntryList &entries) {
    int pos = 0;
    bool firstEntry = true;

    while (pos < (int)xml.length()) {
      // Find next <D:response> or <d:response>
      int respStart = _findTagCI(xml, "response", pos);
      if (respStart < 0) break;

      int respEnd = _findTagCI(xml, "/response", respStart);
      if (respEnd < 0) respEnd = xml.length();

      String block = xml.substring(respStart, respEnd);

      // Extract href
      String href = _extractTagValue(block, "href");
      href = _urlDecodePath(href);

      // Skip the base directory itself (first entry)
      if (firstEntry) {
        firstEntry = false;
        pos = respEnd + 1;
        continue;
      }

      // Extract displayname (may be empty)
      String displayName = _extractTagValue(block, "displayname");

      // If displayname is empty, derive from href
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

      // Check if it's a collection (directory)
      bool isDir = (block.indexOf("collection") >= 0);

      // Get content length
      size_t fileSize = 0;
      String sizeStr = _extractTagValue(block, "getcontentlength");
      if (sizeStr.length() > 0) fileSize = sizeStr.toInt();

      DAVFileEntry entry;
      entry.name = displayName;
      entry.isDir = isDir;
      entry.size = fileSize;
      entry.hasCover = false;
      entry.hasNfo = false;

      // Categorize files
      if (!entry.isDir) {
        String lname = displayName;
        lname.toLowerCase();
        bool isDiskImage = lname.endsWith(".adf") || lname.endsWith(".dsk") ||
                           lname.endsWith(".adz") || lname.endsWith(".img") ||
                           lname.endsWith(".zip");
        bool isCover = lname.endsWith(".jpg") || lname.endsWith(".jpeg") || lname.endsWith(".png");
        bool isNfo = lname.endsWith(".nfo");

        // Skip files that aren't disk images, covers, or nfo
        if (!isDiskImage && !isCover && !isNfo) {
          pos = respEnd + 1;
          continue;
        }
        // Mark cover/nfo files specially (they'll be used as metadata)
        if (isCover) { entry.coverFile = displayName; }
        if (isNfo)   { entry.nfoFile = displayName; }
      }

      entries.push_back(entry);
      pos = respEnd + 1;
    }
  }

  // Find a tag case-insensitively (handles D:tag, d:tag, tag variants)
  // For closing tags, pass tagName without the slash (isClose=true)
  int _findTagCI(const String &xml, const String &tagName, int startPos, bool isClose = false) {
    String actualTag = tagName;
    // Legacy support: strip leading slash, treat as close tag
    if (actualTag.startsWith("/")) {
      actualTag = actualTag.substring(1);
      isClose = true;
    }
    String prefix = isClose ? "</" : "<";
    String variants[] = { prefix + "D:" + actualTag, prefix + "d:" + actualTag, prefix + actualTag };
    int earliest = -1;
    for (int v = 0; v < 3; v++) {
      int found = xml.indexOf(variants[v], startPos);
      if (found >= 0 && (earliest < 0 || found < earliest)) {
        earliest = found;
      }
    }
    return earliest;
  }

  // Extract text content of an XML tag (case-insensitive namespace)
  String _extractTagValue(const String &xml, const String &tagName) {
    // Try D:tag, d:tag, tag
    String variants[] = { "D:" + tagName, "d:" + tagName, tagName };
    for (int v = 0; v < 3; v++) {
      String openTag = "<" + variants[v];
      int start = xml.indexOf(openTag);
      if (start < 0) continue;
      // Skip to end of opening tag
      int gt = xml.indexOf('>', start);
      if (gt < 0) continue;
      // Check for self-closing tag
      if (xml.charAt(gt - 1) == '/') return "";
      // Find closing tag
      String closeTag = "</" + variants[v] + ">";
      int end = xml.indexOf(closeTag, gt + 1);
      if (end < 0) continue;
      return xml.substring(gt + 1, end);
    }
    return "";
  }

  // URL-decode a path string
  String _urlDecodePath(const String &s) {
    String result = "";
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
};

// Global WebDAV client instance
GotekDAV davClient;

#endif // WEBDAV_CLIENT_H
