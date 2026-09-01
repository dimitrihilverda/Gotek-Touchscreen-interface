// ============================================================================
// Host-side unit tests for the streaming PROPFIND parser in webdav_client.h.
//
// The parser is a state machine over a byte stream, so the failure modes that
// matter are the ones a real socket produces and a desk-check does not: tags
// split across reads, blocks straddling the sliding window, chunked framing
// boundaries landing mid-tag, and the last entry of a listing being dropped
// when the stream ends.
//
// Every test therefore runs at several socket chunk sizes, including 1 byte
// per read — the pathological case that catches reassembly bugs.
//
// Build + run:  tools/test/run.sh
// ============================================================================
#include "stubs/Arduino.h"
#include "stubs/WiFi.h"
#include "stubs/SD_MMC.h"

// The client no longer reads globals: settings arrive through configure().
// See davTestConfig() below, called wherever a test constructs a GotekDAV.

// The parser internals under test are private; opening them up is the least
// invasive way to test the real code rather than a copy of it.
#define private public
#define WiFiClientSecure_h
#include "../../Gotek_Touchscreen/webdav_client.h"
#undef private

#include <cstdio>
#include <string>
#include <vector>
#include <functional>

static int g_pass = 0, g_fail = 0;

// The same settings the old globals carried, handed over the new way.
static void davTestConfig(GotekDAV &dav) {
  DavConfig c;
  c.host = "example.com";  c.port = 443;  c.https = true;
  c.user = "u";            c.pass = "p";
  c.basePath = "/remote.php/dav/files/u/Games";
  c.enabled = true;
  dav.configure(c, nullptr);
}
static std::string g_currentTest;

static void check(bool cond, const std::string &what) {
  if (cond) { g_pass++; return; }
  g_fail++;
  printf("  FAIL [%s] %s\n", g_currentTest.c_str(), what.c_str());
}

template <typename A, typename B>
static void checkEq(const A &got, const B &want, const std::string &what) {
  if (got == want) { g_pass++; return; }
  g_fail++;
  std::ostringstream oss;
  printf("  FAIL [%s] %s\n", g_currentTest.c_str(), what.c_str());
}

static void checkEqInt(long got, long want, const std::string &what) {
  if (got == want) { g_pass++; return; }
  g_fail++;
  printf("  FAIL [%s] %s: got %ld, want %ld\n", g_currentTest.c_str(), what.c_str(), got, want);
}

static void checkEqStr(const std::string &got, const std::string &want, const std::string &what) {
  if (got == want) { g_pass++; return; }
  g_fail++;
  printf("  FAIL [%s] %s: got \"%s\", want \"%s\"\n",
         g_currentTest.c_str(), what.c_str(), got.c_str(), want.c_str());
}

// ── XML builders ──────────────────────────────────────────────────────────

static std::string responseBlock(const std::string &href,
                                 const std::string &displayName,
                                 bool isDir,
                                 long size,
                                 const std::string &ns = "D") {
  std::string p = ns.empty() ? "" : ns + ":";
  std::string x = "<" + p + "response>";
  x += "<" + p + "href>" + href + "</" + p + "href>";
  x += "<" + p + "propstat><" + p + "prop>";
  if (!displayName.empty())
    x += "<" + p + "displayname>" + displayName + "</" + p + "displayname>";
  if (isDir) x += "<" + p + "resourcetype><" + p + "collection/></" + p + "resourcetype>";
  else {
    x += "<" + p + "resourcetype/>";
    x += "<" + p + "getcontentlength>" + std::to_string(size) + "</" + p + "getcontentlength>";
  }
  x += "</" + p + "prop><" + p + "status>HTTP/1.1 200 OK</" + p + "status></" + p + "propstat>";
  x += "</" + p + "response>";
  return x;
}

static std::string wrapMultistatus(const std::string &body, const std::string &ns = "D") {
  std::string p = ns.empty() ? "" : ns + ":";
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<" + p +
         "multistatus xmlns:" + (ns.empty() ? "D" : ns) + "=\"DAV:\">" +
         body + "</" + p + "multistatus>\n";
}

