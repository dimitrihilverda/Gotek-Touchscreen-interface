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

## Spoor 4 — Browser-based flasher

Goal: User plugs in their device, opens `dimitrihilverda.<tld>/gotek-flash`, picks a variant, clicks Flash.

- Use [esptool-js](https://github.com/espressif/esptool-js) (Web Serial API, Chrome/Edge).
- Static HTML page with variant dropdown: `lite`, `full`.
- Pre-built `.bin` artifacts pushed as GitHub Release assets by a CI workflow on tag.
  - `arduino-cli compile` or PlatformIO matrix build, one job per variant.
- Page fetches the latest release `.bin` from GitHub and streams it via Web Serial.
- Detect chip (must be ESP32-S3) before flashing.

## Order of work

| Spoor | Effort | Brownout impact |
|-------|--------|-----------------|
| 2     | ~½ day | High — fastest validation |
| 1     | ~1 day | Total (Lite has no WiFi at all) |
| 3     | ~½ day | Low–medium, mostly idle current |
| 4     | ~1 day | None (UX) |

Spoor 2 first → measure → Spoor 1 → 3 and 4 in parallel.

## Verification

- Spoor 2: device boots reliably with WiFi enabled on the Amiga. Confirm with the user (and ideally a USB current meter).
- Spoor 1: Lite binary contains no WiFi symbols (`nm` / `arduino-cli` size report); device powers up to SD browser without ever radiating.
- Spoor 3: `esp_pm_dump_locks` or simply observing idle current drop; no functional regression.
- Spoor 4: end-to-end flash of an ESP32-S3 dev board from the page.
