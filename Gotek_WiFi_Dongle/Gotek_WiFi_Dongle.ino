/*
  Gotek WiFi Dongle — Headless PSRAM-only Edition

  A minimal ESP32-S3 WiFi dongle that plugs into a Gotek's USB port.
  No SD card, no display — disk images are sent via WiFi and stored in PSRAM.

  How it works:
    1. Dongle creates WiFi AP ("Gotek-Dongle")
    2. Open http://192.168.4.1 on your phone/laptop
    3. Upload or drag & drop an ADF/DSK file
    4. Dongle loads it into PSRAM → presents as USB floppy to Gotek
    5. Play!

  Target board: Seeed XIAO ESP32-S3 (21 x 17.5 mm)
    - ESP32-S3 dual-core 240MHz
    - 8MB PSRAM (plenty for a 1.44MB floppy image)
    - 8MB Flash
    - WiFi 802.11 b/g/n
    - USB-C with OTG support
    - No SD card needed!

  Board settings (Arduino IDE):
    Board: XIAO_ESP32S3
    USB CDC On Boot → Enabled
    PSRAM → OPI PSRAM
    Flash Size → 8MB
    Partition → Default 4MB with spiffs

  Wiring:
    USB-A plug → Gotek USB port
    That's it. No other connections needed.
*/

#include <Arduino.h>
#include <vector>
#include <USB.h>
#include <USBMSC.h>
#include <WiFi.h>
#include <WiFiClient.h>

extern "C" {
  extern bool tud_mounted(void);
  extern void tud_disconnect(void);
  extern void tud_connect(void);
  extern void* ps_malloc(size_t size);
}

#define FW_VERSION "v1.0.0-WiFiDongle"

// ==========================================================================
// STATUS LED (XIAO ESP32-S3 built-in LED on IO21)
// ==========================================================================
#define LED_PIN 21

void ledBlink(int times = 1, int ms = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(ms);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(ms);
  }
}

// ==========================================================================
// WiFi CONFIG
// ==========================================================================
String cfg_wifi_ssid = "Gotek-Dongle";
String cfg_wifi_pass = "retrogaming";
uint8_t cfg_wifi_channel = 6;

// WiFi state
bool wifi_ap_active = false;
String wifi_ap_ip = "";

// ==========================================================================
// RAM DISK — FAT12 floppy in PSRAM
// ==========================================================================
#define RAM_DISK_SIZE (2880 * 512)   // 1.44 MB
#define FAT1_OFFSET   512
#define FAT2_OFFSET   5120
#define ROOTDIR_OFFSET 9728
#define DATA_OFFSET   16896

uint8_t *ram_disk = NULL;
uint32_t msc_block_count;

// Current state
String loaded_filename = "";
size_t loaded_size = 0;
bool disk_present = false;

// USB MSC
USBMSC msc;

// ==========================================================================
// FAT12 FILESYSTEM
// ==========================================================================

void fat12_set(uint8_t *fat, int idx, uint16_t val) {
  if (idx % 2 == 0) {
    fat[idx * 3 / 2] = val & 0xFF;
    fat[idx * 3 / 2 + 1] = (fat[idx * 3 / 2 + 1] & 0xF0) | ((val >> 8) & 0x0F);
  } else {
    fat[idx * 3 / 2] = (fat[idx * 3 / 2] & 0x0F) | ((val & 0x0F) << 4);
    fat[idx * 3 / 2 + 1] = (val >> 4) & 0xFF;
  }
}

void build_boot_sector(uint8_t *buf) {
  memset(buf, 0, 512);
  buf[0x00] = 0xEB; buf[0x01] = 0x3C; buf[0x02] = 0x90;
  memcpy(&buf[0x03], "MSDOS5.0", 8);
  *(uint16_t *)&buf[0x0B] = 512;    // bytes per sector
  buf[0x0D] = 1;                     // sectors per cluster
  *(uint16_t *)&buf[0x0E] = 1;      // reserved sectors
  buf[0x10] = 2;                     // number of FATs
  *(uint16_t *)&buf[0x11] = 224;    // root dir entries
  *(uint16_t *)&buf[0x13] = 2880;   // total sectors (1.44MB)
  buf[0x15] = 0xF0;                  // media descriptor (1.44MB floppy)
  *(uint16_t *)&buf[0x16] = 9;      // sectors per FAT
  *(uint16_t *)&buf[0x18] = 18;     // sectors per track
  *(uint16_t *)&buf[0x1A] = 2;      // heads
  buf[0x24] = 0x00;                  // drive number (floppy)
  buf[0x26] = 0x29;                  // extended boot sig
  buf[0x27] = 0x47; buf[0x28] = 0x4F; buf[0x29] = 0x54; buf[0x2A] = 0x4B;
  memcpy(&buf[0x2B], "GOTEK      ", 11);
  memcpy(&buf[0x36], "FAT12   ", 8);
  buf[510] = 0x55; buf[511] = 0xAA;
}

