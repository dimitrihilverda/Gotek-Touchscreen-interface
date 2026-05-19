// ============================================================================
// wifi_setup.h — on-device WiFi onboarding (scan + soft keyboard + connect)
//
// Entry point: runWifiSetup()  — blocking; takes over the screen until the
// user backs out or a successful STA connection is established. Returns when
// done. Persists chosen credentials into cfg_wifi_client_* + CONFIG.TXT and
// starts mDNS (gotek.local) after a successful connect.
//
// Depends on:
//   - gfx_*, touchRead, waitForRelease, hitBtn (main .ino)
//   - cfg_wifi_*, wifi_sta_connected, wifi_sta_ip, wifi_ap_active (globals)
//   - saveConfig() (main .ino)
//   - drawListItem, drawSignalBars, drawLockIcon, drawModalFrame (ui_common.h)
//   - promptText (ui_keyboard.h)
//
// Include AFTER ui_common.h and ui_keyboard.h.
// ============================================================================
#pragma once

// Cached scan result so we can redraw without re-scanning every frame.
struct WifiScanEntry {
  String  ssid;
  int     rssi;
  uint8_t enc;        // WIFI_AUTH_OPEN = 0; > 0 = locked
};
static std::vector<WifiScanEntry> g_scan_results;
static int g_scan_scroll = 0;       // top item index of the visible window

static const char *MDNS_HOSTNAME = "gotek";

// ── Helpers ──────────────────────────────────────────────────────────────────

// Sort scan results by RSSI desc, dedupe by SSID (strongest wins).
static void wifiScanSortDedupe() {
  std::sort(g_scan_results.begin(), g_scan_results.end(),
            [](const WifiScanEntry &a, const WifiScanEntry &b) { return a.rssi > b.rssi; });
  std::vector<WifiScanEntry> deduped;
  for (auto &e : g_scan_results) {
    if (e.ssid.length() == 0) continue;
    bool seen = false;
    for (auto &d : deduped) if (d.ssid == e.ssid) { seen = true; break; }
    if (!seen) deduped.push_back(e);
  }
  g_scan_results = deduped;
}

// Bring mDNS up — safe to call multiple times.
static void wifiStartMDNS() {
  MDNS.end();
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.print("mDNS started: http://");
    Serial.print(MDNS_HOSTNAME);
    Serial.println(".local/");
  } else {
    Serial.println("mDNS failed to start");
  }
}

// ── Screens ──────────────────────────────────────────────────────────────────

static void wifiDrawHeader(const char *title) {
  gfx_fillScreen(TFT_BLACK);
  gfx_setTextSize(2);
  gfx_setTextColor(TFT_CYAN, TFT_BLACK);
  gfx_setCursor(10, 8);
  gfx_print(title);
  gfx_fillRect(10, 28, gW - 20, 1, WB_MED_GREY);
}

// Scan results list — uses drawListItem for each row.
static void wifiDrawScanList() {
  wifiDrawHeader("Wi-Fi Setup");

  // [SCAN] button top-right, [BACK] far right
  int btnW = 80, btnH = 28;
  int scanX = gW - 2 * btnW - 18;
  int backX = gW - btnW - 10;
  gfx_fillRect(scanX, 4, btnW, btnH, 0x10A2);
  gfx_drawRect(scanX, 4, btnW, btnH, TFT_CYAN);
  gfx_setTextSize(2); gfx_setTextColor(TFT_CYAN, 0x10A2);
  { int tw = gfx_textWidth("Scan"); gfx_setCursor(scanX + (btnW - tw) / 2, 10); gfx_print("Scan"); }

  gfx_fillRect(backX, 4, btnW, btnH, 0x4208);
  gfx_drawRect(backX, 4, btnW, btnH, WB_MED_GREY);
  gfx_setTextColor(TFT_WHITE, 0x4208);
  { int tw = gfx_textWidth("Back"); gfx_setCursor(backX + (btnW - tw) / 2, 10); gfx_print("Back"); }

  // List area
  int listTop = 40;
  int listBot = gH - 8;
  int rowH    = 44;
  int visible = (listBot - listTop) / rowH;

  if (g_scan_results.empty()) {
    gfx_setTextSize(2);
    gfx_setTextColor(WB_MED_GREY, TFT_BLACK);
    String msg = "Tap Scan to search for networks";
    int tw = gfx_textWidth(msg);
    gfx_setCursor((gW - tw) / 2, listTop + 40);
    gfx_print(msg);
    gfx_flush();
    return;
  }

  if (g_scan_scroll < 0) g_scan_scroll = 0;
  if (g_scan_scroll > (int)g_scan_results.size() - visible)
    g_scan_scroll = std::max(0, (int)g_scan_results.size() - visible);

  for (int i = 0; i < visible && (g_scan_scroll + i) < (int)g_scan_results.size(); ++i) {
    auto &e = g_scan_results[g_scan_scroll + i];
    int y = listTop + i * rowH;
    bool isSaved = (e.ssid == cfg_wifi_client_ssid && cfg_wifi_client_ssid.length() > 0);
    String secondary = String(e.rssi) + " dBm";
    if (isSaved) secondary += "  •  saved";
    if (e.enc == WIFI_AUTH_OPEN) secondary += "  •  open";

    drawListItem(8, y, gW - 16, rowH - 4, e.ssid, secondary,
                 /*highlighted*/ isSaved, /*selected*/ false);

    // Signal bars + lock icon on the right side of the row
    drawSignalBars(gW - 8 - 16 - 4, y + (rowH - 12) / 2, e.rssi);
    if (e.enc != WIFI_AUTH_OPEN) {
      drawLockIcon(gW - 8 - 16 - 4 - 18, y + (rowH - 12) / 2, WB_ORANGE);
    }
  }

  // Scroll hint at the bottom if there are more
  if ((int)g_scan_results.size() > visible) {
    gfx_setTextSize(1);
    gfx_setTextColor(WB_MED_GREY, TFT_BLACK);
    String hint = String(g_scan_scroll + 1) + ".." + String(std::min((int)g_scan_results.size(), g_scan_scroll + visible)) +
                  "  /  " + String(g_scan_results.size()) + "  (swipe to scroll)";
    int tw = gfx_textWidth(hint);
    gfx_setCursor((gW - tw) / 2, listBot - 10);
    gfx_print(hint);
  }

  gfx_flush();
}

