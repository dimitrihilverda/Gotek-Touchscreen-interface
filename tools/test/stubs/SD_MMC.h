// In-memory stand-ins for the SD filesystem. The parser tests never touch the
// card; these exist only so the download helpers in webdav_client.h compile.
#pragma once
#include "Arduino.h"
#include <map>
#include <string>

namespace fs { class FS {}; }

class File {
public:
  std::string *buf = nullptr;
  size_t rpos = 0;
  bool ok = false;
  explicit operator bool() const { return ok; }
  bool operator!() const { return !ok; }
  size_t write(const uint8_t *d, size_t n) { if (buf) buf->append((const char *)d, n); return n; }
  size_t read(uint8_t *d, size_t n) {
    if (!buf || rpos >= buf->size()) return 0;
    size_t k = std::min(n, buf->size() - rpos);
    memcpy(d, buf->data() + rpos, k);
    rpos += k;
    return k;
  }
  size_t size() const { return buf ? buf->size() : 0; }
  void close() {}
  void print(const String &) {}
  void println(const String &) {}
};

class _SDStub : public fs::FS {
public:
  std::map<std::string, std::string> files;
  bool exists(const char *p) { return files.count(p) != 0; }
  bool mkdir(const char *)   { return true; }
  bool remove(const char *p) { return files.erase(p) > 0; }
  File open(const char *p, const char *mode = "r") {
    File f;
    std::string key(p);
    if (mode && mode[0] == 'w') { files[key] = ""; f.buf = &files[key]; f.ok = true; }
    else if (files.count(key))  { f.buf = &files[key]; f.ok = true; }
    return f;
  }
};
static _SDStub SD_MMC;