void make_83_name(const char *src, uint8_t *dst) {
  memset(dst, ' ', 11);
  const char *dot = strrchr(src, '.');
  int nameLen = dot ? (int)(dot - src) : (int)strlen(src);
  for (int i = 0, j = 0; i < nameLen && j < 8; i++) dst[j++] = toupper(src[i]);
  if (dot) { dot++; for (int j = 8; *dot && j < 11; dot++) dst[j++] = toupper(*dot); }
}

void build_empty_volume() {
  memset(ram_disk, 0, RAM_DISK_SIZE);
  build_boot_sector(&ram_disk[0]);

  // FAT1 + FAT2: media descriptor + end-of-chain
  uint8_t *fat1 = &ram_disk[FAT1_OFFSET];
  uint8_t *fat2 = &ram_disk[FAT2_OFFSET];
  memset(fat1, 0, 4608);
  memset(fat2, 0, 4608);
  fat12_set(fat1, 0, 0xFF0);
  fat12_set(fat1, 1, 0xFFF);
  fat12_set(fat2, 0, 0xFF0);
  fat12_set(fat2, 1, 0xFFF);

  // Empty root directory
  memset(&ram_disk[ROOTDIR_OFFSET], 0, 7168);

  msc_block_count = RAM_DISK_SIZE / 512;
}

// Load file data (already in ram_disk data area) into FAT structure
void build_fat_for_file(const char *filename, size_t fileSize) {
  // Root directory entry
  uint8_t *root = &ram_disk[ROOTDIR_OFFSET];
  memset(root, 0, 32);
  uint8_t fname83[11];
  make_83_name(filename, fname83);
  memcpy(root, fname83, 11);
  root[11] = 0x20;  // archive attribute
  *(uint16_t *)&root[26] = 2;  // start cluster
  *(uint32_t *)&root[28] = fileSize;

  // FAT chain
  uint16_t clusters = (fileSize + 511) / 512;
  uint8_t *fat1 = &ram_disk[FAT1_OFFSET];
  uint8_t *fat2 = &ram_disk[FAT2_OFFSET];
  for (int c = 2; c < 2 + clusters; c++) {
    uint16_t val = (c < 2 + clusters - 1) ? (c + 1) : 0xFFF;
    fat12_set(fat1, c, val);
    fat12_set(fat2, c, val);
  }
}

// ==========================================================================
// USB MSC CALLBACKS
// ==========================================================================

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  uint32_t addr = lba * 512 + offset;
  if (ram_disk && addr + bufsize <= RAM_DISK_SIZE) {
    memcpy(buffer, &ram_disk[addr], bufsize);
    return bufsize;
  }
  return -1;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  uint32_t addr = lba * 512 + offset;
  if (ram_disk && addr + bufsize <= RAM_DISK_SIZE) {
    memcpy(&ram_disk[addr], buffer, bufsize);
    return bufsize;
  }
  return -1;
}

// ==========================================================================
// DISK LOAD / EJECT
// ==========================================================================

// Load disk image data that's already been written to ram_disk[DATA_OFFSET]
void loadDisk(const String &filename, size_t size) {
  tud_disconnect();
  delay(50);

  // Rebuild FAT structure around the data
  build_boot_sector(&ram_disk[0]);
  uint8_t *fat1 = &ram_disk[FAT1_OFFSET];
  uint8_t *fat2 = &ram_disk[FAT2_OFFSET];
  memset(fat1, 0, 4608);
  memset(fat2, 0, 4608);
  fat12_set(fat1, 0, 0xFF0);
  fat12_set(fat1, 1, 0xFFF);
  fat12_set(fat2, 0, 0xFF0);
  fat12_set(fat2, 1, 0xFFF);
  memset(&ram_disk[ROOTDIR_OFFSET], 0, 7168);

  build_fat_for_file(filename.c_str(), size);

  loaded_filename = filename;
  loaded_size = size;
  disk_present = true;

  msc.mediaPresent(true);
  tud_connect();

  Serial.println("Loaded: " + filename + " (" + String(size) + " bytes)");
  ledBlink(2, 50);
}

