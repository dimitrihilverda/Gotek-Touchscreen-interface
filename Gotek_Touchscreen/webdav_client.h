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
  String href;            // full DAV path from PROPFIND href (URL-decoded), e.g. "/webdav/files/amiga/Turrican/"

  // Full DAV paths — populated by background indexer, empty until indexed
  String coverPath;       // e.g. "/webdav/files/amiga/Turrican/Turrican.jpg"
  String nfoPath;         // e.g. "/webdav/files/amiga/Turrican/Turrican.nfo"
  std::vector<String> diskPaths; // full paths to all disk files in folder
  bool   indexed;         // true = background indexer has run PROPFIND on this folder
};

// ============================================================================
// WebDAV Client Class
// ============================================================================

class GotekDAV {
public:
  GotekDAV() : _connected(false), _lastError(""), _debugLog("") {}

  String lastError() { return _lastError; }
  String lastDebug() { return _debugLog; }
  bool isConnected() { return _connected; }

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
    std::vector<DAVFileEntry> test;
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
  bool listDir(const String &path, std::vector<DAVFileEntry> &entries) {
    entries.clear();
    _lastError = "";

    // Build full path — if path already starts with cfg_dav_path, use it directly
    String fullPath;
    String base = cfg_dav_path;
    if (!base.endsWith("/")) base += "/";
    if (path.length() == 0 || path == "/") {
      fullPath = base;
    } else if (path.startsWith(base) || path.startsWith(cfg_dav_path)) {
      // Already a full path
      fullPath = path;
      if (!fullPath.endsWith("/")) fullPath += "/";
    } else {
      fullPath = base;
      if (path.startsWith("/")) fullPath += path.substring(1);
      else fullPath += path;
      if (!fullPath.endsWith("/")) fullPath += "/";
    }

    // URL-encode spaces in path but keep slashes
    String encodedPath = _urlEncodePath(fullPath);

    _log("DAV: PROPFIND " + encodedPath);

    // Create HTTPS or HTTP client on heap
    WiFiClient *tcp = nullptr;
    WiFiClientSecure *secure = nullptr;
    if (cfg_dav_https) {
      secure = new WiFiClientSecure();
      if (!secure) { _lastError = "Out of memory"; return false; }
      secure->setInsecure();  // Skip cert validation (ESP32 has no CA store)
      secure->setTimeout(15);
      tcp = secure;
    } else {
      tcp = new WiFiClient();
      if (!tcp) { _lastError = "Out of memory"; return false; }
      tcp->setTimeout(15);
    }

    _log("DAV: connecting to " + cfg_dav_host + ":" + String(cfg_dav_port) + " WiFiStatus=" + String(WiFi.status()) + " localIP=" + WiFi.localIP().toString());
    if (!tcp->connect(cfg_dav_host.c_str(), cfg_dav_port)) {
      _lastError = "TCP connect failed to " + cfg_dav_host + ":" + String(cfg_dav_port);
      _log("DAV: " + _lastError + " WiFiStatus=" + String(WiFi.status()));
      delete tcp;
      return false;
    }

    // Build PROPFIND request
    String auth = _basicAuth(cfg_dav_user, cfg_dav_pass);
    String body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                  "<D:propfind xmlns:D=\"DAV:\">"
                  "<D:prop><D:resourcetype/><D:getcontentlength/><D:displayname/></D:prop>"
                  "</D:propfind>";

    _log("DAV: -> PROPFIND " + encodedPath + " host=" + cfg_dav_host + " port=" + String(cfg_dav_port) + " https=" + String(cfg_dav_https));

    tcp->println("PROPFIND " + encodedPath + " HTTP/1.1");
    tcp->println("Host: " + cfg_dav_host);
    tcp->println("Authorization: Basic " + auth);
    tcp->println("Depth: 1");
    tcp->println("Content-Type: application/xml");
    tcp->println("Content-Length: " + String(body.length()));
    tcp->println("Connection: close");
    tcp->println();
    tcp->print(body);

    // Read response
    String response = _readHTTPBody(tcp);
    tcp->stop();
    delete tcp;

    if (response.length() == 0) {
      _lastError = "Empty response from server";
      _log("DAV: " + _lastError);
      return false;
    }

    // Check for HTTP error in stored status
    if (_httpStatus >= 400) {
      // Log response body for debugging
      _log("DAV: error body: " + response.substring(0, 500));
      // Include short excerpt in error for web UI
      String excerpt = response.substring(0, 120);
      excerpt.replace("\"", "'");
      excerpt.replace("\n", " ");
      excerpt.replace("\r", "");
      _lastError = "HTTP " + String(_httpStatus) + ": " + excerpt;
      return false;
    }

    // Log XML size and first/last part for debugging
    _log("DAV: body length=" + String(response.length()) + " bytes");
    _log("DAV: XML[0..400]: " + response.substring(0, 400));
    if (response.length() > 400) {
      _log("DAV: XML[last 200]: " + response.substring(response.length() - 200));
    }

    // Parse the multistatus XML response
    _parsePropfindResponse(response, fullPath, entries);

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

    // Build full remote path — if remotePath already starts with cfg_dav_path, use it directly
    String fullRemote;
    {
      String base3 = cfg_dav_path;
      if (!base3.endsWith("/")) base3 += "/";
      if (remotePath.startsWith(base3) || remotePath.startsWith(cfg_dav_path)) {
        fullRemote = remotePath;
      } else {
        fullRemote = base3;
        if (remotePath.startsWith("/")) fullRemote += remotePath.substring(1);
        else fullRemote += remotePath;
      }
    }

    String encodedPath = _urlEncodePath(fullRemote);

    _log("DAV: GET " + encodedPath);

    // Create HTTPS or HTTP client on heap
    WiFiClient *tcp = nullptr;
    WiFiClientSecure *secure = nullptr;
    if (cfg_dav_https) {
      secure = new WiFiClientSecure();
      if (!secure) { _lastError = "Out of memory"; return -1; }
      secure->setInsecure();
      secure->setTimeout(30);
      tcp = secure;
    } else {
      tcp = new WiFiClient();
      if (!tcp) { _lastError = "Out of memory"; return -1; }
      tcp->setTimeout(30);
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

    // Build full remote path — if remotePath already starts with cfg_dav_path, use it directly
    String fullRemote;
    String base2 = cfg_dav_path;
    if (!base2.endsWith("/")) base2 += "/";
    if (remotePath.startsWith(base2) || remotePath.startsWith(cfg_dav_path)) {
      fullRemote = remotePath;
    } else {
      fullRemote = base2;
      if (remotePath.startsWith("/")) fullRemote += remotePath.substring(1);
      else fullRemote += remotePath;
    }

    String encodedPath = _urlEncodePath(fullRemote);
    _log("DAV: GET->RAM " + encodedPath + " bufSize=" + String(bufSize));

    // Create HTTPS or HTTP client on heap
    WiFiClient *tcp = nullptr;
    WiFiClientSecure *secure = nullptr;
    if (cfg_dav_https) {
      secure = new WiFiClientSecure();
      if (!secure) { _lastError = "Out of memory"; return -1; }
      secure->setInsecure();
      secure->setTimeout(30);
      tcp = secure;
    } else {
      tcp = new WiFiClient();
      if (!tcp) { _lastError = "Out of memory"; return -1; }
      tcp->setTimeout(30);
    }

    _log("DAV: stream cfg: host=" + cfg_dav_host + " port=" + String(cfg_dav_port) + " https=" + String(cfg_dav_https) + " path_in=" + remotePath + " full=" + fullRemote);
    _log("DAV: stream WiFiStatus=" + String(WiFi.status()) + " localIP=" + WiFi.localIP().toString());

    if (!tcp->connect(cfg_dav_host.c_str(), cfg_dav_port)) {
      _lastError = "TCP connect failed for stream";
      _log("DAV: " + _lastError + " (host=" + cfg_dav_host + ":" + String(cfg_dav_port) + " WiFiStatus=" + String(WiFi.status()) + ")");
      delete tcp;
      return -1;
    }
    _log("DAV: TCP connected for stream ok");

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
        }
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

  // Read HTTP response body (skip headers, handle chunked/content-length)
  String _readHTTPBody(WiFiClient *tcp) {
    _httpStatus = 0;
    String body = "";
    long contentLength = -1;
    bool chunked = false;

    // Read headers
    unsigned long timeout = millis();
    while (tcp->connected() && millis() - timeout < 15000) {
      if (!tcp->available()) { delay(1); continue; }
      String line = tcp->readStringUntil('\n');
      line.trim();

      _log("DAV hdr: " + line);

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
    _log("DAV: HTTP " + String(_httpStatus) + " contentLen=" + String(contentLength) +
         (chunked ? " chunked" : ""));

    // Read body
    timeout = millis();
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
        _log("DAV: chunk #" + String(chunkCount) + " size=" + String(chunkSize));
        // Read chunk data
        long bytesRead = 0;
        while (bytesRead < chunkSize && tcp->connected() && millis() - timeout < 15000) {
          if (tcp->available()) {
            body += (char)tcp->read();
            bytesRead++;
            timeout = millis();
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
          body += (char)tcp->read();
          timeout = millis();
        } else {
          delay(1);
        }
      }
    } else {
      // Read until connection closes (or timeout)
      while (tcp->connected() && millis() - timeout < 10000) {
        if (tcp->available()) {
          body += (char)tcp->read();
          timeout = millis();
        } else {
          delay(1);
        }
      }
    }
    return body;
  }

  // Parse PROPFIND multistatus XML response
  // Extracts <D:href>, <D:displayname>, <D:getcontentlength>, and <D:collection/>
  void _parsePropfindResponse(const String &xml, const String &basePath,
                               std::vector<DAVFileEntry> &entries) {
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
      entry.href = href;   // full decoded path from PROPFIND response
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