// Wrap a body in HTTP/1.1 with Content-Length framing.
static std::string httpContentLength(const std::string &body, int status = 207) {
  char hdr[256];
  snprintf(hdr, sizeof hdr,
           "HTTP/1.1 %d Multi-Status\r\n"
           "Content-Type: application/xml; charset=utf-8\r\n"
           "Content-Length: %zu\r\n"
           "\r\n", status, body.size());
  return std::string(hdr) + body;
}

// Wrap a body in HTTP/1.1 with chunked framing, split into `chunkBytes` pieces.
static std::string httpChunked(const std::string &body, size_t chunkBytes, int status = 207) {
  char hdr[256];
  snprintf(hdr, sizeof hdr,
           "HTTP/1.1 %d Multi-Status\r\n"
           "Content-Type: application/xml; charset=utf-8\r\n"
           "Transfer-Encoding: chunked\r\n"
           "\r\n", status);
  std::string out(hdr);
  for (size_t i = 0; i < body.size(); i += chunkBytes) {
    size_t n = std::min(chunkBytes, body.size() - i);
    char sz[32];
    snprintf(sz, sizeof sz, "%zx\r\n", n);
    out += sz;
    out += body.substr(i, n);
    out += "\r\n";
  }
  out += "0\r\n\r\n";
  return out;
}

// ── Driver ────────────────────────────────────────────────────────────────

// Run the full header-read + stream-parse path over a canned HTTP response,
// with the fake socket handing over at most `sockChunk` bytes per read.
static DAVEntryList runParse(const std::string &httpResponse, size_t sockChunk) {
  GotekDAV dav;
  davTestConfig(dav);
  WiFiClient sock;
  sock.feed(httpResponse);
  sock.chunkSize = sockChunk;

  long contentLength = -1;
  bool chunked = false;
  dav._readHTTPHeaders(&sock, contentLength, chunked);

  DAVEntryList entries;
  dav._streamParsePropfind(&sock, contentLength, chunked, entries);
  return entries;
}

// Socket chunk sizes every scenario is replayed at. 1 is the important one.
static const size_t kChunkSizes[] = { 1, 3, 64, 512, 65536 };

static void forEachChunkSize(const std::string &name,
                             const std::function<void(size_t)> &body) {
  for (size_t cs : kChunkSizes) {
    g_currentTest = name + " @sock=" + std::to_string(cs);
    body(cs);
  }
}

// ── Tests ─────────────────────────────────────────────────────────────────

static void test_basicListing() {
  std::string body = wrapMultistatus(
      responseBlock("/remote.php/dav/files/u/Games/", "Games", true, 0) +
      responseBlock("/remote.php/dav/files/u/Games/Turrican/", "Turrican", true, 0) +
      responseBlock("/remote.php/dav/files/u/Games/Lemmings/", "Lemmings", true, 0) +
      responseBlock("/remote.php/dav/files/u/Games/Zool/", "Zool", true, 0));

  forEachChunkSize("basic", [&](size_t cs) {
    auto e = runParse(httpContentLength(body), cs);
    checkEqInt((long)e.size(), 3, "entry count (collection itself skipped)");
    if (e.size() == 3) {
      checkEqStr(e[0].name().s, "Turrican", "entry 0 name");
      checkEqStr(e[1].name().s, "Lemmings", "entry 1 name");
      checkEqStr(e[2].name().s, "Zool",     "entry 2 name (LAST — regression guard for EOF drain)");
      check(e[0].isDir && e[1].isDir && e[2].isDir, "all marked as directories");
    }
  });
}

static void test_chunkedFraming() {
  std::string body = wrapMultistatus(
      responseBlock("/g/", "Games", true, 0) +
      responseBlock("/g/Alpha/", "Alpha", true, 0) +
      responseBlock("/g/Beta/",  "Beta",  true, 0) +
      responseBlock("/g/Gamma/", "Gamma", true, 0));

  // Vary the HTTP chunk size independently of the socket chunk size so chunk
  // boundaries land inside tags, not just between them.
  for (size_t httpChunk : { (size_t)7, (size_t)61, (size_t)1024 }) {
    forEachChunkSize("chunked/http=" + std::to_string(httpChunk), [&](size_t cs) {
      auto e = runParse(httpChunked(body, httpChunk), cs);
      checkEqInt((long)e.size(), 3, "entry count");
      if (e.size() == 3) {
        checkEqStr(e[0].name().s, "Alpha", "entry 0");
        checkEqStr(e[2].name().s, "Gamma", "entry 2 (last)");
      }
    });
  }
}

