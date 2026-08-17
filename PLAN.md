# Release ritual

Before tagging, in this order:

1. `python tools/make_webui_header.py` — **required after any webui.html edit.**
   webui.h is a generated, committed file. It silently drifted for several
   releases, so web fixes were tagged and flashed without ever reaching the
   device. CI now fails on a stale header, but regenerate locally first.
2. `bash tools/test/run.sh` — host-side unit tests (WebDAV parser + body pump),
   webui.html JS syntax, webui.h freshness.
3. `arduino-cli compile` for **both** matrix FQBNs (JC3248 and Waveshare).
4. Tag `vX.Y.Z` and push with `--follow-tags`. CI builds and attaches the bins.
5. `cd ~/OneDrive/Documents/projecten/retro-shop/public/tools/gotek-flasher && ./update-firmware.sh vX.Y.Z`,
   then commit and `git push production main`.

---

# Open questions and deliberately-deferred work

Findings from the August 2026 audit that were **not** acted on, and why. Each
needs either hardware on a bench or the user's own server to settle.

## Needs a panel in front of you

- **`gfx_flush()` spends 24 ms per frame in `delayMicroseconds(500)`** across 48
  strips. Removing it is the single largest render win available and the single
  most dangerous change: `dma_buffer` is one shared buffer with
  `trans_queue_depth = 1` and no `on_color_trans_done` callback, so the delay is
  covering a real race where the next memcpy can overwrite the buffer mid-DMA.
  Fixing it properly means a second bounce buffer or a completion callback, and
  verifying it means watching the panel for tearing.
- **Partial / banded flush.** Two independent audits read the driver and reached
  *opposite* conclusions: one says thumbnails share a physical row band so a
  row-strip flush works, the other says `RASET` is skipped in QSPI mode so only
  a column-band flush is possible. Both cannot be right. Everything downstream
  (single-row scroll repaint, progress bar, keyboard caret) is blocked until
  this is settled on hardware — a wrong window command garbles the display.

## Needs a USB current meter

- **CPU frequency scaling (80/160/240).** Probably safe, but at 80 MHz the
  software AES-GCM path may not drain WiFi RX fast enough during a sustained
  TLS download, which would present as a dropped connection rather than an
  obvious clock fault.
- **WiFi AP lifecycle** (stopping the idle AP, `WIFI_PS_MIN_MODEM` in AP mode).
  The largest remaining steady-state power win — and the AP is the user's
  recovery path when the touchscreen misbehaves, so a bug here locks them out
  of the device entirely.
- **Idle dim / panel `DISPOFF`.** The wake path is unverified; if touch stops
  being serviced or `DISPON` does not restore, the device looks dead.
- **Regulator topology is unknown.** Whether the board uses an LDO or a buck
  converter determines whether any mA figure maps onto the Amiga's 5 V rail at
  all. Worth measuring before optimising further.

## Known limits

- **RAM disk is 1.44 MB** (`RAM_DISK_SIZE = 2880 * 512`), of which 1,457,664
  bytes are usable after the FAT12 overhead. That fits Amiga ADF (901,120),
  Atari ST (720–800 KB), CPC DSK and most HFE images — but **not** a full
  1,474,560-byte PC 1.44 MB image. Since v0.12.1 an oversized image is refused
  with a message instead of being silently truncated and mounted. Raising the
  limit means changing the FAT12 geometry constants together, which wants a
  bench test.
- **Waveshare variant still fails CI** (`lgfx::Touch_CST328` absent from
  LovyanGFX 1.2.21) — see the existing note further down.

## Worth doing, just not unattended

- **WebDAV thumbnails at scale.** v0.13.x draws a thumbnail whenever the cover
  is already cached and re-targets the pre-cacher at the visible rows, which is
  the safe 80 %. The rest — JPEGDEC scaled decode, and asking Nextcloud/Stack
  for a server-side preview instead of the full-size original — needs testing
  against the user's actual server.
- **Boot index (`/INDEX.BIN`).** The folder scan re-opens each game directory
  several times and bubble-sorts the result; on a 3000-title card that is tens
  of seconds of every boot. Straightforward to fix, but it touches the code path
  that decides what the Amiga sees, so it wants a careful pass with the card in
  hand.
- **On-device search, favourites and recently-played.** The keyboard modal
  already exists (`ui_keyboard.h`); all three reduce to swapping which index
  vector the list iterates.

---

# Power & Lite-Build Plan

The full firmware works on a stable 5V supply but the Amiga's internal USB rail (~500 mA nominal) browns out when the WiFi AP comes up at the same time as the backlight. This plan addresses the brownout, introduces a Lite (SD-only) build, and lays groundwork for browser-based flashing from a website.

We don't rewrite from scratch — the architecture is fine. Changes are surgical.

## Branch

All work lands on `power-lite`. Merge to `main` when each spoor is verified.

