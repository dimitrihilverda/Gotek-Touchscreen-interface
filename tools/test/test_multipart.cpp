// Host tests for the multipart body scanner.
//
// This is the path a disk image travels into the device, and it carried three
// defects that only appear with binary payloads. Each has a test here so it
// cannot come back:
//
//   1. NUL bytes truncated the delimiter search (String::indexOf -> strstr).
//   2. A delimiter split across two reads was never found.
//   3. The hold-back meant to prevent (2) wrote the held bytes out anyway.
//
// The header under test is the one that ships — no copy.

#include "../../Gotek_Touchscreen/multipart_scan.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int checks = 0, failures = 0;

static void ok(bool cond, const char *what) {
  checks++;
  if (!cond) {
    failures++;
    std::printf("  FAIL: %s\n", what);
  }
}

static void eq(long got, long want, const char *what) {
  checks++;
  if (got != want) {
    failures++;
    std::printf("  FAIL: %s (got %ld, want %ld)\n", what, got, want);
  }
}

// Drives the scanner with a chosen read size, collecting what it emits.
static std::vector<uint8_t> run(const std::string &delim,
                                const std::vector<uint8_t> &wire,
                                size_t readSize,
                                bool *hitBoundary, bool *final_) {
  MultipartBody body;
  if (!body.begin(delim.c_str(), (int)delim.size())) {
    if (hitBoundary) *hitBoundary = false;
    return {};
  }
  std::vector<uint8_t> out;
  bool hit = false;
  for (size_t off = 0; off < wire.size() && !hit; off += readSize) {
    const size_t n = std::min(readSize, wire.size() - off);
    hit = body.feed(wire.data() + off, (int)n,
                    [&](const uint8_t *p, int len) {
                      out.insert(out.end(), p, p + len);
                    });
  }
  if (hitBoundary) *hitBoundary = hit;
  if (final_) *final_ = body.sawFinalDelimiter();
  return out;
}

// payload + CRLF + delimiter + "--"
static std::vector<uint8_t> wireFor(const std::vector<uint8_t> &payload,
                                    const std::string &delim) {
  std::vector<uint8_t> w = payload;
  w.push_back('\r');
  w.push_back('\n');
  w.insert(w.end(), delim.begin(), delim.end());
  w.push_back('-');
  w.push_back('-');
  return w;
}

int main() {
  const std::string delim = "------WebKitFormBoundary7MA4YWxkTrZu0gW";

  // ── mpFindBytes is binary-safe ─────────────────────────────────────────
  {
    const uint8_t hay[] = { 'a', 0, 0, 0, 'X', 'Y', 'Z', 0, 'q' };
    eq(mpFindBytes(hay, sizeof(hay), "XYZ", 3), 4,
       "finds a needle sitting after NUL bytes");
    eq(mpFindBytes(hay, sizeof(hay), "nope", 4), -1, "reports a genuine miss");
    eq(mpFindBytes(hay, sizeof(hay), "", 0), -1, "empty needle is not a match");
    eq(mpFindBytes(hay, 2, "XYZ", 3), -1, "needle longer than haystack");
    // The last byte, to catch an off-by-one at the end of the window.
    const uint8_t tail[] = { 1, 2, 3, 'Z' };
    eq(mpFindBytes(tail, sizeof(tail), "Z", 1), 3, "matches the final byte");
  }

  // ── Defect 1: payload full of NUL bytes ────────────────────────────────
  // A real ADF is largely zeros. The old scan stopped at the first one, so the
  // delimiter was never seen and the file grew by the whole epilogue.
  {
    // The size matters: it must NOT be a multiple of the read size, or the
    // delimiter lands at the start of a fresh read with no NUL ahead of it and
    // the old strstr-based scan would have found it by luck. 4000 bytes with
    // 1024-byte reads puts trailing zeros and the delimiter in the same read,
    // which is what a real ADF looks like.
    std::vector<uint8_t> payload(4000, 0x00);
    payload[0] = 'D'; payload[1] = 'O'; payload[2] = 'S';   // an ADF starts "DOS"
    bool hit = false, fin = false;
    auto out = run(delim, wireFor(payload, delim), 1024, &hit, &fin);
    ok(hit, "delimiter found in an all-NUL payload");
    ok(fin, "final delimiter recognised");
    eq((long)out.size(), (long)payload.size(), "NUL payload emitted whole");
    ok(out == payload, "NUL payload emitted byte-for-byte");
  }

  // ── Defect 2: delimiter split across reads ─────────────────────────────
  // Every possible split, by walking the read size across the region where the
  // delimiter straddles a boundary.
  {
    std::vector<uint8_t> payload(3000);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)(i * 7);
    const auto wire = wireFor(payload, delim);

    int bad = 0;
    for (size_t rs = 1; rs <= 64; rs++) {          // tiny reads: worst case
      bool hit = false;
      auto out = run(delim, wire, rs, &hit, nullptr);
      if (!hit || out != payload) bad++;
    }
    eq(bad, 0, "correct for every read size from 1 to 64 bytes");

    bad = 0;
    for (size_t rs = 900; rs <= 1100; rs++) {      // around the window size
      bool hit = false;
      auto out = run(delim, wire, rs, &hit, nullptr);
      if (!hit || out != payload) bad++;
    }
    eq(bad, 0, "correct for every read size from 900 to 1100 bytes");
  }

  // ── Defect 3: no delimiter bytes may reach the file ────────────────────
  {
    std::vector<uint8_t> payload(2048, 0xAB);
    const auto wire = wireFor(payload, delim);
    bool hit = false;
    auto out = run(delim, wire, 512, &hit, nullptr);
    ok(hit, "boundary reached");
    eq((long)out.size(), (long)payload.size(), "emitted exactly the payload");
    ok(mpFindBytes(out.data(), (int)out.size(), delim.c_str(),
                   (int)delim.size()) < 0,
       "no delimiter bytes leaked into the output");
    ok(out.empty() || out.back() == 0xAB, "trailing CRLF stripped");
  }

  // ── Payload that merely resembles the delimiter ────────────────────────
  {
    std::string near = delim.substr(0, delim.size() - 1);   // one char short
    std::vector<uint8_t> payload(near.begin(), near.end());
    payload.push_back('!');                                  // ...and diverges
    for (int i = 0; i < 500; i++) payload.push_back(0x00);
    bool hit = false;
    auto out = run(delim, wireFor(payload, delim), 37, &hit, nullptr);
    ok(hit, "boundary still found after a near-miss prefix");
    ok(out == payload, "near-miss prefix passed through untouched");
  }

  // ── A payload of exactly zero bytes ────────────────────────────────────
  {
    std::vector<uint8_t> payload;
    bool hit = false;
    auto out = run(delim, wireFor(payload, delim), 64, &hit, nullptr);
    ok(hit, "empty part still terminates");
    eq((long)out.size(), 0, "empty part emits nothing");
  }

  // ── An over-long boundary is refused rather than overflowing ───────────
  {
    MultipartBody body;
    std::string huge(MP_MAX_DELIM + 1, 'x');
    ok(!body.begin(huge.c_str(), (int)huge.size()),
       "delimiter longer than the buffer is rejected");
    ok(!body.begin("", 0), "empty delimiter is rejected");
  }

  std::printf("%d passed, %d failed\n", checks - failures, failures);
  return failures ? 1 : 0;
}