static void test_namespaceVariants() {
  for (const std::string ns : { std::string("D"), std::string("d"),
                                std::string("lp1"), std::string("") }) {
    std::string body = wrapMultistatus(
        responseBlock("/g/", "Games", true, 0, ns) +
        responseBlock("/g/Hybris/", "Hybris", true, 0, ns), ns.empty() ? "D" : ns);
    forEachChunkSize("ns=" + (ns.empty() ? "(none)" : ns), [&](size_t cs) {
      auto e = runParse(httpContentLength(body), cs);
      checkEqInt((long)e.size(), 1, "entry count");
      if (e.size() == 1) checkEqStr(e[0].name().s, "Hybris", "name");
    });
  }
}

static void test_entityDecoding() {
  std::string body = wrapMultistatus(
      responseBlock("/g/", "Games", true, 0) +
      responseBlock("/g/T/", "Tom &amp; Jerry", true, 0) +
      responseBlock("/g/Q/", "&quot;Quoted&quot;", true, 0) +
      responseBlock("/g/L/", "A &lt;B&gt; C", true, 0) +
      responseBlock("/g/N/", "Caf&#233;", true, 0));

  forEachChunkSize("entities", [&](size_t cs) {
    auto e = runParse(httpContentLength(body), cs);
    checkEqInt((long)e.size(), 4, "entry count");
    if (e.size() == 4) {
      checkEqStr(e[0].name().s, "Tom & Jerry",  "&amp; decoded");
      checkEqStr(e[1].name().s, "\"Quoted\"",   "&quot; decoded");
      checkEqStr(e[2].name().s, "A <B> C",      "&lt;/&gt; decoded");
      checkEqStr(e[3].name().s, "Caf\xE9",      "numeric entity decoded");
    }
  });
}

static void test_hrefFallbackWhenNoDisplayName() {
  // Apache mod_dav often omits displayname; the name must come from href,
  // percent-decoded.
  std::string body = wrapMultistatus(
      responseBlock("/g/", "", true, 0) +
      responseBlock("/g/Monkey%20Island%202/", "", true, 0) +
      responseBlock("/g/Sim%20City/", "", true, 0));

  forEachChunkSize("href-fallback", [&](size_t cs) {
    auto e = runParse(httpContentLength(body), cs);
    checkEqInt((long)e.size(), 2, "entry count");
    if (e.size() == 2) {
      checkEqStr(e[0].name().s, "Monkey Island 2", "percent-decoded href basename");
      checkEqStr(e[1].name().s, "Sim City",        "second entry");
    }
  });
}

static void test_fileClassification() {
  std::string body = wrapMultistatus(
      responseBlock("/g/T/", "Turrican", true, 0) +
      responseBlock("/g/T/d1.adf",     "Turrican Disk 1.adf", false, 901120) +
      responseBlock("/g/T/d2.adf",     "Turrican Disk 2.adf", false, 901120) +
      responseBlock("/g/T/cover.jpg",  "cover.jpg",           false, 45981) +
      responseBlock("/g/T/notes.nfo",  "notes.nfo",           false, 512) +
      responseBlock("/g/T/junk.exe",   "junk.exe",            false, 1234) +
      responseBlock("/g/T/thumbs.db",  "thumbs.db",           false, 99));

  forEachChunkSize("classify", [&](size_t cs) {
    auto e = runParse(httpContentLength(body), cs);
    // .exe and .db are dropped; 2 disks + cover + nfo remain.
    checkEqInt((long)e.size(), 4, "noise files filtered out");
    if (e.size() == 4) {
      checkEqStr(e[0].name().s, "Turrican Disk 1.adf", "disk 1");
      checkEqInt((long)e[0].size, 901120, "disk 1 size parsed");
      check(!e[0].isDir, "disk is not a directory");
      check(e[2].hasCover, "cover tagged");
      check(e[3].hasNfo, "nfo tagged");
    }
  });
}