// Status screen during connect attempt.
static void wifiDrawConnecting(const String &ssid) {
  wifiDrawHeader("Wi-Fi Setup");
  int boxW = 280, boxH = 110;
  int bx = (gW - boxW) / 2, by = (gH - boxH) / 2;
  drawModalFrame(bx, by, boxW, boxH, "Connecting", TFT_CYAN);
  gfx_setTextSize(2);
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  String s1 = ssid.length() > 22 ? ssid.substring(0, 21) + "~" : ssid;
  int tw = gfx_textWidth(s1);
  gfx_setCursor(bx + (boxW - tw) / 2, by + 44);
  gfx_print(s1);
  gfx_setTextSize(1);
  gfx_setTextColor(WB_MED_GREY, TFT_BLACK);
  const char *m = "this may take up to 15 seconds";
  int mw = gfx_textWidth(m);
  gfx_setCursor(bx + (boxW - mw) / 2, by + boxH - 16);
  gfx_print(m);
  gfx_flush();
}

// Success screen. Shows IP + hostname; one Done button to dismiss.
// Layout: rows have explicit spacing; Done lives in its own row at the bottom
// so it never overlaps the LAN / mDNS URLs.
static void wifiDrawSuccess(const String &ssid, const String &ip) {
  wifiDrawHeader("Wi-Fi Setup");

  // Sizing — wider modal on JC3248 leaves room for the full mDNS URL at
  // textSize 2 without truncation. Increased height adds a dedicated button row.
  int boxW = (gW >= 400) ? 420 : (gW - 20);
  int boxH = 218;
  int bx = (gW - boxW) / 2, by = (gH - boxH) / 2;
  drawModalFrame(bx, by, boxW, boxH, "Connected", 0x03E0);

  int contentX = bx + 14;
  int y = by + 36;
  const int labelH  = 12;
  const int valueH  = 20;
  const int rowGap  = 6;

  // Row 1 — Network
  gfx_setTextSize(1); gfx_setTextColor(WB_MED_GREY, TFT_BLACK);
  gfx_setCursor(contentX, y); gfx_print("Network:");
  y += labelH;
  gfx_setTextSize(2); gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  gfx_setCursor(contentX, y); gfx_print(ssid);
  y += valueH + rowGap;

  // Row 2 — LAN address
  gfx_setTextSize(1); gfx_setTextColor(WB_MED_GREY, TFT_BLACK);
  gfx_setCursor(contentX, y); gfx_print("LAN address:");
  y += labelH;
  gfx_setTextSize(2); gfx_setTextColor(TFT_CYAN, TFT_BLACK);
  gfx_setCursor(contentX, y); gfx_print("http://" + ip + "/");
  y += valueH + rowGap;

  // Row 3 — mDNS hostname
  gfx_setTextSize(1); gfx_setTextColor(WB_MED_GREY, TFT_BLACK);
  gfx_setCursor(contentX, y); gfx_print("Also reachable as:");
  y += labelH;
  gfx_setTextSize(2); gfx_setTextColor(TFT_CYAN, TFT_BLACK);
  gfx_setCursor(contentX, y); gfx_print("http://" + String(MDNS_HOSTNAME) + ".local/");

  // Done button — its own row at the bottom-right, well below the URLs.
  int btnW = 110, btnH = 30;
  int bxn = bx + boxW - btnW - 12;
  int byn = by + boxH - btnH - 10;
  gfx_fillRect(bxn, byn, btnW, btnH, 0x03E0);
  gfx_drawRect(bxn, byn, btnW, btnH, TFT_WHITE);
  gfx_setTextSize(2); gfx_setTextColor(TFT_BLACK, 0x03E0);
  { int tw = gfx_textWidth("Done"); gfx_setCursor(bxn + (btnW - tw) / 2, byn + (btnH - 16) / 2); gfx_print("Done"); }

  gfx_flush();

  // Block until tapped
  while (true) {
    uint16_t px, py;
    if (touchRead(&px, &py)) {
      if (hitBtn(px, py, bxn, byn, btnW, btnH)) { waitForRelease(); return; }
    }
    delay(20);
  }
}