## Spoor 1 — Lite build via compile-time feature flags

Goal: A build that never starts the WiFi stack, ships ~150–200 KB smaller, and is guaranteed not to brown out.

USB MSC is **always on** — it's the whole point of the device (the Amiga reads the
emulated floppy over USB). Only the network-side features are stripped.

- Add `build_config.h` with:
  - `ENABLE_WIFI` (off in Lite, on in Full) — gates AP + STA + the entire WiFi stack
  - `ENABLE_WEBSERVER` (off in Lite, on in Full) — gates HTTP server + REST API
  - `ENABLE_WEBDAV_CLIENT` (off in Lite, on in Full) — gates remote-browse
  - `ENABLE_FTP_CLIENT` (off in Lite, on in Full) — gates remote-browse
- Two variants in one codebase:
  - `lite` — touch UI + SD browser + USB MSC. No radio.
  - `full` — adds WiFi AP, web server, WebDAV/FTP clients, remote-dongle mode.
- Wrap `#include <WiFi.h>`, `<WiFiClientSecure.h>`, `<HTTPClient.h>`, the `webserver.h` / `webdav_client.h` / `ftp_client.h` includes, and their use-sites in `#ifdef ENABLE_WIFI` / matching flags. UI settings screens hide the WiFi tile in Lite at compile-time.

## Spoor 2 — Boot-sequence fix (applies to Full and Lite)

Goal: Spread current-draw spikes in time instead of stacking them.

- **Soft backlight ramp**: replace abrupt `ledcWrite(LCD_PIN_BL, 200)` with 0→target over ~300 ms in a short loop. Removes inrush.
- **Lower default backlight**: 200 (78%) → 140 (~55%). Tunable via `CONFIG.TXT` (`BACKLIGHT_LEVEL=140`).
- **Defer WiFi**: move `initWiFiAP()` + `startWebServer()` to *after* `USB.begin()` and a small settle delay (~150 ms). Each subsystem powers up sequentially.
- **Touch-hold escape hatch**: during the splash, poll touch — if held >2 s, override `WIFI_ENABLED=0` for this boot. Fixes the chicken-and-egg if the device can't boot with WiFi on. Show a hint on the splash ("Hold screen to skip WiFi").

## Spoor 3 — Runtime power tuning (Full only)

Goal: Lower idle current and reduce average draw once running.

- `setCpuFrequencyMhz(80)` as idle baseline; bump to 240 on touch / redraw / SD read, then return.
- `WiFi.setSleep(WIFI_PS_MIN_MODEM)` once AP is up — modem-sleep between beacons.
- Increase DTIM interval (`esp_wifi_set_inactive_time`) so radio sleeps longer.
- Optional: turn backlight off after N seconds idle, wake on touch.

## Spoor 5 — On-device WiFi setup (plug-and-play network onboarding)

Goal: get the device on the user's LAN without the current phone-juggling flow
(connect phone to AP → lose internet → navigate to 192.168.4.1 → enter credentials →
disconnect → hunt for new LAN IP). The device is a touchscreen — let it do the work.

### Why the current flow is bad
1. Phone has to drop its own internet to talk to the device's AP.
2. User has to type credentials on a captive-portal-style page that they reached
   via an IP address they had to remember.
3. After save, the user has no easy way to find the device's new LAN IP.
4. WebDAV/FTP usage then requires that LAN IP, manually entered.

### Approach (recommended): A + D

**A. On-device scan + on-screen keyboard** (the headline feature)

- New screen `SCR_WIFI_SETUP`, reached from a `[WIFI]` button on System Info.
- Layout:
  - Header: "WiFi Setup" + `[SCAN]` button.
  - Scrollable list of nearby SSIDs from `WiFi.scanNetworks(false, true)`:
    - SSID name (truncated if long)
    - Signal-strength bars (4 levels from RSSI)
    - Lock icon for WPA/WPA2 networks
    - Tick mark on the currently configured SSID
  - Tap an SSID → password modal with on-screen keyboard.
- Keyboard:
  - QWERTY layout, 4 rows × ~10 keys (each ~44 px on JC3248).
  - Shift, backspace, space, symbols (`@`, `_`, `!`, `.`, `-`, digits row at top).
  - Show entered chars (`*` masked by default, eye toggle for show/hide).
  - `[CONNECT]` and `[CANCEL]` buttons.
- On `[CONNECT]`:
  - `WiFi.begin(ssid, pass)` in `WIFI_AP_STA` mode (keep AP up for fallback).
  - Status screen with spinner: "Connecting to <SSID>…" + timeout (~15 s).
  - Success: show assigned IP + `gotek.local` hostname + QR code with `http://gotek.local/`.
  - Failure: red banner, return to keyboard with password prefilled for retry.
