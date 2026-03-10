#ifndef FTP_CLIENT_H
#define FTP_CLIENT_H

/*
  Gotek Touchscreen — Lightweight FTP Client
  Uses raw WiFiClient for FTP control + data channels.
  Supports passive mode (PASV) for NAT-friendly operation.
  No external library dependencies.
*/

#include <WiFi.h>

// FTP config variables are defined in the main .ino file:
// cfg_ftp_enabled, cfg_ftp_host, cfg_ftp_port, cfg_ftp_user, cfg_ftp_pass, cfg_ftp_path

// ============================================================================
// FTP Types
// ============================================================================

struct FTPFileEntry {
  String name;
  bool   isDir;
  size_t size;
};

// ============================================================================
// FTP Client Class
// ============================================================================

class GotekFTP {
public:
  GotekFTP() : _connected(false), _lastError("") {}

  // Connect to FTP server and login
  bool connect() {
    _lastError = "";
    if (cfg_ftp_host.length() == 0) {
      _lastError = "No FTP host configured";
      return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
      _lastError = "WiFi not connected";
      return false;
    }

    Serial.println("FTP: connecting to " + cfg_ftp_host + ":" + String(cfg_ftp_port));

    if (!_ctrl.connect(cfg_ftp_host.c_str(), cfg_ftp_port)) {
      _lastError = "Connection failed";
      return false;
    }
    _ctrl.setTimeout(10000);

    // Read welcome banner (220)
    String resp = _readResponse();
    if (!resp.startsWith("220")) {
      _lastError = "Bad welcome: " + resp;
      disconnect();
      return false;
    }

    // Login
    if (!_sendCmd("USER " + cfg_ftp_user, "331")) {
      // Some servers accept USER directly with 230
      if (!_lastResp.startsWith("230")) {
        _lastError = "USER failed: " + _lastResp;
        disconnect();
        return false;
      }
    } else {
      if (!_sendCmd("PASS " + cfg_ftp_pass, "230")) {
        _lastError = "Login failed: " + _lastResp;
        disconnect();
        return false;
      }
    }

    // Set binary transfer mode
    _sendCmd("TYPE I", "200");

    _connected = true;
    Serial.println("FTP: logged in as " + cfg_ftp_user);
    return true;
  }

  void disconnect() {
    if (_ctrl.connected()) {
      _ctrl.println("QUIT");
      delay(100);
      _ctrl.stop();
    }
    _connected = false;
    Serial.println("FTP: disconnected");
  }

  bool isConnected() { return _connected && _ctrl.connected(); }
  String lastError() { return _lastError; }

  // List directory contents
  bool listDir(const String &path, std::vector<FTPFileEntry> &entries) {
    entries.clear();
    _lastError = "";

    if (!isConnected()) {
      _lastError = "Not connected";
      return false;
    }

    // CWD to requested path
    String fullPath = cfg_ftp_path;
    if (!fullPath.endsWith("/")) fullPath += "/";
    if (path.length() > 0 && path != "/") {
      if (path.startsWith("/")) fullPath += path.substring(1);
      else fullPath += path;
    }

    if (!_sendCmd("CWD " + fullPath, "250")) {
      _lastError = "CWD failed: " + _lastResp;
      return false;
    }

    // Open PASV data channel
    WiFiClient data;
    if (!_openPasv(data)) return false;

    // Send MLSD (machine-readable listing) or fall back to LIST
    bool useMlsd = true;
    _ctrl.println("MLSD");
    String resp = _readResponse();
    if (!resp.startsWith("150") && !resp.startsWith("125")) {
      // MLSD not supported, try LIST -la
      data.stop();
      if (!_openPasv(data)) return false;
      useMlsd = false;
      _ctrl.println("LIST -la");
      resp = _readResponse();
      if (!resp.startsWith("150") && !resp.startsWith("125")) {
        data.stop();
        _lastError = "LIST failed: " + resp;
        return false;
      }
    }

    // Read data
    String listing = "";
    unsigned long timeout = millis();
    while (data.connected() || data.available()) {
      if (millis() - timeout > 15000) break;
      while (data.available()) {
        listing += (char)data.read();
        timeout = millis();
      }
      delay(1);
    }
    data.stop();

    // Read transfer complete (226)
    _readResponse();

    // Parse listing
    if (useMlsd) {
      _parseMlsd(listing, entries);
    } else {
      _parseList(listing, entries);
    }

    Serial.println("FTP: listed " + String(entries.size()) + " entries in " + fullPath);
    return true;
  }

