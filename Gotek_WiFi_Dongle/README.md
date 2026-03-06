# Gotek WiFi Dongle

A headless WiFi USB dongle that plugs directly into a Gotek's USB port. No display, no touchscreen — control everything from your phone or laptop via a web browser.

## Concept

Most Gotek users already have a working Gotek with FlashFloppy firmware. This dongle replaces the USB thumb drive with a tiny WiFi-enabled ESP32-S3 board. Instead of physically swapping USB sticks or walking to the computer, you browse your game library and load disks wirelessly from your phone.

```
┌──────────────────────────────────────────────────────┐
│  Retro Computer (Amiga, Atari ST, etc.)              │
│                                                      │
│  ┌─────────────────────┐   USB    ┌───────────────┐  │
│  │  Gotek / FlashFloppy │◄────────│  WiFi Dongle  │  │
│  │  (floppy emulator)   │         │  (ESP32-S3)   │  │
│  └─────────────────────┘         └───────┬───────┘  │
│                                          │ WiFi     │
└──────────────────────────────────────────│──────────┘
                                           │
                              ┌─────────────┘
                              ▼
                    ┌───────────────────┐
                    │  Phone / Laptop   │
                    │  Web Browser      │
                    │  http://gotek.local│
                    └───────────────────┘
```

## How It Works

1. The dongle creates a WiFi Access Point (`Gotek-Dongle`, password `retrogaming`)
2. Connect your phone or laptop to this WiFi network
3. Open `http://192.168.4.1` in your browser
4. Browse your game library, tap to load — the dongle presents the disk image as a USB floppy to the Gotek
5. The Gotek/FlashFloppy sees it as a regular USB drive with a disk image on it

The dongle uses the ESP32-S3's native USB and PSRAM to emulate a FAT12 floppy disk in memory, identical to a physical USB stick with an ADF/DSK file on it.

## Target Hardware

### Seeed XIAO ESP32-S3 (Recommended)

The **Seeed XIAO ESP32-S3 Sense** is the ideal board for this project:

| Spec | Value |
|------|-------|
| Size | 21 × 17.5 mm |
| MCU | ESP32-S3 dual-core 240MHz |
| Flash | 8 MB |
| PSRAM | 8 MB OPI |
| WiFi | 802.11 b/g/n |
| Bluetooth | 5.0 LE |
| USB | Type-C, OTG capable |
| microSD | Built-in slot (Sense variant) |
| GPIO | 11 pins (IO2–IO10, IO20, IO21) |

The **Sense variant** is recommended because it includes a built-in microSD card slot, eliminating the need for any external wiring beyond the USB connection.

The regular XIAO ESP32-S3 (without Sense) also works but requires an external microSD breakout module.

### Other Boards

Any ESP32-S3 board with PSRAM can be used. Edit the `ACTIVE_BOARD` define in the sketch:

```cpp
#define ACTIVE_BOARD BOARD_XIAO_S3_SENSE  // Default — built-in microSD
// #define ACTIVE_BOARD BOARD_XIAO_S3     // XIAO without Sense (external SD)
// #define ACTIVE_BOARD BOARD_GENERIC_S3  // Generic ESP32-S3 dev board
```

### Pin Configuration

**XIAO ESP32-S3 Sense** (built-in microSD):
| Function | GPIO |
|----------|------|
| SD CLK | IO7 |
| SD CMD | IO9 |
| SD D0 | IO8 |
| Status LED | IO21 |

**XIAO ESP32-S3** (external microSD breakout):
| Function | GPIO |
|----------|------|
| SD CLK | IO7 (SCL) |
| SD CMD | IO5 (A3) |
| SD D0 | IO4 (A2) |
| Status LED | IO21 |

## Building the Hardware

### Option 1: XIAO Sense + USB-A Adapter (Simplest)

The XIAO ESP32-S3 Sense with a 3D-printed USB-A adapter enclosure. No soldering required beyond the USB-A plug.

```
┌──────────────────────────┐
│  USB-A     ┌──────────┐  │
│  Plug  ◄───│ XIAO S3  │  │  ← 3D printed case
│            │  Sense   │  │
│            └──────────┘  │
│        [microSD slot]    │
└──────────────────────────┘
```

**Wiring:**
- USB-A plug pin 1 (VBUS/5V) → XIAO 5V
- USB-A plug pin 2 (D−) → XIAO D−
- USB-A plug pin 3 (D+) → XIAO D+
- USB-A plug pin 4 (GND) → XIAO GND

### Option 2: XIAO + External microSD Module

For the regular XIAO (non-Sense), add a microSD breakout:
- Wire SD CLK, CMD, D0 to the GPIO pins listed above
- Connect 3.3V and GND

## Building the Firmware

### Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 2.x or later
- ESP32 board support package (add Seeed XIAO boards via Board Manager)
- No external libraries required (WiFi and USB are built-in)

### Arduino IDE Settings

| Setting | Value |
|---------|-------|
| Board | XIAO_ESP32S3 |
| USB CDC On Boot | Enabled |
| PSRAM | OPI PSRAM |
| Flash Size | 8MB (64Mbit) |
| Partition Scheme | Huge APP (3MB No OTA / 1MB SPIFFS) |

