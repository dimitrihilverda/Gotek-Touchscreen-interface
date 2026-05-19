// ============================================================================
// ui_common.h — reusable UI primitives shared by multiple screens
//
// Depends on the gfx_* API and the WB_* / TFT_* colour constants defined in the
// main .ino. Include AFTER those, near the bottom of the sketch (see webserver.h
// for the same pattern). All draw functions write to the framebuffer; the
// caller is responsible for calling gfx_flush() when ready.
// ============================================================================
#pragma once

// ── Modal frame ──────────────────────────────────────────────────────────────
// Draws a centred dark panel with an accent-coloured border, a 1 px inner
// shadow and a title bar. Returns the Y coordinate where the caller should
// start drawing content (just below the title bar).
inline int drawModalFrame(int x, int y, int w, int h, const char *title, uint16_t accent) {
  // Soft drop-shadow (4 px offset)
  gfx_fillRect(x + 4, y + 4, w, h, 0x18C3);
  // Body
  gfx_fillRect(x, y, w, h, TFT_BLACK);
  gfx_drawRect(x, y, w, h, accent);
  gfx_drawRect(x + 1, y + 1, w - 2, h - 2, 0x4208);
  // Title bar
  if (title && title[0] != '\0') {
    gfx_fillRect(x + 2, y + 2, w - 4, 22, accent);
    gfx_setTextSize(2);
    gfx_setTextColor(TFT_BLACK, accent);
    int tw = gfx_textWidth(String(title));
    gfx_setCursor(x + (w - tw) / 2, y + 6);
    gfx_print(String(title));
    return y + 28;
  }
  return y + 6;
}

// ── Signal strength bars ─────────────────────────────────────────────────────
// 4-bar vertical indicator, takes 16 px wide × 12 px high.
// rssi: dBm value from WiFi.RSSI() (typically -30 ... -100).
inline void drawSignalBars(int x, int y, int rssi) {
  // Map RSSI to 0..4 bars: -55+ = 4, -65 = 3, -75 = 2, -85 = 1, weaker = 0.
  int bars = 0;
  if      (rssi >= -55) bars = 4;
  else if (rssi >= -65) bars = 3;
  else if (rssi >= -75) bars = 2;
  else if (rssi >= -85) bars = 1;

  const int bw = 3, gap = 1;
  for (int i = 0; i < 4; ++i) {
    int bh = 3 + i * 3;                       // 3, 6, 9, 12 px tall
    int bx = x + i * (bw + gap);
    int by = y + (12 - bh);
    uint16_t col = (i < bars) ? TFT_GREEN : 0x2945;   // lit vs dim
    gfx_fillRect(bx, by, bw, bh, col);
  }
}

// ── Lock icon ────────────────────────────────────────────────────────────────
// 9×11 simple padlock. Used to indicate WPA-protected networks.
inline void drawLockIcon(int x, int y, uint16_t color) {
  // Shackle (arc approximated as 3-segment outline)
  gfx_drawRect(x + 1, y, 7, 4, color);
  gfx_fillRect(x + 2, y, 5, 2, TFT_BLACK);   // clear inside of arc top
  gfx_drawRect(x + 1, y + 2, 7, 1, color);
  // Body
  gfx_fillRect(x, y + 4, 9, 7, color);
  // Keyhole
  gfx_fillRect(x + 4, y + 6, 1, 3, TFT_BLACK);
}