  // Download a file from FTP to SD card
  // Returns bytes written, or -1 on error
  long downloadFile(const String &remotePath, const String &localPath) {
    _lastError = "";

    if (!isConnected()) {
      _lastError = "Not connected";
      return -1;
    }

    // Ensure parent directory exists on SD
    int lastSlash = localPath.lastIndexOf('/');
    if (lastSlash > 0) {
      String parentDir = localPath.substring(0, lastSlash);
      SD_MMC.mkdir(parentDir.c_str());
    }

    // Build full remote path
    String fullRemote = cfg_ftp_path;
    if (!fullRemote.endsWith("/")) fullRemote += "/";
    if (remotePath.startsWith("/")) fullRemote += remotePath.substring(1);
    else fullRemote += remotePath;

    // Get file size first (for progress)
    _sendCmd("SIZE " + fullRemote, "213");
    long fileSize = 0;
    if (_lastResp.startsWith("213")) {
      fileSize = _lastResp.substring(4).toInt();
    }

    // Open PASV data channel
    WiFiClient data;
    if (!_openPasv(data)) return -1;

    // Request file
    _ctrl.println("RETR " + fullRemote);
    String resp = _readResponse();
    if (!resp.startsWith("150") && !resp.startsWith("125")) {
      data.stop();
      _lastError = "RETR failed: " + resp;
      return -1;
    }

    // Open local file for writing
    File outFile = SD_MMC.open(localPath.c_str(), "w");
    if (!outFile) {
      data.stop();
      // Drain data channel
      while (data.available()) data.read();
      _readResponse();
      _lastError = "Cannot create local file";
      return -1;
    }

    // Stream download
    long totalBytes = 0;
    uint8_t buf[4096];
    unsigned long timeout = millis();

    while (data.connected() || data.available()) {
      if (millis() - timeout > 30000) {
        _lastError = "Download timeout";
        break;
      }
      size_t avail = data.available();
      if (avail == 0) { delay(5); continue; }

      size_t toRead = (avail > sizeof(buf)) ? sizeof(buf) : avail;
      size_t bytesRead = data.read(buf, toRead);
      if (bytesRead > 0) {
        outFile.write(buf, bytesRead);
        totalBytes += bytesRead;
        timeout = millis();
      }
    }

    outFile.close();
    data.stop();

    // Read transfer complete (226)
    _readResponse();

    if (totalBytes == 0) {
      SD_MMC.remove(localPath.c_str());
      if (_lastError.length() == 0) _lastError = "Zero bytes received";
      return -1;
    }

    Serial.println("FTP: downloaded " + fullRemote + " -> " + localPath + " (" + String(totalBytes) + " bytes)");
    return totalBytes;
  }

  // Get current working directory
  String pwd() {
    if (!isConnected()) return "";
    _sendCmd("PWD", "257");
    // Response: 257 "/path" ...
    int q1 = _lastResp.indexOf('"');
    int q2 = _lastResp.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 > q1) return _lastResp.substring(q1 + 1, q2);
    return "";
  }