### Upload

1. Connect the XIAO via USB-C
2. Select the correct COM port in Arduino IDE
3. Click Upload
4. Insert the prepared microSD card
5. Plug the dongle into your Gotek's USB port

## SD Card Setup

The SD card layout is identical to the touchscreen version:

```
SD Card Root/
├── CONFIG.TXT              # Auto-generated config file
├── ADF/                    # Amiga disk images
│   ├── Speedball 2/
│   │   ├── Speedball 2.adf
│   │   ├── Speedball 2.jpg     # Cover art (JPEG)
│   │   └── Speedball 2.nfo     # Game info (plain text)
│   ├── Cannon Fodder/
│   │   ├── Cannon Fodder-1.adf # Multi-disk: -1, -2, -3 suffix
│   │   ├── Cannon Fodder-2.adf
│   │   ├── Cannon Fodder-3.adf
│   │   ├── Cannon Fodder.jpg
│   │   └── Cannon Fodder.nfo
│   └── ...
└── DSK/                    # ZX Spectrum / Amstrad CPC images
    └── ...
```

You can also upload games via the web interface — no need to remove the SD card.

## Web Interface

The built-in web interface provides full control over the dongle:

- **Dashboard** — System info, memory stats, currently loaded game
- **Game Browser** — Scrollable list with cover art, tap to load/eject
- **Game Details** — Full cover art, game info, multi-disk selector
- **Upload** — Drag & drop ADF/DSK files, cover art, and NFO files
- **Config** — WiFi settings, mode selection (ADF/DSK)
- **Cover Art** — Upload from device or download from URL

### WiFi Configuration

**Default Access Point:**
- SSID: `Gotek-Dongle`
- Password: `retrogaming`
- IP: `http://192.168.4.1`

**Connect to home network** (for internet access / cover art downloads):
Add these to `CONFIG.TXT` or set via the web interface:
```ini
WIFI_CLIENT_ENABLED=1
WIFI_CLIENT_SSID=YourHomeNetwork
WIFI_CLIENT_PASS=YourPassword
```

When connected to your home network, the dongle is accessible via its local IP address (shown on the dashboard and in Serial output).

## Status LED

The built-in LED on IO21 provides visual feedback:

| Pattern | Meaning |
|---------|---------|
| 1 blink on boot | Initializing |
| 3 blinks | Setup complete, ready |
| 1 short blink | Loading disk image |
| 2 short blinks | Disk loaded successfully |
| 3 short blinks | Disk ejected |
| 5 rapid blinks (repeating) | Fatal error (RAM allocation failed) |

## API Reference

The dongle exposes the same REST API as the touchscreen version:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web UI (SPA) |
| `/api/system/info` | GET | System info (firmware, memory, SD stats) |
| `/api/config` | GET/POST | Read/write configuration |
| `/api/games/list` | GET | List all games with metadata |
| `/api/games/{mode}/{name}` | GET | Game detail (disks, NFO, cover status) |
| `/api/games/{mode}/{name}` | DELETE | Delete a game |
| `/api/games/{mode}/{name}/load` | POST | Load a disk image |
| `/api/games/{mode}/{name}/cover` | GET | Serve cover image |
| `/api/games/{mode}/{name}/cover` | POST | Upload cover image |
| `/api/games/{mode}/{name}/cover-url` | POST | Download cover from URL |
| `/api/games/{mode}/{name}/nfo` | POST | Update NFO text |
| `/api/games/upload` | POST | Upload disk image (multipart) |
| `/api/disk/status` | GET | Currently loaded disk info |
| `/api/disk/unload` | POST | Eject current disk |
| `/api/rescan` | POST | Rescan SD card for games |
| `/api/wifi/status` | GET | WiFi connection status |
| `/api/wifi/scan` | GET | Scan for available networks |
| `/api/themes/list` | GET | List available themes |
| `/api/themes/{name}/activate` | POST | Activate a theme |

## Differences from Touchscreen Version

| Feature | Touchscreen | WiFi Dongle |
|---------|-------------|-------------|
| Display | 2.8"–3.5" touchscreen | None (headless) |
| Control | Touch + Web UI | Web UI only |
| Form factor | Standalone unit | USB stick |
| SD card | Full-size SD (SDMMC) | microSD (XIAO Sense) |
| Libraries | JPEGDEC, PNGdec, LovyanGFX | None (built-in only) |
| Flash usage | ~2MB (display drivers + fonts) | ~500KB |
| WiFi SSID | Gotek-Setup | Gotek-Dongle |

## Related Projects

- **[Gotek_Touchscreen](../Gotek_Touchscreen/)** — Full touchscreen version with display
- **[OpenFlops](https://github.com/SukkoPera/OpenFlops)** — Open-source Gotek replacement hardware
- **[FlashFloppy](https://github.com/keirf/FlashFloppy)** — Custom Gotek firmware

## License

MIT — see [LICENSE](../LICENSE) for details.