// ── Text field ───────────────────────────────────────────────────────────────
// Rounded rectangle text input. When `masked` is true, characters are rendered
// as '*' (except the last one when `revealLast` to give typing feedback). The
// `focused` flag draws a brighter border + a blinking caret.
inline void drawTextField(int x, int y, int w, int h,
                          const String &text,
                          bool masked, bool revealLast,
                          bool focused) {
  uint16_t border = focused ? TFT_CYAN : WB_MED_GREY;
  gfx_fillRect(x, y, w, h, 0x0841);                  // very dark fill
  gfx_drawRect(x, y, w, h, border);
  gfx_drawRect(x + 1, y + 1, w - 2, h - 2, 0x18C3);  // inner darker

  // Build the rendered string
  String render;
  if (masked) {
    int n = text.length();
    for (int i = 0; i < n; ++i) {
      bool last = (i == n - 1) && revealLast;
      render += last ? text[i] : '*';
    }
  } else {
    render = text;
  }

  gfx_setTextSize(2);
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  // Right-align if it would overflow on the left so the user sees what
  // they're typing — common pattern on phones.
  int textW = gfx_textWidth(render);
  int padX = 8, padY = (h - 16) / 2;
  int tx = x + padX;
  int avail = w - 2 * padX;
  if (textW > avail) {
    tx = x + w - padX - textW;       // scrolled to show the right end
  }
  // Clip by drawing a black rect over any text that would land outside the box
  // (simple approach: cap render length to fit avail)
  while (textW > avail + 12 && render.length() > 0) {
    render = render.substring(1);
    textW = gfx_textWidth(render);
    tx = x + w - padX - textW;
  }
  gfx_setCursor(tx, y + padY);
  gfx_print(render);

  // Caret (blink ~2 Hz off the wall clock)
  if (focused && ((millis() / 500) & 1) == 0) {
    int cx = tx + textW + 1;
    if (cx > x + w - 6) cx = x + w - 6;
    gfx_fillRect(cx, y + 6, 2, h - 12, TFT_CYAN);
  }
}

// ── Eye icon (show/hide password) ────────────────────────────────────────────
// 16×10 simple eye. `open` switches between an outline + pupil and a closed slit.
inline void drawEyeIcon(int x, int y, bool open, uint16_t color) {
  if (open) {
    // Eye outline (oval-ish)
    gfx_drawRect(x + 2, y + 2, 12, 6, color);
    gfx_drawPixel(x + 1, y + 4, color); gfx_drawPixel(x + 1, y + 5, color);
    gfx_drawPixel(x + 14, y + 4, color); gfx_drawPixel(x + 14, y + 5, color);
    // Pupil
    gfx_fillRect(x + 7, y + 4, 2, 2, color);
  } else {
    // Closed slit
    gfx_fillRect(x + 2, y + 5, 12, 1, color);
    gfx_drawPixel(x + 1, y + 5, color);
    gfx_drawPixel(x + 14, y + 5, color);
  }
}

// ── Generic list item ────────────────────────────────────────────────────────
// Renders one row in a list with optional left/right indicators. Used by the
// WiFi scan list — designed to be reusable for any "tappable row" UI.
//
//   primary      main label (left-aligned)
//   secondary    optional smaller label below primary (or empty)
//   highlighted  draw with a subtle highlight bg (e.g. currently saved SSID)
//   selected     draw with a strong selection bg (e.g. last tapped)
inline void drawListItem(int x, int y, int w, int h,
                         const String &primary,
                         const String &secondary,
                         bool highlighted,
                         bool selected) {
  uint16_t bg = TFT_BLACK;
  if (selected)        bg = 0x10A2;          // dark cyan
  else if (highlighted) bg = 0x0841;         // very subtle

  gfx_fillRect(x, y, w, h, bg);
  gfx_drawRect(x, y, w, h - 1, 0x18C3);      // hairline separator

  int textX = x + 8;
  gfx_setTextSize(2);
  gfx_setTextColor(selected ? TFT_CYAN : TFT_WHITE, bg);
  gfx_setCursor(textX, y + (secondary.length() > 0 ? 4 : (h - 16) / 2));
  gfx_print(primary);

  if (secondary.length() > 0) {
    gfx_setTextSize(1);
    gfx_setTextColor(WB_MED_GREY, bg);
    gfx_setCursor(textX, y + h - 12);
    gfx_print(secondary);
  }
}

// ── Tap inside helper ────────────────────────────────────────────────────────
// Same as hitBtn but accepts uint16_t — exposed so other UI modules don't have
// to redeclare. (hitBtn lives in the main .ino.)
extern bool hitBtn(uint16_t px, uint16_t py, int bx, int by, int bw, int bh);
