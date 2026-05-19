// ============================================================================
// thumb_cache.h — two-layer cover-art thumbnail cache
//
// Layer 1: PSRAM LRU of decoded 46×46 RGB565 thumbnails. Instant during scroll.
// Layer 2: SD-card cache (/THUMB_CACHE/<hash>.bin, 4232 bytes each). Skips the
//          expensive JPEG/PNG decode on subsequent boots.
// Layer 3 (miss): falls back to the existing decode path (gfx_drawJpgFile /
//          drawPngFile), then captures the rendered framebuffer pixels into
//          both caches for next time.
//
// Entry point: drawThumb(x, y, sourcePath) — drop-in replacement for the
// existing gfx_drawJpgFile / drawPngFile calls when rendering list thumbnails.
//
// Notes:
// - Capture-from-framebuffer (fb_getPixel) is only available on JC3248. On
//   Waveshare we fall through to the existing draw path with no caching.
// - Cache invalidation: not automatic. Replacing a cover image keeps the old
//   thumbnail until the user taps the Clear-cache button in System Info.
// ============================================================================
#pragma once

#define THUMB_W 46
#define THUMB_H 46
#define THUMB_BYTES (THUMB_W * THUMB_H * 2)
#define THUMB_LRU_CAP 100
#define THUMB_CACHE_DIR "/THUMB_CACHE"

#if ACTIVE_DISPLAY == DISPLAY_JC3248
// fb_getPixel is defined in the main .ino but uses internal globals — forward
// declare so this header compiles before its body is parsed.
extern uint16_t fb_getPixel(int vx, int vy);
#endif

