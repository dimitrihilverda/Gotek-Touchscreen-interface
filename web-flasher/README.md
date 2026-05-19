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

The manifests use **relative URLs**, so the binaries must live next to
`index.html`. GitHub Releases by themselves don't expose CORS headers on
their download redirects, so a `fetch()` from a different origin can't
reach the .bin files — same-origin hosting is the reliable path.

### 1. GitHub Pages (automatic, recommended)

The `build-release.yml` workflow bundles `index.html`, both
`manifest-*.json`, and all `.bin` files into a single artifact and pushes
it to GitHub Pages on every tag. Enable Pages once for the repo:

  Repo Settings → Pages → Build and deployment → Source: **GitHub Actions**

The flasher will then be available at
`https://dimitrihilverda.github.io/Gotek-Touchscreen-interface/` after the
next release.

### 2. Your own site

Copy `index.html`, the `manifest-*.json` files, **and the `.bin` files
from the latest release** into the same directory on your web root, e.g.
`https://dimitrihilverda.nl/gotek-flash/`. Re-upload the .bin files when
you cut a new release.

### 3. Local testing

```sh
cd web-flasher
./fetch-binaries.sh         # pulls .bin files from the latest release
python -m http.server 8765  # any static server is fine
```

Then open <http://localhost:8765/> in Chrome / Edge. The .bin files are
listed in `.gitignore` so they don't get committed.

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