// Failure screen with retry / cancel.
// Returns true if user wants to retry, false to give up and return to list.
static bool wifiDrawFailure(const String &ssid, const String &reason) {
  wifiDrawHeader("Wi-Fi Setup");
  int boxW = 320, boxH = 160;
  int bx = (gW - boxW) / 2, by = (gH - boxH) / 2;
  drawModalFrame(bx, by, boxW, boxH, "Failed", TFT_RED);

  gfx_setTextSize(2);
  gfx_setTextColor(TFT_WHITE, TFT_BLACK);
  String s1 = ssid.length() > 24 ? ssid.substring(0, 23) + "~" : ssid;
  gfx_setCursor(bx + 14, by + 40); gfx_print(s1);

  gfx_setTextSize(1);
  gfx_setTextColor(WB_MED_GREY, TFT_BLACK);
  gfx_setCursor(bx + 14, by + 66); gfx_print(reason);

  // Retry / Cancel buttons
  int btnW = 100, btnH = 32;
  int retryX  = bx + boxW - btnW - 10;
  int cancelX = retryX - btnW - 8;
  int byn     = by + boxH - btnH - 10;
  gfx_fillRect(retryX, byn, btnW, btnH, TFT_CYAN);
  gfx_drawRect(retryX, byn, btnW, btnH, TFT_WHITE);
  gfx_setTextSize(2); gfx_setTextColor(TFT_BLACK, TFT_CYAN);
  { int tw = gfx_textWidth("Retry"); gfx_setCursor(retryX + (btnW - tw) / 2, byn + (btnH - 16) / 2); gfx_print("Retry"); }

  gfx_fillRect(cancelX, byn, btnW, btnH, 0x4208);
  gfx_drawRect(cancelX, byn, btnW, btnH, WB_MED_GREY);
  gfx_setTextColor(TFT_WHITE, 0x4208);
  { int tw = gfx_textWidth("Cancel"); gfx_setCursor(cancelX + (btnW - tw) / 2, byn + (btnH - 16) / 2); gfx_print("Cancel"); }

  gfx_flush();

  while (true) {
    uint16_t px, py;
    if (touchRead(&px, &py)) {
      if (hitBtn(px, py, retryX,  byn, btnW, btnH)) { waitForRelease(); return true;  }
      if (hitBtn(px, py, cancelX, byn, btnW, btnH)) { waitForRelease(); return false; }
    }
    delay(20);
  }
}

// ── Core actions ─────────────────────────────────────────────────────────────

// Block while WiFi.scanNetworks runs (a few seconds). Updates the cached list.
static void wifiRunScan() {
  // Show "scanning..." overlay
  wifiDrawHeader("Wi-Fi Setup");
  showBusyIndicator("SCANNING...");

  // Ensure radio is up enough for an active scan
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_AP_STA);

  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  g_scan_results.clear();
  if (n > 0) {
    g_scan_results.reserve(n);
    for (int i = 0; i < n; ++i) {
      WifiScanEntry e;
      e.ssid = WiFi.SSID(i);
      e.rssi = WiFi.RSSI(i);
      e.enc  = WiFi.encryptionType(i);
      g_scan_results.push_back(e);
    }
  }
  WiFi.scanDelete();
  wifiScanSortDedupe();
  g_scan_scroll = 0;

  hideBusyIndicator();
}

