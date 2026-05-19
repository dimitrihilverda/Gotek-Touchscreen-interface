// ============================================================================
// ui_keyboard.h — on-screen QWERTY keyboard modal
//
// Single entry point: promptText(title, initial, masked, maxLen).
// Blocks until the user taps OK or Cancel. Returns the entered text on OK, or
// an empty String AND sets `*cancelled` to true on Cancel.
//
// Designed for the JC3248 480×320; gracefully scales on the Waveshare 320×240.
// Depends on gfx_*, touchRead, hitBtn (from main .ino) and the primitives in
// ui_common.h.
// ============================================================================
#pragma once

// Layout planes — three are switched between by Shift and Symbols.
//   plane 0 = lowercase, plane 1 = uppercase (shift), plane 2 = symbols.
// Each plane is a 4-row layout, rows 0..3, each row terminated by a '\0'.
// Row 0 = 10 keys (number/symbol row), rows 1..3 = letter rows.
static const char *KB_PLANE[3][4] = {
  // lowercase
  { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm.,?" },
  // uppercase
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM.,?" },
  // symbols
  { "!@#$%^&*()", "-_=+[]{}\\:", ";'\"`~<>?/|",  "        .," }
};

// Geometry — recomputed per call so it adapts to gW/gH.
struct KbGeom {
  int kbY;          // top of keyboard
  int kbH;          // total keyboard height
  int rowH;         // per-row height (including gap)
  int keyW;         // per-key width
  int keyH;         // per-key height
  int keyGap;       // horizontal gap between keys
  int textY;        // text field top
  int textH;        // text field height
  int actionRowY;   // y of the action row (shift / sym / space / backspace / OK / cancel)
};

static KbGeom kbComputeGeom() {
  KbGeom g{};
  g.keyGap = 4;
  g.keyH   = (gH >= 320) ? 36 : 26;
  g.rowH   = g.keyH + g.keyGap;
  // 10 keys + 9 gaps fit in gW - 16 horizontal margin
  g.keyW = (gW - 16 - 9 * g.keyGap) / 10;
  // 4 letter/number rows + 1 action row = 5 * rowH
  g.kbH  = 5 * g.rowH;
  g.kbY  = gH - g.kbH - 6;
  // Text field above keyboard
  g.textH    = (gH >= 320) ? 36 : 28;
  g.textY    = g.kbY - g.textH - 8;
  g.actionRowY = g.kbY + 4 * g.rowH;
  return g;
}

// Find which key was tapped on a letter/number row.
// Returns the character, or '\0' if outside any key.
static char kbHitLetterKey(uint16_t px, uint16_t py, const KbGeom &g, int plane) {
  for (int row = 0; row < 4; ++row) {
    int ry = g.kbY + row * g.rowH;
    if (py < ry || py >= ry + g.keyH) continue;
    const char *keys = KB_PLANE[plane][row];
    int n = (int)strlen(keys);
    // Letter rows are centred under the digit row.
    int rowW   = n * g.keyW + (n - 1) * g.keyGap;
    int startX = (gW - rowW) / 2;
    for (int k = 0; k < n; ++k) {
      int kx = startX + k * (g.keyW + g.keyGap);
      if (px >= kx && px < kx + g.keyW) return keys[k];
    }
  }
  return '\0';
}

// Action-row layout: SHIFT  SYM  [space ×3]  BACKSPACE  OK  CANCEL
// 6 buttons, 5 gaps, 8 slot-widths total (space spans 3 slots).
// Returns one of: 0 (none), 1 (shift), 2 (sym), 3 (space), 4 (backspace),
// 5 (ok), 6 (cancel), 7 (eye toggle on the text field).
static int kbHitAction(uint16_t px, uint16_t py, const KbGeom &g) {
  // Eye toggle — 30 px square on the right edge of the text field.
  int tfX  = 8;
  int tfW  = gW - 16;
  int eyeX = tfX + tfW - 30;
  if (py >= g.textY && py < g.textY + g.textH &&
      px >= eyeX && px < eyeX + 30) return 7;

  if (py < g.actionRowY || py >= g.actionRowY + g.keyH) return 0;

  int slotW = (gW - 16 - 5 * g.keyGap) / 8;
  int x = 8;
  if (px >= x && px < x + slotW) return 1;                  x += slotW + g.keyGap;        // shift
  if (px >= x && px < x + slotW) return 2;                  x += slotW + g.keyGap;        // sym
  int spaceW = 3 * slotW + 2 * g.keyGap;
  if (px >= x && px < x + spaceW) return 3;                 x += spaceW + g.keyGap;       // space
  if (px >= x && px < x + slotW) return 4;                  x += slotW + g.keyGap;        // backspace
  if (px >= x && px < x + slotW) return 5;                  x += slotW + g.keyGap;        // ok
  if (px >= x && px < x + slotW) return 6;                                                 // cancel
  return 0;
}

static void kbDrawKey(int x, int y, int w, int h, const String &label, bool down) {
  uint16_t bg     = down ? TFT_CYAN : 0x18C3;
  uint16_t border = down ? TFT_WHITE : 0x4208;
  uint16_t fg     = down ? TFT_BLACK : TFT_WHITE;
  gfx_fillRect(x, y, w, h, bg);
  gfx_drawRect(x, y, w, h, border);
  gfx_setTextSize(2);
  gfx_setTextColor(fg, bg);
  int tw = gfx_textWidth(label);
  int th = gfx_fontHeight();
  if (tw > w - 6) {
    gfx_setTextSize(1);
    tw = gfx_textWidth(label);
    th = gfx_fontHeight();
  }
  gfx_setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  gfx_print(label);
}

static void kbDraw(const KbGeom &g, const String &title, const String &text,
                   bool masked, bool revealed, int plane) {
  // Background — full screen wash so the keyboard takes focus
  gfx_fillScreen(TFT_BLACK);

  // Title at top
  gfx_setTextSize(2);
  gfx_setTextColor(TFT_CYAN, TFT_BLACK);
  int tw = gfx_textWidth(title);
  gfx_setCursor((gW - tw) / 2, 8);
  gfx_print(title);
  gfx_fillRect(10, 28, gW - 20, 1, WB_MED_GREY);

  // Text field with eye toggle on the right
  int tfX = 8, tfW = gW - 16;
  drawTextField(tfX, g.textY, tfW - 32, g.textH, text,
                masked && !revealed, /*revealLast*/ false, /*focused*/ true);
  // Eye button area
  int eyeX = tfX + tfW - 30;
  gfx_fillRect(eyeX, g.textY, 30, g.textH, 0x0841);
  gfx_drawRect(eyeX, g.textY, 30, g.textH, WB_MED_GREY);
  drawEyeIcon(eyeX + 7, g.textY + (g.textH - 10) / 2, revealed, TFT_CYAN);

  // Letter / number rows
  for (int row = 0; row < 4; ++row) {
    int ry = g.kbY + row * g.rowH;
    const char *keys = KB_PLANE[plane][row];
    int n = (int)strlen(keys);
    int rowW   = n * g.keyW + (n - 1) * g.keyGap;
    int startX = (gW - rowW) / 2;
    for (int k = 0; k < n; ++k) {
      int kx = startX + k * (g.keyW + g.keyGap);
      char c[2] = {keys[k], 0};
      kbDrawKey(kx, ry, g.keyW, g.keyH, String(c), false);
    }
  }

  // Action row: shift, sym, space, backspace, OK, cancel
  int slotW = (gW - 16 - 5 * g.keyGap) / 8;
  int x = 8;
  kbDrawKey(x, g.actionRowY, slotW, g.keyH, plane == 1 ? "\x18" : "shf", plane == 1); x += slotW + g.keyGap;
  kbDrawKey(x, g.actionRowY, slotW, g.keyH, plane == 2 ? "abc" : "sym", plane == 2); x += slotW + g.keyGap;
  kbDrawKey(x, g.actionRowY, 3 * slotW + 2 * g.keyGap, g.keyH, "space", false);
  x += 3 * slotW + 2 * g.keyGap + g.keyGap;
  kbDrawKey(x, g.actionRowY, slotW, g.keyH, "del", false); x += slotW + g.keyGap;
  // OK in green-ish
  gfx_fillRect(x, g.actionRowY, slotW, g.keyH, 0x03E0);
  gfx_drawRect(x, g.actionRowY, slotW, g.keyH, TFT_WHITE);
  gfx_setTextSize(2); gfx_setTextColor(TFT_BLACK, 0x03E0);
  { int tw2 = gfx_textWidth("OK"); gfx_setCursor(x + (slotW - tw2) / 2, g.actionRowY + (g.keyH - 16) / 2); gfx_print("OK"); }
  x += slotW + g.keyGap;
  // Cancel in red-ish
  gfx_fillRect(x, g.actionRowY, slotW, g.keyH, 0x8800);
  gfx_drawRect(x, g.actionRowY, slotW, g.keyH, TFT_WHITE);
  gfx_setTextSize(2); gfx_setTextColor(TFT_WHITE, 0x8800);
  { int tw3 = gfx_textWidth("X"); gfx_setCursor(x + (slotW - tw3) / 2, g.actionRowY + (g.keyH - 16) / 2); gfx_print("X"); }

  gfx_flush();
}

// Main entry. Blocks until the user taps OK or Cancel.
//   title      label across the top
//   initial    pre-filled text
//   masked     render with '*' (toggleable on screen)
//   maxLen     cap on input length
//   cancelled  optional output flag set true if user pressed Cancel
inline String promptText(const String &title,
                         const String &initial,
                         bool masked,
                         int maxLen = 63,
                         bool *cancelled = nullptr) {
  KbGeom g = kbComputeGeom();
  String text = initial;
  int plane = 0;          // 0=lower, 1=upper, 2=symbols
  bool revealed = !masked;
  bool sticky_shift = false;

  unsigned long lastBackspace = 0;
  bool holdingBackspace = false;
  unsigned long holdStart = 0;

  // Initial draw
  kbDraw(g, title, text, masked, revealed, plane);
  unsigned long lastCaretBlink = millis();

  while (true) {
    uint16_t px, py;
    bool touched = touchRead(&px, &py);

    if (touched) {
      // Letter / number key?
      char ch = kbHitLetterKey(px, py, g, plane);
      if (ch != '\0') {
        if ((int)text.length() < maxLen) {
          text += ch;
        }
        // After typing a letter, drop one-shot shift back to lowercase
        if (plane == 1 && !sticky_shift) plane = 0;
        kbDraw(g, title, text, masked, revealed, plane);
        // Wait for release before accepting next key
        waitForRelease();
        continue;
      }

      int act = kbHitAction(px, py, g);
      if (act == 1) {
        // SHIFT — toggles between lower and upper
        plane = (plane == 1) ? 0 : 1;
        sticky_shift = (plane == 1);     // could be made double-tap-sticky later
        kbDraw(g, title, text, masked, revealed, plane);
        waitForRelease();
      } else if (act == 2) {
        // SYMBOLS toggle
        plane = (plane == 2) ? 0 : 2;
        sticky_shift = false;
        kbDraw(g, title, text, masked, revealed, plane);
        waitForRelease();
      } else if (act == 3) {
        // SPACE
        if ((int)text.length() < maxLen) text += ' ';
        kbDraw(g, title, text, masked, revealed, plane);
        waitForRelease();
      } else if (act == 4) {
        // BACKSPACE — supports hold-to-repeat
        if (!holdingBackspace) {
          holdingBackspace = true;
          holdStart = millis();
          lastBackspace = 0;
        }
        unsigned long now = millis();
        unsigned long heldFor = now - holdStart;
        unsigned long interval = heldFor < 600 ? 250 : 60;   // accelerate after 0.6s
        if (now - lastBackspace >= interval) {
          if (text.length() > 0) text.remove(text.length() - 1);
          lastBackspace = now;
          kbDraw(g, title, text, masked, revealed, plane);
        }
        // Don't waitForRelease — let the loop continue polling so we can repeat
        delay(20);
      } else if (act == 5) {
        // OK
        if (cancelled) *cancelled = false;
        waitForRelease();
        return text;
      } else if (act == 6) {
        // CANCEL
        if (cancelled) *cancelled = true;
        waitForRelease();
        return String();
      } else if (act == 7) {
        // Eye toggle
        revealed = !revealed;
        kbDraw(g, title, text, masked, revealed, plane);
        waitForRelease();
      }
    } else {
      holdingBackspace = false;
      // Periodic caret blink redraw
      if (millis() - lastCaretBlink >= 500) {
        kbDraw(g, title, text, masked, revealed, plane);
        lastCaretBlink = millis();
      }
      delay(16);
    }
  }
}
