// Minimal Arduino compatibility shims so firmware headers can be compiled and
// unit-tested on the host. Only the surface the WebDAV client actually uses is
// modelled — this is a test scaffold, not an emulator.
#pragma once
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <cctype>

// ── String ────────────────────────────────────────────────────────────────
// Arduino's String over std::string. Semantics that matter for the parser:
// indexOf returns -1 when absent, substring clamps, toInt stops at junk.
class String {
public:
  std::string s;
  String() {}
  String(const char *p) : s(p ? p : "") {}
  String(const std::string &o) : s(o) {}
  String(char c) { s.assign(1, c); }
  explicit String(int v)           { char b[24]; snprintf(b, sizeof b, "%d",  v); s = b; }
  explicit String(long v)          { char b[24]; snprintf(b, sizeof b, "%ld", v); s = b; }
  explicit String(unsigned v)      { char b[24]; snprintf(b, sizeof b, "%u",  v); s = b; }
  explicit String(unsigned long v) { char b[24]; snprintf(b, sizeof b, "%lu", v); s = b; }
  explicit String(size_t v)        { char b[24]; snprintf(b, sizeof b, "%zu", v); s = b; }

  const char *c_str() const { return s.c_str(); }
  unsigned length()   const { return (unsigned)s.size(); }
  char charAt(int i)  const { return (i >= 0 && i < (int)s.size()) ? s[i] : 0; }
  char operator[](int i) const { return charAt(i); }

  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *p)   { s += p;   return *this; }
  String &operator+=(char c)          { s += c;   return *this; }
  void concat(const char *p)          { s += p; }
  void reserve(size_t n)              { s.reserve(n); }

  bool operator==(const String &o) const { return s == o.s; }
  bool operator==(const char *p)   const { return s == (p ? p : ""); }
  bool operator!=(const String &o) const { return s != o.s; }

  int indexOf(const String &n, int from = 0) const {
    if (from < 0) from = 0;
    if ((size_t)from > s.size()) return -1;
    auto p = s.find(n.s, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(char c, int from = 0) const {
    if (from < 0) from = 0;
    if ((size_t)from > s.size()) return -1;
    auto p = s.find(c, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int lastIndexOf(char c) const {
    auto p = s.rfind(c);
    return p == std::string::npos ? -1 : (int)p;
  }
  String substring(int a) const {
    if (a < 0) a = 0;
    if ((size_t)a >= s.size()) return String();
    return String(s.substr(a));
  }
  String substring(int a, int b) const {
    if (a < 0) a = 0;
    if (b > (int)s.size()) b = (int)s.size();
    if (a >= b) return String();
    return String(s.substr(a, b - a));
  }
  bool startsWith(const String &p) const { return s.rfind(p.s, 0) == 0; }
  bool endsWith(const String &p) const {
    return s.size() >= p.s.size() && s.compare(s.size() - p.s.size(), p.s.size(), p.s) == 0;
  }
  void trim() {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
  }
  void toLowerCase() { std::transform(s.begin(), s.end(), s.begin(), ::tolower); }
  void replace(const String &f, const String &r) {
    if (f.s.empty()) return;
    size_t p = 0;
    while ((p = s.find(f.s, p)) != std::string::npos) { s.replace(p, f.s.size(), r.s); p += r.s.size(); }
  }
  long toInt() const { return strtol(s.c_str(), nullptr, 10); }
};

inline String operator+(const String &a, const String &b) { String r(a); r += b; return r; }
inline String operator+(const String &a, const char *b)   { String r(a); r += b; return r; }
inline String operator+(const char *a, const String &b)   { String r(a); r += b; return r; }

// ── timing / scheduling ───────────────────────────────────────────────────
inline unsigned long millis() {
  using namespace std::chrono;
  static auto t0 = steady_clock::now();
  return (unsigned long)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}
inline void delay(unsigned long) {}   // tests drive the fake socket synchronously
inline void yield() {}

// ── ESP / PSRAM ───────────────────────────────────────────────────────────
inline void *ps_malloc(size_t n) { return malloc(n); }
struct _ESPStub {
  uint32_t getFreeHeap()  { return 200000; }
  uint32_t getFreePsram() { return 8000000; }
};
static _ESPStub ESP;

// Captured rather than printed so tests stay quiet unless they fail.
#include <vector>
static std::vector<std::string> g_sdLogLines;
inline void sdLog(const String &m) { g_sdLogLines.push_back(m.s); }