// Attempt to join `ssid` with `pass`. Returns true and fills `outIp` on success.
static bool wifiAttemptConnect(const String &ssid, const String &pass,
                               String &outIp, String &outReason) {
  wifiDrawConnecting(ssid);

  // Keep AP up too — fallback if STA flakes
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (millis() - start < 15000) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      outIp = WiFi.localIP().toString();
      return true;
    }
    if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) {
      outReason = (s == WL_NO_SSID_AVAIL) ? "Network not found" : "Connect failed (wrong password?)";
      return false;
    }
    delay(200);
  }
  outReason = "Timed out after 15 seconds";
  WiFi.disconnect();
  return false;
}

// Persist a successfully-tested SSID/password.
static void wifiSaveCredentials(const String &ssid, const String &pass) {
  cfg_wifi_client_ssid    = ssid;
  cfg_wifi_client_pass    = pass;
  cfg_wifi_client_enabled = true;
  saveConfig();
}

// ── Main entry ───────────────────────────────────────────────────────────────

// Blocking modal. Returns when the user backs out or finishes a successful connect.
inline void runWifiSetup() {
  // Initial draw — empty list, prompt user to scan
  wifiDrawScanList();

  // Touch scroll state for list paging
  int touchStartY = -1;
  int scrollAtStart = 0;

  while (true) {
    uint16_t px, py;
    bool touched = touchRead(&px, &py);
    if (!touched) {
      touchStartY = -1;
      delay(16);
      continue;
    }

    // Header buttons: Scan (right of title) / Back (far right)
    int btnW = 80, btnH = 28;
    int scanX = gW - 2 * btnW - 18;
    int backX = gW - btnW - 10;
    if (hitBtn(px, py, scanX, 4, btnW, btnH)) {
      waitForRelease();
      wifiRunScan();
      wifiDrawScanList();
      continue;
    }
    if (hitBtn(px, py, backX, 4, btnW, btnH)) {
      waitForRelease();
      return;
    }

    // List rows: tap on a row → password prompt → connect
    int listTop = 40;
    int rowH    = 44;
    int visible = (gH - 8 - listTop) / rowH;
    if (py >= listTop && py < listTop + visible * rowH && !g_scan_results.empty()) {
      // Detect drag scroll: track first touch, if user moves significantly
      // before releasing, treat as scroll. Otherwise treat as tap on row.
      if (touchStartY < 0) {
        touchStartY = py;
        scrollAtStart = g_scan_scroll;
      }
      // Poll until release to distinguish tap vs drag
      unsigned long pressStart = millis();
      int lastY = py;
      bool dragged = false;
      while (true) {
        uint16_t tx, ty;
        if (!touchRead(&tx, &ty)) break;
        if (abs((int)ty - touchStartY) > 12) {
          dragged = true;
          int dy = (int)ty - touchStartY;
          int newScroll = scrollAtStart - dy / rowH;
          int maxScroll = std::max(0, (int)g_scan_results.size() - visible);
          if (newScroll < 0) newScroll = 0;
          if (newScroll > maxScroll) newScroll = maxScroll;
          if (newScroll != g_scan_scroll) {
            g_scan_scroll = newScroll;
            wifiDrawScanList();
          }
        }
        lastY = ty;
        delay(20);
        if (millis() - pressStart > 5000) break;
      }
      if (!dragged) {
        int idx = (py - listTop) / rowH + g_scan_scroll;
        if (idx >= 0 && idx < (int)g_scan_results.size()) {
          const auto e = g_scan_results[idx];
          // Open password keyboard (or skip for open networks)
          String pass;
          bool cancelled = false;
          if (e.enc != WIFI_AUTH_OPEN) {
            pass = promptText("Password for " + e.ssid, "", /*masked=*/true, /*maxLen=*/63, &cancelled);
            if (cancelled) {
              wifiDrawScanList();
              continue;
            }
          }
          // Try connecting; loop for retry
          while (true) {
            String ip, reason;
            if (wifiAttemptConnect(e.ssid, pass, ip, reason)) {
              wifiSaveCredentials(e.ssid, pass);
              wifiStartMDNS();
              wifiDrawSuccess(e.ssid, ip);
              return;       // Exit setup — back to caller
            }
            if (!wifiDrawFailure(e.ssid, reason)) {
              break;        // user cancelled
            }
            // Retry — re-prompt for password unless open network
            if (e.enc != WIFI_AUTH_OPEN) {
              pass = promptText("Password for " + e.ssid, pass, /*masked=*/true, /*maxLen=*/63, &cancelled);
              if (cancelled) break;
            }
          }
          wifiDrawScanList();
        }
      }
      touchStartY = -1;
    }
  }
}

// Hook from boot/setup() — bring mDNS up if we're already connected to STA.
inline void wifiStartMDNSIfConnected() {
  if (wifi_sta_connected) {
    wifiStartMDNS();
  }
}
