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
