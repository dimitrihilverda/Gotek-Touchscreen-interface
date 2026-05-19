# Web flasher

One-click installer for the Gotek Touchscreen Interface firmware, powered by
[ESP Web Tools](https://esphome.github.io/esp-web-tools/).

## Files

- `index.html` — standalone page. Drop on any static host.
- `manifest-jc3248.json` — ESP Web Tools manifest for the Guition JC3248W535C.
- `manifest-waveshare.json` — manifest for the Waveshare ESP32-S3-Touch-LCD-2.8.

The page has a board dropdown that swaps which manifest the install button
loads. Visitor's last choice is remembered in `localStorage`.

## How to host

Two options:

### 1. Your own site (recommended)

Copy `index.html` and `manifest.json` to your web root, e.g.
`https://dimitrihilverda.nl/gotek-flash/`. That's the whole deployment —
the manifest references absolute URLs on GitHub Releases, so the binaries
never need to live on your server.

### 2. GitHub Pages

Enable GitHub Pages for this repository (Settings → Pages → Source: `main` → folder `/web-flasher`).
The flasher then lives at `https://dimitrihilverda.github.io/Gotek-Touchscreen-interface/`.

## Requirements

- **Chrome, Edge, or Opera** on desktop. Firefox and Safari do not implement
  the Web Serial API.
- A USB-C / USB-A data cable (not a charge-only cable).

## How releases feed the flasher

The `.github/workflows/build-release.yml` workflow uses a build matrix to
produce binaries for each supported display variant on every pushed
`v*.*.*` tag (or via manual `workflow_dispatch`). The arduino-cli
`--build-property` flag injects the right `ACTIVE_DISPLAY` define per
matrix entry. For each variant `V` the workflow attaches these files to
the GitHub release:

| Filename                                   | Offset (hex) | Source                |
|--------------------------------------------|--------------|-----------------------|
| `gotek-touchscreen-{V}-bootloader.bin`     | `0x0000`     | sketch build          |
| `gotek-touchscreen-{V}-partitions.bin`     | `0x8000`     | sketch build          |
| `boot_app0.bin`                            | `0xE000`     | ESP32 Arduino core    |
| `gotek-touchscreen-{V}.bin`                | `0x10000`    | sketch build (app)    |

The manifests use `releases/latest/download/<filename>` URLs, so the
flasher always picks up the newest tagged release without any manifest
edits. Currently `{V}` is `jc3248` or `waveshare`. Adding a new variant is
just one more matrix entry in the workflow plus a new `manifest-{V}.json`
and option in the dropdown.

## Cutting a new release

1. Bump `FW_INTERNAL` in `Gotek_Touchscreen.ino`.
2. Commit, push.
3. Tag: `git tag -a vX.Y.Z -m "..."` and `git push origin vX.Y.Z`.
4. The workflow compiles and publishes the release automatically.
