// ============================================================================
// button_cache.h — decoded theme-button tile cache
//
// drawThemedButton() used to hit the SD card twice for every button on every
// repaint: getPngSize() opens and parses the PNG header, then drawPngFile()
// opens and decodes the whole image again. A list screen draws four or five
// buttons, so a single scroll frame cost roughly ten SD opens and half a dozen
// PNG decodes — all producing pixels identical to the previous frame's.
//
// This caches the RESULT. The first draw goes through the normal decode path
// and then captures the rendered rectangle straight out of the framebuffer;
// every later draw is a blit from PSRAM. That deliberately mirrors
// thumb_cache.h rather than introducing a second PNG decode path, because the
// capture-and-blit approach there is already proven on this hardware.
//
// Consequences of capturing composited pixels:
//   * A button is cached together with whatever it was drawn on top of. Every
//     caller paints buttons onto the black background left by gfx_fillScreen,
//     so this is well defined — but a button drawn over artwork would cache
//     that artwork with it. Callers that need this must skip the cache.
//   * Alpha is already resolved at capture time, so no mask is stored.
//
// Capture requires framebuffer read-back, which only the JC3248 backend has;
// on Waveshare every call falls through to the original decode path exactly
// as before.
// ============================================================================
#pragma once

#define BTN_CACHE_SLOTS   12    // more than any screen draws at once
#define BTN_CACHE_MAX_W  200
#define BTN_CACHE_MAX_H   64

#if ACTIVE_DISPLAY == DISPLAY_JC3248
extern uint16_t fb_getPixel(int vx, int vy);
#endif

struct BtnSlot {
  char      key[24];    // "<pngName>|<w>x<h>" — geometry is part of identity
  int16_t   w, h;
  uint16_t *pixels;     // w*h RGB565 in PSRAM
  size_t    cap;        // BYTES actually allocated at `pixels`
  bool      valid;      // holds a captured tile
  bool      missing;    // theme has no such PNG — negative cache
};

static BtnSlot g_btnCache[BTN_CACHE_SLOTS];
static bool    g_btnCacheInit = false;
static uint32_t g_btnHits = 0, g_btnFills = 0;

static void btnCacheInit() {
  if (g_btnCacheInit) return;
  for (int i = 0; i < BTN_CACHE_SLOTS; i++) {
    g_btnCache[i].key[0]  = 0;
    g_btnCache[i].pixels  = nullptr;
    g_btnCache[i].cap     = 0;
    g_btnCache[i].valid   = false;
    g_btnCache[i].missing = false;
    g_btnCache[i].w = g_btnCache[i].h = 0;
  }
  g_btnCacheInit = true;
}

static void btnCacheMakeKey(char *out, size_t cap, const char *pngName, int w, int h) {
  snprintf(out, cap, "%s|%dx%d", pngName ? pngName : "?", w, h);
}

// Find an existing slot for this key, or claim a free one. Returns nullptr
// when the cache is full — callers then just draw uncached.
static BtnSlot *btnCacheFind(const char *key, bool claim) {
  btnCacheInit();
  for (int i = 0; i < BTN_CACHE_SLOTS; i++) {
    if (g_btnCache[i].key[0] && strcmp(g_btnCache[i].key, key) == 0) return &g_btnCache[i];
  }
  if (!claim) return nullptr;
  for (int i = 0; i < BTN_CACHE_SLOTS; i++) {
    if (!g_btnCache[i].key[0]) {
      strncpy(g_btnCache[i].key, key, sizeof(g_btnCache[i].key) - 1);
      g_btnCache[i].key[sizeof(g_btnCache[i].key) - 1] = 0;
      g_btnCache[i].valid   = false;
      g_btnCache[i].missing = false;
      return &g_btnCache[i];
    }
  }
  // Full. The working set is a handful of buttons, so this means a theme with
  // an unusual number of distinct button geometries — fall back to decoding.
  return nullptr;
}

// Blit a cached tile. Uses gfx_drawPixel so display rotation and the active
// backend are handled the same way everything else handles them.
static void btnCacheBlit(int x, int y, const BtnSlot *s) {
  for (int yy = 0; yy < s->h; yy++) {
    for (int xx = 0; xx < s->w; xx++) {
      gfx_drawPixel(x + xx, y + yy, s->pixels[yy * s->w + xx]);
    }
  }
}

// Capture a just-drawn button out of the framebuffer into the slot.
static bool btnCacheCapture(int x, int y, int w, int h, BtnSlot *s) {
#if ACTIVE_DISPLAY == DISPLAY_JC3248
  if (w <= 0 || h <= 0 || w > BTN_CACHE_MAX_W || h > BTN_CACHE_MAX_H) return false;

  // Reallocate when the slot's block is too small for this geometry.
  //
  // This was a heap overflow. The only gate here used to be `if (!s->pixels)`,
  // and nothing recorded how big the block was — while clearButtonCache
  // deliberately keeps the PSRAM pointer and only blanks the key. A slot first
  // filled by a 40x36 button (2880 B) could therefore be handed to a 148x36
  // one (10656 B) and captured straight past the end of a live allocation,
  // over the TLSF block headers that sit inline right after it. The panic then
  // lands much later, in an unrelated ps_malloc or free, with a backtrace
  // pointing at innocent code — which is exactly how it presented.
  const size_t need = (size_t)w * h * 2;
  if (!s->pixels || s->cap < need) {
    free(s->pixels);
    s->pixels = (uint16_t *)ps_malloc(need);
    if (!s->pixels) {
      s->cap = 0;
      s->valid = false;
      s->w = s->h = 0;
      return false;
    }
    s->cap = need;
  }
  for (int yy = 0; yy < h; yy++) {
    for (int xx = 0; xx < w; xx++) {
      s->pixels[yy * w + xx] = fb_getPixel(x + xx, y + yy);
    }
  }
  s->w = w; s->h = h;
  s->valid = true;
  g_btnFills++;
  return true;
#else
  (void)x; (void)y; (void)w; (void)h; (void)s;
  return false;
#endif
}

// Drop every cached tile. Must be called whenever the active theme changes,
// otherwise the old theme's buttons keep being blitted.
inline void clearButtonCache() {
  btnCacheInit();
  for (int i = 0; i < BTN_CACHE_SLOTS; i++) {
    g_btnCache[i].key[0]  = 0;
    g_btnCache[i].valid   = false;
    g_btnCache[i].missing = false;
    // Keep the PSRAM allocation for reuse; only the identity is cleared.
  }
  g_btnHits = g_btnFills = 0;
}

struct ButtonCacheStats { uint32_t hits, fills; int filled; };
inline ButtonCacheStats buttonCacheStats() {
  ButtonCacheStats st{g_btnHits, g_btnFills, 0};
  for (int i = 0; i < BTN_CACHE_SLOTS; i++) if (g_btnCache[i].valid) st.filled++;
  return st;
}