// FNV-1a 32-bit hash of a String. Good enough for path-based addressing of
// thumbnails; collisions are statistically negligible at our scale.
static uint32_t tcHashPath(const String &s) {
  uint32_t h = 2166136261u;
  for (unsigned int i = 0; i < s.length(); ++i) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

static String tcHashFile(const String &path) {
  char buf[16];
  snprintf(buf, sizeof(buf), "/%08lx.bin", (unsigned long)tcHashPath(path));
  return String(THUMB_CACHE_DIR) + String(buf);
}

// LRU slot — pixels are allocated lazily in PSRAM the first time we fill it.
struct ThumbSlot {
  uint32_t  pathHash;
  uint32_t  lastUsed;
  uint16_t *pixels;     // 46*46 RGB565, native byte order
  bool      valid;
};
static ThumbSlot g_tcLru[THUMB_LRU_CAP];
static bool g_tcInit = false;
static uint32_t g_tcTick = 0;
static uint32_t g_tcHits = 0, g_tcSdHits = 0, g_tcDecodes = 0;

static void tcInitIfNeeded() {
  if (g_tcInit) return;
  for (int i = 0; i < THUMB_LRU_CAP; ++i) {
    g_tcLru[i].pathHash = 0;
    g_tcLru[i].lastUsed = 0;
    g_tcLru[i].pixels   = nullptr;
    g_tcLru[i].valid    = false;
  }
  // Ensure the on-SD cache folder exists
  if (!SD_MMC.exists(THUMB_CACHE_DIR)) {
    SD_MMC.mkdir(THUMB_CACHE_DIR);
  }
  g_tcInit = true;
}

// Find a slot for the given hash. Returns the existing slot if hit, or the
// least-recently-used slot for eviction otherwise. allocates pixels[] lazily.
static ThumbSlot *tcAcquireSlot(uint32_t hash) {
  tcInitIfNeeded();
  ThumbSlot *oldest = &g_tcLru[0];
  for (int i = 0; i < THUMB_LRU_CAP; ++i) {
    ThumbSlot *s = &g_tcLru[i];
    if (s->valid && s->pathHash == hash) return s;
    if (s->lastUsed < oldest->lastUsed) oldest = s;
  }
  // Evict and reuse `oldest`. Allocate pixels[] on first use of this slot.
  if (!oldest->pixels) {
    oldest->pixels = (uint16_t *)ps_malloc(THUMB_BYTES);
  }
  oldest->pathHash = hash;
  oldest->valid    = false;     // not filled yet
  return oldest;
}

// Read the 4232-byte thumbnail file from /THUMB_CACHE/ into the slot pixels.
static bool tcLoadFromSD(const String &sourcePath, ThumbSlot *slot) {
  if (!slot || !slot->pixels) return false;
  String f = tcHashFile(sourcePath);
  File h = SD_MMC.open(f.c_str(), "r");
  if (!h) return false;
  if (h.size() != THUMB_BYTES) { h.close(); return false; }
  size_t n = h.read((uint8_t *)slot->pixels, THUMB_BYTES);
  h.close();
  return n == THUMB_BYTES;
}

static bool tcSaveToSD(const String &sourcePath, const uint16_t *pixels) {
  String f = tcHashFile(sourcePath);
  File h = SD_MMC.open(f.c_str(), "w");
  if (!h) return false;
  size_t n = h.write((const uint8_t *)pixels, THUMB_BYTES);
  h.close();
  return n == THUMB_BYTES;
}

// Blit a cached 46×46 RGB565 thumbnail to the framebuffer at (x, y).
// Iterates virtually — each gfx_drawPixel handles the active rotation /
// display backend so this works on JC3248 and Waveshare alike.
static void tcBlit(int x, int y, const uint16_t *pixels) {
  for (int yy = 0; yy < THUMB_H; ++yy) {
    for (int xx = 0; xx < THUMB_W; ++xx) {
      gfx_drawPixel(x + xx, y + yy, pixels[yy * THUMB_W + xx]);
    }
  }
}

// Capture the just-rendered 46×46 area from the framebuffer back into a slot.
// Only works on JC3248 (Waveshare writes directly to the LCD, no readback).
static bool tcCaptureFromFB(int x, int y, uint16_t *outPixels) {
#if ACTIVE_DISPLAY == DISPLAY_JC3248
  for (int yy = 0; yy < THUMB_H; ++yy) {
    for (int xx = 0; xx < THUMB_W; ++xx) {
      outPixels[yy * THUMB_W + xx] = fb_getPixel(x + xx, y + yy);
    }
  }
  return true;
#else
  (void)x; (void)y; (void)outPixels;
  return false;
#endif
}

// Existing decode helpers (defined earlier in the main .ino).
extern void gfx_drawJpgFile(fs::FS &fs, const char *path, int x, int y, int maxW, int maxH);
extern bool drawPngFile(const char *path, int x, int y);

static bool tcIsJpeg(const String &p) {
  String low = p;
  low.toLowerCase();
  return low.endsWith(".jpg") || low.endsWith(".jpeg");
}
static bool tcIsPng(const String &p) {
  String low = p;
  low.toLowerCase();
  return low.endsWith(".png");
}

// Entry point — render a 46×46 thumbnail for `sourcePath` at (x, y), using
// LRU → SD cache → decode in that order. Returns true if anything was drawn.
inline bool drawThumb(int x, int y, const String &sourcePath) {
  if (sourcePath.length() == 0) return false;
  tcInitIfNeeded();
  uint32_t hash = tcHashPath(sourcePath);
  uint32_t now  = ++g_tcTick;

  // L1: PSRAM LRU
  for (int i = 0; i < THUMB_LRU_CAP; ++i) {
    ThumbSlot *s = &g_tcLru[i];
    if (s->valid && s->pathHash == hash) {
      s->lastUsed = now;
      g_tcHits++;
      tcBlit(x, y, s->pixels);
      return true;
    }
  }

  // L2: SD cache
  ThumbSlot *slot = tcAcquireSlot(hash);
  if (slot->pixels && tcLoadFromSD(sourcePath, slot)) {
    slot->valid    = true;
    slot->lastUsed = now;
    g_tcSdHits++;
    tcBlit(x, y, slot->pixels);
    return true;
  }

  // L3: decode source + capture + persist.
  // Clear the 46×46 area to black first so any aspect-ratio padding around the
  // cover is captured as black, not as whatever (e.g. row highlight) was
  // already underneath. Keeps the cached thumbnail consistent across renders.
  g_tcDecodes++;
  gfx_fillRect(x, y, THUMB_W, THUMB_H, TFT_BLACK);
  if (tcIsJpeg(sourcePath)) {
    gfx_drawJpgFile(SD_MMC, sourcePath.c_str(), x, y, THUMB_W, THUMB_H);
  } else if (tcIsPng(sourcePath)) {
    drawPngFile(sourcePath.c_str(), x, y);
  } else {
    return false;
  }

  // Capture the just-rendered pixels and persist them for next time.
  if (slot->pixels && tcCaptureFromFB(x, y, slot->pixels)) {
    slot->valid    = true;
    slot->lastUsed = now;
    tcSaveToSD(sourcePath, slot->pixels);
  }
  return true;
}

// Wipe both caches — exposed so a "Clear cache" button on System Info can call it.
inline void clearThumbCache() {
  // L1: invalidate all slots (keep PSRAM allocations for reuse)
  tcInitIfNeeded();
  for (int i = 0; i < THUMB_LRU_CAP; ++i) {
    g_tcLru[i].valid    = false;
    g_tcLru[i].pathHash = 0;
    g_tcLru[i].lastUsed = 0;
  }
  // L2: remove every file in /THUMB_CACHE/
  File dir = SD_MMC.open(THUMB_CACHE_DIR);
  if (!dir) return;
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    String n = f.name();
    f.close();
    // openNextFile may return either basename or full path depending on core;
    // try the full path version first, then the basename.
    if (!SD_MMC.remove(n.c_str())) {
      String full = String(THUMB_CACHE_DIR) + "/" + n;
      SD_MMC.remove(full.c_str());
    }
  }
  dir.close();
  g_tcHits = g_tcSdHits = g_tcDecodes = 0;
}

// Optional: stats for diagnostics / System Info display
struct ThumbCacheStats {
  uint32_t hits;
  uint32_t sdHits;
  uint32_t decodes;
  int      filled;
};
inline ThumbCacheStats thumbCacheStats() {
  ThumbCacheStats s{g_tcHits, g_tcSdHits, g_tcDecodes, 0};
  for (int i = 0; i < THUMB_LRU_CAP; ++i) if (g_tcLru[i].valid) s.filled++;
  return s;
}
