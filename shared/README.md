# Gotek — Shared Firmware Library

This directory contains code shared by all Gotek device targets. The goal is **one web UI, one API surface, one set of connectivity drivers** — only hardware-specific code lives in the device `.ino`.

## Architecture

```
JC3248W535EN/
├── shared/                      ← THIS directory — shared for all devices
│   ├── http_utils.h             JSON helpers, HTTP response senders, request parser
│   ├── log_buffer.h             In-memory log ring buffer (GET /api/log)
│   ├── dav_folder_cache.h       Per-folder DAV disk-list cache (SD or SPIFFS)
│   ├── connectivity_api.h       FTP, WebDAV, WiFi API route handlers
│   └── README.md                This file
│
├── Gotek_Touchscreen/           JC3248 + Waveshare touch display target
│   ├── Gotek_Touchscreen.ino    Main: display, SD, USB MSC, touch, game list
│   ├── api_handlers.h           Device API: games, covers, upload, themes (SD)
│   ├── webserver.h              HTTP server loop + multipart upload
│   ├── ftp_client.h             FTP client (shared binary)
│   ├── webdav_client.h          WebDAV client (shared binary)
│   ├── webui.h                  Gzipped web UI PROGMEM array
│   └── webui.html               Web UI source (edit this, then regenerate webui.h)
│
└── Gotek_WiFi_Dongle/           Headless WiFi dongle target (XIAO ESP32-S3)
    ├── Gotek_WiFi_Dongle.ino    Main: PSRAM RAM disk, USB MSC, NVS config
    ├── ftp_client.h             FTP client (shared binary)
    ├── webdav_client.h          WebDAV client (shared binary)
    ├── webui.h                  Same gzipped web UI
    └── webui.html               Same web UI source (always keep in sync)
```

## Device capability matrix

| Feature               | JC3248 / Waveshare | WiFi Dongle |
|-----------------------|--------------------|-------------|
| Touch display         | ✅                  | ❌           |
| SD card (game store)  | ✅ (SD_MMC)         | ❌           |
| PSRAM RAM disk        | ✅ (game buffer)    | ✅ (floppy)  |
| USB MSC (floppy emu)  | ✅                  | ✅           |
| NVS Preferences       | ❌ (CONFIG.TXT)     | ✅           |
| SPIFFS cache          | ❌ (SD)             | ✅           |
| Cover cache (SD)      | ✅                  | ❌           |
| WebDAV folder cache   | ✅ (SD)             | ✅ (SPIFFS)  |
| FTP client            | ✅ (download→SD)    | ✅ (→RAM)    |
| WebDAV client         | ✅ (stream→RAM)     | ✅ (stream→RAM) |
| Web UI                | ✅ (identical)      | ✅ (identical) |
| Themes                | ✅ (SD + display)   | ✅ (web only) |
| LOG.TXT (SD)          | ✅                  | ❌           |
| Log buffer (RAM)      | ❌                  | ✅           |

## Extending to a new device

1. Create `Gotek_NewDevice/` directory.
2. Copy `ftp_client.h`, `webdav_client.h`, `webui.h`, `webui.html` from an existing device.
3. In your `.ino`, define:
   ```cpp
   #define DEVICE_NEW_DEVICE
   // Storage backend define for dav_folder_cache.h:
   #define DAV_CACHE_FS    SPIFFS          // or SD_MMC
   #define DAV_CACHE_FS_IS_SPIFFS          // omit for SD
   #define DAV_CACHE_DIR   ""              // SPIFFS: empty; SD: "/DAV_FOLDER_CACHE"
   ```
4. Include the shared headers **before** any device-specific code:
   ```cpp
   #include "../shared/http_utils.h"
   #include "../shared/log_buffer.h"
   #include "../shared/dav_folder_cache.h"
   #include "../shared/connectivity_api.h"
   ```
5. Implement device-specific API routes (disk load, system info, etc.).
6. In the main request router, call the shared handlers for connectivity routes
   and add device-specific routes around them.

## Updating the Web UI

The web UI source is `webui.html`. After editing it, regenerate `webui.h` for
**both** device targets:

```bash
# Linux / macOS
gzip -9 -k webui.html
python3 - <<'EOF'
with open("webui.html.gz","rb") as f: d=f.read()
with open("webui.h","w") as out:
    out.write("// Auto-generated — do not edit\n")
    out.write(f"const uint8_t webui_html_gz[] PROGMEM = {{\n  ")
    out.write(", ".join(f"0x{b:02x}" for b in d))
    out.write(f"\n}};\nconst size_t webui_html_gz_len = {len(d)};\n")
EOF
```

Then copy the resulting `webui.h` to both `Gotek_Touchscreen/` and
`Gotek_WiFi_Dongle/`. The variable name `webui_html_gz` / `webui_html_gz_len`
is the same in both.