static void test_multiSystemExtensions() {
  // Formats used by the other machines a Gotek gets fitted to.
  std::string body = wrapMultistatus(
      responseBlock("/g/M/", "Mixed", true, 0) +
      responseBlock("/g/M/a.st",   "atari.st",    false, 737280) +
      responseBlock("/g/M/b.msa",  "atari.msa",   false, 400000) +
      responseBlock("/g/M/c.dsk",  "cpc.dsk",     false, 194816) +
      responseBlock("/g/M/d.hfe",  "flash.hfe",   false, 1000000) +
      responseBlock("/g/M/e.img",  "pc.img",      false, 1474560) +
      responseBlock("/g/M/f.d64",  "c64.d64",     false, 174848) +
      responseBlock("/g/M/g.ipf",  "unsupp.ipf",  false, 500000));

  forEachChunkSize("multi-system-ext", [&](size_t cs) {
    auto e = runParse(httpContentLength(body), cs);
    checkEqInt((long)e.size(), 6, ".st/.msa/.dsk/.hfe/.img/.d64 kept, .ipf dropped");
  });
}

static void test_largeListingStress() {
  // 3000 folders is the size that crashed the old whole-document parser.
  std::string body;
  body += responseBlock("/g/", "Games", true, 0);
  for (int i = 0; i < 3000; i++) {
    char nm[64];
    snprintf(nm, sizeof nm, "Game Number %04d", i);
    body += responseBlock(std::string("/g/") + nm + "/", nm, true, 0);
  }
  std::string full = wrapMultistatus(body);

  g_currentTest = "stress/3000";
  printf("  (stress: XML document is %zu KB)\n", full.size() / 1024);

  // Only the interesting socket sizes — 1 byte per read over 700 KB is slow.
  for (size_t cs : { (size_t)64, (size_t)512, (size_t)1460 }) {
    g_currentTest = "stress/3000 @sock=" + std::to_string(cs);
    auto e = runParse(httpContentLength(full), cs);
    checkEqInt((long)e.size(), 3000, "all 3000 entries parsed");
    if (e.size() == 3000) {
      checkEqStr(e[0].name().s,    "Game Number 0000", "first entry");
      checkEqStr(e[1499].name().s, "Game Number 1499", "middle entry");
      checkEqStr(e[2999].name().s, "Game Number 2999", "last entry");
    }
  }

  // Same document under chunked framing.
  g_currentTest = "stress/3000 chunked";
  auto e = runParse(httpChunked(full, 1400), 512);
  checkEqInt((long)e.size(), 3000, "all 3000 entries parsed (chunked)");
  if (e.size() == 3000) checkEqStr(e[2999].name().s, "Game Number 2999", "last entry (chunked)");
}

static void test_oversizedBlockResync() {
  // A single response block larger than the 8 KB window must not wedge the
  // parser — it should resync and keep the entries around it.
  std::string huge(20000, 'x');
  std::string body = wrapMultistatus(
      responseBlock("/g/", "Games", true, 0) +
      responseBlock("/g/Before/", "Before", true, 0) +
      responseBlock("/g/Huge/", huge, true, 0) +
      responseBlock("/g/After/", "After", true, 0));

  g_currentTest = "oversized-block";
  auto e = runParse(httpContentLength(body), 512);
  // The huge block is expected to be lost; the parser must survive and still
  // deliver the well-formed entries that follow it.
  bool sawAfter = false;
  for (auto &en : e) if (en.name().s == "After") sawAfter = true;
  check(sawAfter, "recovered and parsed the entry after an oversized block");
  check(e.size() >= 1, "did not lose everything");
}

static void test_truncatedStream() {
  // Server dies mid-document: parse what arrived, don't hang or crash.
  std::string body = wrapMultistatus(
      responseBlock("/g/", "Games", true, 0) +
      responseBlock("/g/One/", "One", true, 0) +
      responseBlock("/g/Two/", "Two", true, 0));
  std::string http = httpContentLength(body);
  http = http.substr(0, http.size() - 120);   // chop the tail

  g_currentTest = "truncated";
  auto e = runParse(http, 64);
  check(e.size() >= 1, "kept the entries that did arrive");
  if (e.size() >= 1) checkEqStr(e[0].name().s, "One", "first entry intact");
}