void ejectDisk() {
  tud_disconnect();
  delay(50);

  build_empty_volume();
  loaded_filename = "";
  loaded_size = 0;
  disk_present = false;

  msc.mediaPresent(false);
  tud_connect();

  Serial.println("Disk ejected");
  ledBlink(3, 50);
}

// ==========================================================================
// HTTP HELPERS
// ==========================================================================

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 10);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

void sendResponse(WiFiClient &client, int code, const String &contentType, const String &body) {
  client.println("HTTP/1.1 " + String(code) + " OK");
  client.println("Content-Type: " + contentType);
  client.println("Content-Length: " + String(body.length()));
  client.println("Access-Control-Allow-Origin: *");
  client.println("Access-Control-Allow-Methods: GET,POST,DELETE,OPTIONS");
  client.println("Access-Control-Allow-Headers: Content-Type");
  client.println("Connection: close");
  client.println();
  client.print(body);
}

void sendJSON(WiFiClient &client, int code, const String &json) {
  sendResponse(client, code, "application/json", json);
}

// ==========================================================================
// WEB UI — embedded HTML (self-contained SPA)
// ==========================================================================

const char WEBUI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Gotek WiFi Dongle</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#1a1a2e;--panel:#16213e;--accent:#0f3460;--blue:#00a8cc;--green:#00cc66;--red:#e94560;--orange:#ff8c00;--text:#eee;--dim:#888}
body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);min-height:100vh;display:flex;flex-direction:column;align-items:center}
.header{background:linear-gradient(135deg,#0f3460,#00a8cc);width:100%;padding:20px;text-align:center;box-shadow:0 4px 20px rgba(0,0,0,.5)}
.header h1{font-size:22px;font-weight:700;letter-spacing:2px}
.header .sub{font-size:13px;opacity:.7;margin-top:4px;font-family:monospace}
.container{width:100%;max-width:500px;padding:16px}
.card{background:var(--panel);border:1px solid #2a2a4a;border-radius:12px;padding:20px;margin-bottom:16px}
.card h2{font-size:15px;color:var(--blue);margin-bottom:12px;text-transform:uppercase;letter-spacing:1px}
.status{display:flex;align-items:center;gap:10px;padding:12px;background:#0d1b2a;border-radius:8px;margin-bottom:12px}
.status .dot{width:12px;height:12px;border-radius:50%;flex-shrink:0}
.status .dot.on{background:var(--green);box-shadow:0 0 8px var(--green)}
.status .dot.off{background:var(--red)}
.status .info{flex:1}
.status .name{font-weight:600;font-size:15px}
.status .detail{font-size:12px;color:var(--dim);margin-top:2px}
.drop-zone{border:2px dashed #3a3a6a;border-radius:12px;padding:40px 20px;text-align:center;cursor:pointer;transition:all .2s}
.drop-zone:hover,.drop-zone.over{border-color:var(--blue);background:rgba(0,168,204,.08)}
.drop-zone .icon{font-size:48px;margin-bottom:8px}
.drop-zone p{font-size:14px;color:var(--dim)}
.drop-zone input{display:none}
.progress{display:none;margin-top:12px}
.progress-bar{height:6px;background:#2a2a4a;border-radius:3px;overflow:hidden}
.progress-fill{height:100%;background:linear-gradient(90deg,var(--blue),var(--green));width:0;transition:width .3s;border-radius:3px}
.progress-text{font-size:12px;color:var(--dim);margin-top:4px;text-align:center}
.btn{display:block;width:100%;padding:14px;border:none;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;transition:all .15s;text-align:center}
.btn:active{transform:scale(.98)}
.btn-eject{background:var(--red);color:#fff;margin-top:8px}
.btn-eject:hover{background:#ff2255}
.btn-eject:disabled{background:#444;color:#888;cursor:not-allowed}
.info-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.info-item{background:#0d1b2a;border-radius:6px;padding:10px}
.info-item .label{font-size:10px;color:var(--dim);text-transform:uppercase;letter-spacing:.5px}
.info-item .value{font-size:16px;font-weight:700;margin-top:2px}
.msg{padding:10px;border-radius:6px;font-size:13px;margin-top:8px;display:none}
.msg.ok{display:block;background:rgba(0,204,102,.15);color:var(--green);border:1px solid rgba(0,204,102,.3)}
.msg.err{display:block;background:rgba(233,69,96,.15);color:var(--red);border:1px solid rgba(233,69,96,.3)}
</style>
</head>
<body>

<div class="header">
  <h1>GOTEK WiFi DONGLE</h1>
  <div class="sub" id="ipAddr">connecting...</div>
</div>

<div class="container">

  <!-- Status Card -->
  <div class="card">
    <h2>Current Disk</h2>
    <div class="status">
      <div class="dot" id="statusDot"></div>
      <div class="info">
        <div class="name" id="statusName">No disk loaded</div>
        <div class="detail" id="statusDetail">Upload a disk image to begin</div>
      </div>
    </div>
    <button class="btn btn-eject" id="btnEject" disabled onclick="ejectDisk()">EJECT</button>
  </div>

  <!-- Upload Card -->
  <div class="card">
    <h2>Load Disk Image</h2>
    <div class="drop-zone" id="dropZone" onclick="document.getElementById('fileInput').click()">
      <div class="icon">💾</div>
      <p>Tap to select or drag & drop<br>ADF / DSK / IMG (max 1.44 MB)</p>
      <input type="file" id="fileInput" accept=".adf,.dsk,.img">
    </div>
    <div class="progress" id="progress">
      <div class="progress-bar"><div class="progress-fill" id="progressFill"></div></div>
      <div class="progress-text" id="progressText">Uploading...</div>
    </div>
    <div class="msg" id="msg"></div>
  </div>

  <!-- System Info -->
  <div class="card">
    <h2>System</h2>
    <div class="info-grid">
      <div class="info-item"><div class="label">Firmware</div><div class="value" id="infoFw">-</div></div>
      <div class="info-item"><div class="label">Free RAM</div><div class="value" id="infoRam">-</div></div>
      <div class="info-item"><div class="label">WiFi IP</div><div class="value" id="infoIp">-</div></div>
      <div class="info-item"><div class="label">Clients</div><div class="value" id="infoClients">-</div></div>
    </div>
  </div>

</div>

<script>
const $ = id => document.getElementById(id);

// Status polling
async function updateStatus() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    $('statusDot').className = 'dot ' + (d.loaded ? 'on' : 'off');
    $('statusName').textContent = d.loaded ? d.filename : 'No disk loaded';
    $('statusDetail').textContent = d.loaded ? (d.size/1024).toFixed(0)+' KB — USB active' : 'Upload a disk image to begin';
    $('btnEject').disabled = !d.loaded;
    $('infoFw').textContent = d.firmware;
    $('infoRam').textContent = (d.free_psram/1024).toFixed(0)+' KB';
    $('infoIp').textContent = d.wifi_ip;
    $('infoClients').textContent = d.wifi_clients;
    $('ipAddr').textContent = d.wifi_ip;
  } catch(e) {}
}

// File upload
async function uploadFile(file) {
  const maxSize = 1474560; // 1.44 MB
  if (file.size > maxSize) {
    showMsg('File too large! Max 1.44 MB ('+file.size+' bytes)', true);
    return;
  }
  const ext = file.name.split('.').pop().toLowerCase();
  if (!['adf','dsk','img'].includes(ext)) {
    showMsg('Unsupported format. Use .adf, .dsk, or .img', true);
    return;
  }

  $('progress').style.display = 'block';
  $('progressFill').style.width = '0%';
  $('progressText').textContent = 'Uploading ' + file.name + '...';
  $('msg').style.display = 'none';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/load');

  xhr.upload.onprogress = e => {
    if (e.lengthComputable) {
      const pct = (e.loaded / e.total * 100).toFixed(0);
      $('progressFill').style.width = pct + '%';
      $('progressText').textContent = 'Uploading... ' + pct + '%';
    }
  };

  xhr.onload = () => {
    $('progress').style.display = 'none';
    try {
      const d = JSON.parse(xhr.responseText);
      if (d.status === 'ok') {
        showMsg('Loaded: ' + d.filename + ' (' + (d.size/1024).toFixed(0) + ' KB)', false);
      } else {
        showMsg(d.error || 'Upload failed', true);
      }
    } catch(e) {
      showMsg('Upload failed: ' + xhr.statusText, true);
    }
    updateStatus();
  };

  xhr.onerror = () => {
    $('progress').style.display = 'none';
    showMsg('Connection error', true);
  };

  // Send raw binary with filename in header
  xhr.setRequestHeader('X-Filename', file.name);
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.send(file);
}

function showMsg(text, isErr) {
  $('msg').textContent = text;
  $('msg').className = 'msg ' + (isErr ? 'err' : 'ok');
}

async function ejectDisk() {
  try {
    await fetch('/api/eject', {method:'POST'});
    showMsg('Disk ejected', false);
    updateStatus();
  } catch(e) {
    showMsg('Eject failed', true);
  }
}

// Drag & drop
const dz = $('dropZone');
dz.addEventListener('dragover', e => { e.preventDefault(); dz.classList.add('over'); });
dz.addEventListener('dragleave', () => dz.classList.remove('over'));
dz.addEventListener('drop', e => {
  e.preventDefault();
  dz.classList.remove('over');
  if (e.dataTransfer.files.length) uploadFile(e.dataTransfer.files[0]);
});
$('fileInput').addEventListener('change', e => {
  if (e.target.files.length) uploadFile(e.target.files[0]);
  e.target.value = '';
});

// Init
updateStatus();
setInterval(updateStatus, 3000);
</script>
</body>
</html>
)rawliteral";

// ==========================================================================
// HTTP REQUEST PARSER
// ==========================================================================

struct HttpRequest {
  String method, path;
  int contentLength;
  String filename;     // from X-Filename header
  String contentType;
  String boundary;
};

bool parseRequest(WiFiClient &client, HttpRequest &req) {
  req.method = "";
  req.path = "";
  req.contentLength = 0;
  req.filename = "";
  req.contentType = "";
  req.boundary = "";

  String line = client.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return false;

  int sp1 = line.indexOf(' ');
  int sp2 = line.indexOf(' ', sp1 + 1);
  if (sp1 < 0 || sp2 < 0) return false;

  req.method = line.substring(0, sp1);
  req.path = line.substring(sp1 + 1, sp2);

  // Read headers
  while (client.connected()) {
    String hdr = client.readStringUntil('\n');
    hdr.trim();
    if (hdr.length() == 0) break;

    String lower = hdr;
    lower.toLowerCase();

    if (lower.startsWith("content-length:")) {
      req.contentLength = hdr.substring(15).toInt();
    } else if (lower.startsWith("x-filename:")) {
      req.filename = hdr.substring(11);
      req.filename.trim();
    } else if (lower.startsWith("content-type:")) {
      req.contentType = hdr.substring(13);
      req.contentType.trim();
      int bIdx = req.contentType.indexOf("boundary=");
      if (bIdx >= 0) {
        req.boundary = req.contentType.substring(bIdx + 9);
        req.boundary.trim();
      }
    }
  }
  return true;
}

// ==========================================================================
// REQUEST HANDLER
// ==========================================================================

WiFiServer httpServer(80);

void handleRequest(WiFiClient &client) {
  client.setTimeout(5);

  HttpRequest req;
  if (!parseRequest(client, req)) { client.stop(); return; }

  Serial.println(req.method + " " + req.path);

  // CORS preflight
  if (req.method == "OPTIONS") {
    sendResponse(client, 200, "text/plain", "");
    return;
  }

  // Serve Web UI
  if (req.path == "/" || req.path == "/index.html") {
    String html = String(WEBUI_HTML);
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Content-Length: " + String(html.length()));
    client.println("Connection: close");
    client.println();

    // Send in chunks
    const char *p = html.c_str();
    size_t len = html.length();
    size_t sent = 0;
    while (sent < len) {
      size_t chunk = min((size_t)1024, len - sent);
      client.write((const uint8_t *)(p + sent), chunk);
      sent += chunk;
      yield();
    }
    return;
  }

  // GET /api/status
  if (req.path == "/api/status" && req.method == "GET") {
    String json = "{";
    json += "\"loaded\":" + String(disk_present ? "true" : "false") + ",";
    json += "\"filename\":\"" + jsonEscape(loaded_filename) + "\",";
    json += "\"size\":" + String(loaded_size) + ",";
    json += "\"firmware\":\"" + String(FW_VERSION) + "\",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"free_psram\":" + String(ESP.getFreePsram()) + ",";
    json += "\"wifi_ip\":\"" + wifi_ap_ip + "\",";
    json += "\"wifi_clients\":" + String(WiFi.softAPgetStationNum());
    json += "}";
    sendJSON(client, 200, json);
    return;
  }

  // POST /api/load — receive raw binary disk image
  if (req.path == "/api/load" && req.method == "POST") {
    if (req.contentLength <= 0) {
      sendJSON(client, 400, "{\"error\":\"No data\"}");
      return;
    }
    if (req.contentLength > (int)(RAM_DISK_SIZE - DATA_OFFSET)) {
      // Drain body
      unsigned long t = millis();
      while (client.available() && millis() - t < 3000) { client.read(); yield(); }
      sendJSON(client, 413, "{\"error\":\"File too large. Max 1.44 MB.\"}");
      return;
    }

    String filename = req.filename;
    if (filename.length() == 0) filename = "DISK.ADF";

    Serial.println("Receiving: " + filename + " (" + String(req.contentLength) + " bytes)");
    ledBlink(1, 50);

    // Read directly into PSRAM data area
    int toRead = req.contentLength;
    int pos = 0;
    unsigned long timeout = millis();

    while (pos < toRead && millis() - timeout < 30000) {
      if (client.available()) {
        int n = client.read(&ram_disk[DATA_OFFSET + pos], toRead - pos);
        if (n > 0) { pos += n; timeout = millis(); }
      } else {
        yield();
        delay(1);
      }
    }

    if (pos < toRead) {
      sendJSON(client, 500, "{\"error\":\"Incomplete upload: " + String(pos) + "/" + String(toRead) + "\"}");
      return;
    }

    // Build FAT structure and activate USB
    loadDisk(filename, pos);

    sendJSON(client, 200,
      "{\"status\":\"ok\",\"filename\":\"" + jsonEscape(filename) +
      "\",\"size\":" + String(pos) + "}");
    return;
  }

  // POST /api/eject
  if (req.path == "/api/eject" && req.method == "POST") {
    ejectDisk();
    sendJSON(client, 200, "{\"status\":\"ok\"}");
    return;
  }

  // 404
  sendJSON(client, 404, "{\"error\":\"Not found\"}");
}

// ==========================================================================
// SETUP
// ==========================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Gotek WiFi Dongle ===");
  Serial.println("Firmware: " + String(FW_VERSION));

  // LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  ledBlink(1, 200);

  // Allocate RAM disk in PSRAM
  ram_disk = (uint8_t *)ps_malloc(RAM_DISK_SIZE);
  if (!ram_disk) {
    Serial.println("FATAL: PSRAM allocation failed!");
    while (1) { ledBlink(5, 200); delay(1000); }
  }
  build_empty_volume();
  Serial.println("RAM disk: " + String(RAM_DISK_SIZE / 1024) + " KB allocated in PSRAM");

  // WiFi Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAP(cfg_wifi_ssid.c_str(), cfg_wifi_pass.c_str(), cfg_wifi_channel);
  delay(200);
  wifi_ap_ip = WiFi.softAPIP().toString();
  wifi_ap_active = true;
  Serial.println("WiFi AP: " + cfg_wifi_ssid + " @ " + wifi_ap_ip);

  // HTTP server
  httpServer.begin();
  Serial.println("Web server on port 80");

  // USB Mass Storage
  msc.vendorID("Gotek");
  msc.productID("Disk");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.mediaPresent(false);  // no disk until uploaded
  msc.begin(msc_block_count, 512);
  USB.begin();
  Serial.println("USB MSC ready (no disk)");

  ledBlink(3, 100);
  Serial.println("\nReady! Connect to '" + cfg_wifi_ssid + "' → http://" + wifi_ap_ip);
}

// ==========================================================================
// LOOP
// ==========================================================================

void loop() {
  WiFiClient client = httpServer.available();
  if (client) {
    unsigned long start = millis();
    while (client.connected() && !client.available()) {
      if (millis() - start > 2000) { client.stop(); return; }
      yield();
      delay(1);
    }
    if (client.available()) {
      handleRequest(client);
    }
    delay(1);
    client.stop();
  }
  delay(5);
}
