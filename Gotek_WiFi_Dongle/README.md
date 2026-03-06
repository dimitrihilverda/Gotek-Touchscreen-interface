# Gotek WiFi Dongle

A minimal WiFi-to-USB dongle that plugs into a Gotek's USB port. Send disk images from your phone — no SD card, no display, no extra wiring.

## How It Works

```
  Phone / Laptop                    Gotek WiFi Dongle              Gotek
 ┌──────────────┐    WiFi     ┌─────────────────────┐    USB    ┌─────────┐
 │  Browser      │───────────►│  XIAO ESP32-S3       │─────────►│  Gotek  │
 │  Upload ADF   │            │  8MB PSRAM = disk     │          │  floppy │
 └──────────────┘            └─────────────────────┘          └─────────┘
```

1. The dongle creates a WiFi network (`Gotek-Dongle`, password `retrogaming`)
2. Connect your phone and open `http://192.168.4.1`
3. Tap to select or drag & drop an ADF/DSK/IMG file
4. The file streams directly into PSRAM (8MB available, floppy max 1.44MB)
5. The dongle presents it as a USB floppy drive to the Gotek
6. Play! To switch games, upload another file — it replaces the current disk.

No SD card needed. Your game library lives on your phone/laptop.

## Hardware

### Seeed XIAO ESP32-S3

| Spec | Value |
|------|-------|
| Size | 21 × 17.5 mm |
| MCU | ESP32-S3 dual-core 240MHz |
| PSRAM | 8 MB (stores the disk image) |
| Flash | 8 MB (stores firmware + web UI) |
| WiFi | 802.11 b/g/n |
| USB | Type-C with OTG |
| Price | ~$7 |

That's the entire bill of materials. One board, one USB-A plug, done.

### Wiring

Connect the XIAO's USB data lines to a USB-A plug:

| USB-A Pin | XIAO Pin |
|-----------|----------|
| 1 (VBUS/5V) | 5V |
| 2 (D−) | D− |
| 3 (D+) | D+ |
| 4 (GND) | GND |

A 3D-printed enclosure turns it into a neat USB stick that fits directly into the Gotek.

### Status LED

The built-in LED on IO21 shows what's happening:

| Pattern | Meaning |
|---------|---------|
| 1 long blink | Booting |
| 3 short blinks | Ready, waiting for connection |
| 1 short blink | Receiving disk image |
| 2 short blinks | Disk loaded, USB active |
| 3 short blinks | Disk ejected |
| 5 rapid (repeating) | Error: PSRAM allocation failed |

## Building

### Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 2.x
- Seeed XIAO ESP32-S3 board package
- No external libraries needed

### Arduino IDE Settings

| Setting | Value |
|---------|-------|
| Board | XIAO_ESP32S3 |
| USB CDC On Boot | Enabled |
| PSRAM | OPI PSRAM |
| Flash Size | 8MB |
| Partition Scheme | Default 4MB with spiffs |

### Upload

1. Connect XIAO via USB-C
2. Select COM port → Upload
3. Wire to USB-A plug, insert into Gotek

## Web Interface

The web UI is a single-page app embedded in the firmware (~5KB). It works on any device with a browser.

### Upload & Play

Open `http://192.168.4.1` on your phone:

- **Drag & drop** or tap to select an ADF/DSK/IMG file
- Progress bar shows upload status
- File streams directly into PSRAM → USB floppy activates
- **Eject** button to remove the current disk

### System Info

The dashboard shows firmware version, free PSRAM, WiFi IP, and connected clients.

## REST API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web UI |
| `/api/status` | GET | Current state (loaded file, PSRAM, WiFi) |
| `/api/load` | POST | Upload disk image (raw binary, `X-Filename` header) |
| `/api/eject` | POST | Eject current disk |

### Upload Example (curl)

```bash
curl -X POST http://192.168.4.1/api/load \
  -H "X-Filename: Speedball2.adf" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @Speedball2.adf
```

### Status Example

```bash
curl http://192.168.4.1/api/status
```
```json
{
  "loaded": true,
  "filename": "Speedball2.adf",
  "size": 901120,
  "firmware": "v1.0.0-WiFiDongle",
  "free_psram": 7200000,
  "wifi_ip": "192.168.4.1",
  "wifi_clients": 1
}
```

## Architecture

The entire firmware is a single `.ino` file (~500 lines). No external dependencies.

```
Gotek_WiFi_Dongle.ino
├── WiFi AP (creates hotspot)
├── HTTP Server (serves web UI + REST API)
├── PSRAM RAM Disk (1.44MB FAT12 floppy image)
├── FAT12 Builder (creates filesystem structure around uploaded data)
├── USB Mass Storage (presents RAM disk as floppy to Gotek)
└── LED Status (visual feedback via IO21)
```

The upload flow is zero-copy: the HTTP handler reads the file data directly into the PSRAM data area at the correct offset. Then the FAT12 metadata is built around it and USB is reconnected.

## Differences from Touchscreen Version

| | Touchscreen | WiFi Dongle |
|-|-------------|-------------|
| Display | 2.8"–3.5" touchscreen | None |
| Storage | SD card (unlimited games) | PSRAM only (one game at a time) |
| Game library | On device | On your phone/laptop |
| Control | Touch + Web UI | Web UI only (phone/laptop) |
| Board | JC3248 / Waveshare | XIAO ESP32-S3 |
| Size | ~60 × 40 mm | 21 × 17.5 mm |
| External deps | JPEGDEC, PNGdec, LovyanGFX | None |
| Code size | ~3000 lines | ~500 lines |
| Files | 8 files | 1 file |

## License

MIT — see [LICENSE](../LICENSE) for details.