- On success: persist `WIFI_CLIENT_SSID` / `WIFI_CLIENT_PASS` / `WIFI_CLIENT_ENABLED=1`
  to CONFIG.TXT so it auto-reconnects on next boot.

**D. mDNS** (do this no matter what)

- `MDNS.begin("gotek")` + `MDNS.addService("http", "tcp", 80)` after STA connect.
- Apple/Android/Win11 resolve `gotek.local` natively on most LANs.
- Eliminates "find the new IP" step entirely.
- ~5 lines of code.

### Optional bonus

**B. WPS push-button** — small icon on the WiFi Setup screen labelled `[WPS]`.
`WiFi.beginWPSConfig()`. One tap on the router's WPS button + one tap on the
device. Works on routers that still support WPS; harmless to ship even if
many users can't use it.

### Skipped

**C. Captive portal smoothing** — Not worth the maintenance. Replaced by A.

### Open design questions

1. Password show/hide toggle, or always masked? (Eye icon recommended.)
2. Symbols set: alphanumeric only, or full keyboard with `@`, `!`, `.`, `_`, `-`, `+`,
   `=`, `#`, etc.? Most real-world WPA passwords need at least some.
3. Length: support 63-char WPA passwords without horizontal scrolling — render
   in a wrap-aware text field.
4. After successful connect: show a QR code encoding `http://gotek.local/` so
   the user can jump straight from their phone to the WebDAV UI on the same
   LAN without typing.
5. Should the AP stay up forever after STA connect, or auto-disable after N
   minutes to save power (Spoor 3 territory)? Probably keep it as a fallback
   in case STA drops.

### Effort

- A alone: ~1–1.5 days of UI work (keyboard is the bulk).
- D: 1 hour, drop-in.
- B: 2 hours.
- Total: ~2 days well-spent, eliminates the worst part of the current UX.

### Dependencies

- Belongs in the Full build only — Lite ships with no WiFi at all (Spoor 1).
- Stack with Spoor 1 carefully: WiFi-setup screen and keyboard must live under
  the same `#ifdef ENABLE_WIFI` guard.

---

## Open issue — Waveshare CI dependency

The Waveshare matrix job in `build-release.yml` is currently
`allow_failure: true` because the source at
`Gotek_Touchscreen/Gotek_Touchscreen.ino:298` references
`lgfx::Touch_CST328`, which is absent from LovyanGFX 1.2.21 in the
Arduino registry. The comment two lines down says "CST816S" — likely
one of:

- the original developer used a fork / dev version of LovyanGFX that
  has `Touch_CST328`,
- or the class name in source is wrong and should be
  `Touch_CST816S` / `Touch_CST3xx`.

Fix path: confirm which touch IC the Waveshare 2.8" board actually
exposes, change source to a class that exists in 1.2.21 (or pin
LovyanGFX to a specific git-url that has CST328), then drop
`allow_failure` from the matrix and re-enable the Waveshare option in
`web-flasher/index.html`.

## Spoor 4 — Browser-based flasher  ✅ done (v0.9.0)

Goal: User plugs in their device, opens a static page, clicks Install.

Shipped:

- `.github/workflows/build-release.yml` — compiles the sketch with
  `arduino-cli` on every pushed `v*.*.*` tag (and via `workflow_dispatch`),
  then attaches the four required `.bin` files to a GitHub Release.
- `web-flasher/index.html` — single-file installer powered by
  [ESP Web Tools](https://esphome.github.io/esp-web-tools/). Hostable on
  any static site; works from Chrome / Edge / Opera via the Web Serial API.
- `web-flasher/manifest.json` — points at
  `releases/latest/download/*.bin`, so the manifest never needs updating
  between versions.
- `web-flasher/README.md` — hosting + release process docs.

## Order of work

| Spoor | Effort  | Brownout impact | Notes |
|-------|---------|-----------------|-------|
| 2     | ~½ day  | High            | Fastest validation; done |
| 1     | ~1 day  | Total           | Lite has no WiFi at all |
| 3     | ~½ day  | Low–medium      | Idle current, no regression |
| 5     | ~2 days | None (UX)       | On-device WiFi setup + mDNS |
| 4     | ~1 day  | None (UX)       | Browser-based flasher |

Spoor 2 first → measure → Spoor 1 → 3, 4, 5 prioritised by user demand.

## Verification

- Spoor 2: device boots reliably with WiFi enabled on the Amiga. Confirm with the user (and ideally a USB current meter).
- Spoor 1: Lite binary contains no WiFi symbols (`nm` / `arduino-cli` size report); device powers up to SD browser without ever radiating.
- Spoor 3: `esp_pm_dump_locks` or simply observing idle current drop; no functional regression.
- Spoor 5: scan finds the home network, on-screen keyboard accepts a 63-char WPA password, device reports the new LAN IP and `gotek.local` resolves from a phone on the same LAN.
- Spoor 4: end-to-end flash of an ESP32-S3 dev board from the page.