private:
  WiFiClient _ctrl;
  bool       _connected;
  String     _lastError;
  String     _lastResp;

  // Read a complete FTP response (may be multi-line)
  String _readResponse() {
    _lastResp = "";
    unsigned long timeout = millis();
    while (millis() - timeout < 10000) {
      if (_ctrl.available()) {
        String line = _ctrl.readStringUntil('\n');
        line.trim();
        _lastResp = line;
        // Multi-line responses: "xyz-..." continues, "xyz ..." ends
        if (line.length() >= 4 && line.charAt(3) == ' ') {
          break;  // Final line of response
        }
        timeout = millis();
      }
      delay(1);
    }
    return _lastResp;
  }

  // Send command and check response starts with expected code
  bool _sendCmd(const String &cmd, const String &expectCode) {
    _ctrl.println(cmd);
    _readResponse();
    return _lastResp.startsWith(expectCode);
  }

  // Open a passive data connection
  bool _openPasv(WiFiClient &data) {
    _ctrl.println("PASV");
    String resp = _readResponse();
    if (!resp.startsWith("227")) {
      _lastError = "PASV failed: " + resp;
      return false;
    }

    // Parse PASV response: 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)
    int paren = resp.indexOf('(');
    int endParen = resp.indexOf(')', paren);
    if (paren < 0 || endParen < 0) {
      _lastError = "Bad PASV response";
      return false;
    }

    String nums = resp.substring(paren + 1, endParen);
    int parts[6];
    int idx = 0;
    int start = 0;
    for (int i = 0; i <= (int)nums.length() && idx < 6; i++) {
      if (i == (int)nums.length() || nums.charAt(i) == ',') {
        parts[idx++] = nums.substring(start, i).toInt();
        start = i + 1;
      }
    }
    if (idx < 6) {
      _lastError = "Bad PASV format";
      return false;
    }

    // Use FTP host IP (not PASV-reported IP which may be wrong behind NAT)
    int dataPort = parts[4] * 256 + parts[5];

    if (!data.connect(cfg_ftp_host.c_str(), dataPort)) {
      _lastError = "Data connection failed (port " + String(dataPort) + ")";
      return false;
    }
    data.setTimeout(15000);
    return true;
  }

  // Parse MLSD listing (machine-readable)
  void _parseMlsd(const String &listing, std::vector<FTPFileEntry> &entries) {
    int pos = 0;
    while (pos < (int)listing.length()) {
      int nl = listing.indexOf('\n', pos);
      if (nl < 0) nl = listing.length();
      String line = listing.substring(pos, nl);
      line.trim();
      pos = nl + 1;
      if (line.length() == 0) continue;

      // Format: "type=file;size=12345; filename.ext"
      int semi = line.lastIndexOf(';');
      if (semi < 0) continue;
      String facts = line.substring(0, semi);
      String name = line.substring(semi + 1);
      name.trim();

      if (name == "." || name == "..") continue;

      FTPFileEntry entry;
      entry.name = name;
      entry.isDir = false;
      entry.size = 0;

      facts.toLowerCase();
      if (facts.indexOf("type=dir") >= 0 || facts.indexOf("type=cdir") >= 0 || facts.indexOf("type=pdir") >= 0) {
        entry.isDir = true;
      }
      int sizeIdx = facts.indexOf("size=");
      if (sizeIdx >= 0) {
        String sizeStr = facts.substring(sizeIdx + 5);
        int endSize = sizeStr.indexOf(';');
        if (endSize >= 0) sizeStr = sizeStr.substring(0, endSize);
        entry.size = sizeStr.toInt();
      }

      // Filter: only show directories and ADF/DSK files
      if (!entry.isDir) {
        String lname = name;
        lname.toLowerCase();
        if (!lname.endsWith(".adf") && !lname.endsWith(".dsk") && !lname.endsWith(".adz") &&
            !lname.endsWith(".img") && !lname.endsWith(".zip")) {
          continue;
        }
      }

      entries.push_back(entry);
    }
  }

  // Parse Unix-style LIST output
  void _parseList(const String &listing, std::vector<FTPFileEntry> &entries) {
    int pos = 0;
    while (pos < (int)listing.length()) {
      int nl = listing.indexOf('\n', pos);
      if (nl < 0) nl = listing.length();
      String line = listing.substring(pos, nl);
      line.trim();
      pos = nl + 1;
      if (line.length() < 10) continue;

      // Unix format: drwxr-xr-x ... name
      // or: -rw-r--r-- ... name
      bool isDir = (line.charAt(0) == 'd');

      // Extract filename (last token after the date)
      // Find last space-separated token by working backwards
      // Typical: "drwxr-xr-x 2 user group 4096 Jan 01 12:00 dirname"
      // or:      "-rw-r--r-- 1 user group 901120 Jan 01 12:00 file.adf"

      // Skip lines starting with "total"
      if (line.startsWith("total")) continue;

      // Find name: after the date/time (3 fields from the end might not work)
      // More robust: split into fields
      int fieldCount = 0;
      bool inSpace = true;
      int nameStart = -1;
      int sizeField = -1;
      int fieldIdx = 0;

      for (int i = 0; i < (int)line.length(); i++) {
        bool isSpace = (line.charAt(i) == ' ' || line.charAt(i) == '\t');
        if (inSpace && !isSpace) {
          fieldCount++;
          fieldIdx++;
          if (fieldIdx == 5) sizeField = i;  // 5th field is size
          if (fieldIdx >= 9) { nameStart = i; break; }  // 9th field onwards is name
        }
        inSpace = isSpace;
      }

      if (nameStart < 0) continue;
      String name = line.substring(nameStart);
      name.trim();
      if (name == "." || name == "..") continue;

      // Parse size from 5th field
      size_t fileSize = 0;
      if (sizeField >= 0) {
        String sizeStr = "";
        for (int i = sizeField; i < (int)line.length() && line.charAt(i) != ' '; i++) {
          sizeStr += line.charAt(i);
        }
        fileSize = sizeStr.toInt();
      }

      FTPFileEntry entry;
      entry.name = name;
      entry.isDir = isDir;
      entry.size = fileSize;

      // Filter: only show directories and ADF/DSK files
      if (!entry.isDir) {
        String lname = name;
        lname.toLowerCase();
        if (!lname.endsWith(".adf") && !lname.endsWith(".dsk") && !lname.endsWith(".adz") &&
            !lname.endsWith(".img") && !lname.endsWith(".zip")) {
          continue;
        }
      }

      entries.push_back(entry);
    }
  }
};

// Global FTP client instance
GotekFTP ftpClient;

#endif // FTP_CLIENT_H