static void test_emptyCollection() {
  std::string body = wrapMultistatus(responseBlock("/g/Empty/", "Empty", true, 0));
  forEachChunkSize("empty-collection", [&](size_t cs) {
    auto e = runParse(httpContentLength(body), cs);
    checkEqInt((long)e.size(), 0, "no children");
  });
}

static void test_httpError() {
  g_currentTest = "http-401";
  GotekDAV dav;
  davTestConfig(dav);
  WiFiClient sock;
  sock.feed("HTTP/1.1 401 Unauthorized\r\nContent-Length: 13\r\n\r\nAccess denied");
  long cl = -1; bool ch = false;
  dav._readHTTPHeaders(&sock, cl, ch);
  checkEqInt(dav._httpStatus, 401, "status parsed from header");
}

static void test_scanTagUnitCases() {
  g_currentTest = "scanTag";
  const char *a = "<D:response>";
  checkEqInt(GotekDAV::_scanTag(a, strlen(a), "response", false), 0, "prefixed open tag");
  const char *b = "xx</d:response>yy";
  checkEqInt(GotekDAV::_scanTag(b, strlen(b), "response", true), 2, "prefixed close tag");
  const char *c = "<response >";
  checkEqInt(GotekDAV::_scanTag(c, strlen(c), "response", false), 0, "bare tag with space");
  const char *d = "<D:responsex>";
  checkEqInt(GotekDAV::_scanTag(d, strlen(d), "response", false), -1, "no false match on longer name");
  const char *e = "<D:resp";
  checkEqInt(GotekDAV::_scanTag(e, strlen(e), "response", false), -1, "incomplete tag reported absent");
  const char *f = "<D:collection/>";
  checkEqInt(GotekDAV::_scanTag(f, strlen(f), "collection", false), 0, "self-closing tag");
}

// ── Body pump (shared chunked / Content-Length reader) ────────────────────

// Drive streamToBuffer end to end by pre-seeding the socket. The GET request
// itself is discarded by the stub, so feeding a response is enough.
struct PumpResult {
  long   returned;
  std::string got;
  bool   truncated;
};

static PumpResult runStreamToBuffer(const std::string &httpResponse,
                                    size_t bufSize, size_t sockChunk) {
  static GotekDAV dav;          // static so lastTruncated() survives the call
  WiFiClient sock;
  sock.feed(httpResponse);
  sock.chunkSize = sockChunk;

  long cl = -1; bool ch = false;
  dav._readHTTPHeaders(&sock, cl, ch);

  std::vector<uint8_t> buf(bufSize + 1, 0);
  size_t written = 0;
  bool complete = false, truncated = false;
  dav._pumpBody(&sock, cl, ch,
      [&](const uint8_t *d, size_t n) -> size_t {
        size_t space = bufSize - written;
        size_t take  = (n < space) ? n : space;
        if (take) { memcpy(buf.data() + written, d, take); written += take; }
        return take;
      },
      &complete, &truncated);

  return { (long)written, std::string((char *)buf.data(), written), truncated };
}

static std::string bodyOfLength(size_t n) {
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; i++) s += (char)('A' + (i % 26));
  return s;
}

static void test_pumpContentLength() {
  std::string payload = bodyOfLength(5000);
  forEachChunkSize("pump/content-length", [&](size_t cs) {
    auto r = runStreamToBuffer(httpContentLength(payload, 200), 8192, cs);
    checkEqInt(r.returned, 5000, "all bytes delivered");
    check(r.got == payload, "payload byte-exact");
    check(!r.truncated, "not flagged truncated");
  });
}

static void test_pumpChunked() {
  std::string payload = bodyOfLength(5000);
  for (size_t httpChunk : { (size_t)1, (size_t)13, (size_t)1400 }) {
    forEachChunkSize("pump/chunked/http=" + std::to_string(httpChunk), [&](size_t cs) {
      auto r = runStreamToBuffer(httpChunked(payload, httpChunk, 200), 8192, cs);
      checkEqInt(r.returned, 5000, "all bytes delivered");
      check(r.got == payload, "payload byte-exact — chunk framing stripped, not stored");
    });
  }
}

static void test_pumpChunkedIntoSmallBuffer() {
  // THE regression this pump exists for. The old streamToBuffer, on a full
  // destination, advanced its chunk counter by bytes it had not actually read
  // from the socket. Framing then desynchronised and the next "chunk size
  // line" was parsed out of the middle of the payload, so the transfer died
  // early while still reporting a positive byte count. The pump must instead
  // keep draining, stop delivering, and say so.
  std::string payload = bodyOfLength(20000);
  for (size_t httpChunk : { (size_t)64, (size_t)1400 }) {
    forEachChunkSize("pump/chunked-overflow/http=" + std::to_string(httpChunk), [&](size_t cs) {
      auto r = runStreamToBuffer(httpChunked(payload, httpChunk, 200), 4096, cs);
      checkEqInt(r.returned, 4096, "delivered exactly the buffer size");
      check(r.truncated, "truncation reported");
      check(r.got == payload.substr(0, 4096), "the bytes kept are the FIRST 4096, uncorrupted");
    });
  }
}

static void test_pumpConnectionClose() {
  // No Content-Length, no chunked: the body ends when the peer closes.
  std::string payload = bodyOfLength(3000);
  std::string http = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n" + payload;
  forEachChunkSize("pump/close-framed", [&](size_t cs) {
    auto r = runStreamToBuffer(http, 8192, cs);
    checkEqInt(r.returned, 3000, "all bytes delivered");
    check(r.got == payload, "payload byte-exact");
  });
}

static void test_pumpHeaderMatching() {
  // A header whose VALUE contains "chunked" must not switch on chunked mode.
  // The old code tested every header line for the bare substring.
  g_currentTest = "pump/false-chunked-header";
  std::string payload = bodyOfLength(500);
  char hdr[320];
  snprintf(hdr, sizeof hdr,
           "HTTP/1.1 200 OK\r\n"
           "ETag: \"chunked-cafe1234\"\r\n"
           "Content-Disposition: attachment; filename=\"chunked.adf\"\r\n"
           "Content-Length: %zu\r\n\r\n", payload.size());
  auto r = runStreamToBuffer(std::string(hdr) + payload, 8192, 64);
  checkEqInt(r.returned, 500, "treated as Content-Length, not chunked");
  check(r.got == payload, "payload byte-exact");
}

static void test_pumpChunkedWinsOverContentLength() {
  // RFC 9112: when both framings are advertised, chunked wins.
  g_currentTest = "pump/both-framings";
  std::string payload = bodyOfLength(2000);
  std::string chunkedBody;
  for (size_t i = 0; i < payload.size(); i += 500) {
    size_t n = std::min((size_t)500, payload.size() - i);
    char sz[32]; snprintf(sz, sizeof sz, "%zx\r\n", n);
    chunkedBody += sz; chunkedBody += payload.substr(i, n); chunkedBody += "\r\n";
  }
  chunkedBody += "0\r\n\r\n";
  std::string http = "HTTP/1.1 200 OK\r\nContent-Length: 99\r\n"
                     "Transfer-Encoding: chunked\r\n\r\n" + chunkedBody;
  auto r = runStreamToBuffer(http, 8192, 128);
  checkEqInt(r.returned, 2000, "used chunked framing, ignored the bogus length");
  check(r.got == payload, "payload byte-exact");
}

// ── main ──────────────────────────────────────────────────────────────────

int main() {
  printf("WebDAV streaming parser tests\n");
  printf("=============================\n");

  test_scanTagUnitCases();
  test_basicListing();
  test_chunkedFraming();
  test_namespaceVariants();
  test_entityDecoding();
  test_hrefFallbackWhenNoDisplayName();
  test_fileClassification();
  test_multiSystemExtensions();
  test_emptyCollection();
  test_oversizedBlockResync();
  test_truncatedStream();
  test_httpError();

  test_pumpContentLength();
  test_pumpChunked();
  test_pumpChunkedIntoSmallBuffer();
  test_pumpConnectionClose();
  test_pumpHeaderMatching();
  test_pumpChunkedWinsOverContentLength();

  test_largeListingStress();

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
