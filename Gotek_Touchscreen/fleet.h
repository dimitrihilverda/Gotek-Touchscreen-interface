// fleet.h — the GTi wireless fleet, panel side: hear Webby dongle discovery
// beacons, keep a roster, push a disk image over TCP, relay simple commands.
//
// This implements the protocol Mez SHIPPED in Webby-0.7-fleetUI (his tree,
// firmware/Gotek_SuperMini_Webby) rather than the paper contract of issue
// #20 — reality outranks the proposal. Wire facts, read from his source on
// 2026-09-07:
//   - UDP 51703: every dongle broadcasts a JSON "I'm alive" beacon each 12 s
//     ({"gti":1,"id":"<MAC hex>","name":..,"ip":..,"board":..,"fw":..,
//       "hd":..,"port":80,"tcp":3333,"loaded":..,"disk":..}), to the subnet
//     broadcast AND 255.255.255.255. A peer unheard for 40 s (~3 missed
//     beacons) is dropped from the roster.
//   - TCP 3333: 4-byte BIG-endian size + raw image bytes; the dongle answers
//     0x01 + 4-byte LITTLE-endian load id on success, a single 0x00 on error.
//     Size 0xFFFFFFFF escapes to a 1-byte command: 0x01 get-save,
//     0x02 get-status, 0x03 eject, 0x04 force-eject.
//
// The panel deliberately NEVER sends a beacon: the dongles elect their
// master (who claims gotekomega.local) by comparing the MAC ids of every
// "gti":1 beacon they hear — there is no role field. A quietly listening
// panel is harmless; a beaconing panel with a low MAC would win an election
// it does not play in, and gotekomega.local would simply vanish. Speaking
// needs an "electable":false field in the contract first (proposed on #20).

#pragma once
#include <WiFi.h>
#include <WiFiUdp.h>

#define FLEET_DISCO_PORT 51703
#define FLEET_TCP_PORT   3333
#define FLEET_STALE_MS   40000UL
#define FLEET_MAX_PEERS  16

#define FLEET_CMD_GET_SAVE   0x01
#define FLEET_CMD_GET_STATUS 0x02
#define FLEET_CMD_EJECT      0x03
#define FLEET_CMD_EJECT_F    0x04

struct FleetPeer {
  String   id, name, ip, board, fw, disk;
  uint16_t tcp;
  bool     hd, loaded;
  uint32_t seen;
};

static FleetPeer g_fleetPeers[FLEET_MAX_PEERS];
static int       g_fleetPeerN = 0;
static WiFiUDP   g_fleetUdp;
static bool      g_fleetUdpUp = false;

// FLING/command queue: HTTP handlers set these, loop() executes them — the
// non-blocking rule (a transfer inside a handler serves nothing for its
// whole duration, including the endpoint you would debug it with).
static String   g_fleetSendIp;             // queued FLING target, empty = none
static uint16_t g_fleetSendTcp = FLEET_TCP_PORT;
static String   g_fleetCmdIp;              // queued command target, empty = none
static uint8_t  g_fleetCmd     = 0;
static bool     g_fleetBusy    = false;    // loop() is mid-operation
static String   g_fleetLastTarget;         // ip of the last finished operation
static String   g_fleetLastResult;         // "ok" or an error line

// Beacon fields travel the open LAN; escape everything that goes back out as
// JSON, or one dongle named `"};alert(` breaks the whole roster response.
static String fleetJesc(const String &s) {
  String o;
  o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

// The same tiny "key":value extractor the dongle side uses.
static String fleetJf(const String &s, const char *key) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  if (i >= (int)s.length()) return "";
  if (s[i] == '"') {
    const int e = s.indexOf('"', i + 1);
    return e < 0 ? String("") : s.substring(i + 1, e);
  }
  int e = i;
  while (e < (int)s.length() && s[e] != ',' && s[e] != '}') e++;
  return s.substring(i, e);
}

static void fleetPrune() {
  const uint32_t now = millis();
  int w = 0;
  for (int i = 0; i < g_fleetPeerN; i++)
    if (now - g_fleetPeers[i].seen < FLEET_STALE_MS) {
      if (w != i) g_fleetPeers[w] = g_fleetPeers[i];
      w++;
    }
  g_fleetPeerN = w;
}

static void fleetUpsert(const String &j) {
  const String id = fleetJf(j, "id");
  if (id.length() == 0) return;
  FleetPeer *p = nullptr;
  for (int i = 0; i < g_fleetPeerN; i++)
    if (g_fleetPeers[i].id == id) { p = &g_fleetPeers[i]; break; }
  if (!p) {
    if (g_fleetPeerN >= FLEET_MAX_PEERS) return;
    p = &g_fleetPeers[g_fleetPeerN++];
    p->id = id;
  }
  p->name   = fleetJf(j, "name");
  p->ip     = fleetJf(j, "ip");
  p->board  = fleetJf(j, "board");
  p->fw     = fleetJf(j, "fw");
  p->disk   = fleetJf(j, "disk");
  const long t = fleetJf(j, "tcp").toInt();
  p->tcp    = (t > 0 && t < 65536) ? (uint16_t)t : FLEET_TCP_PORT;
  p->hd     = fleetJf(j, "hd") == "true";
  p->loaded = fleetJf(j, "loaded") == "true";
  p->seen   = millis();
}

// Call every loop() pass. Opens the socket once STA WiFi is up; drops the
// socket and the roster when the network goes away.
static void fleetService() {
  if (WiFi.status() != WL_CONNECTED) {
    if (g_fleetUdpUp) {
      g_fleetUdp.stop();
      g_fleetUdpUp = false;
      g_fleetPeerN = 0;
    }
    return;
  }
  if (!g_fleetUdpUp) g_fleetUdpUp = g_fleetUdp.begin(FLEET_DISCO_PORT) != 0;
  if (!g_fleetUdpUp) return;
  for (int guard = 0; guard < 8; guard++) {
    const int sz = g_fleetUdp.parsePacket();
    if (sz <= 0) break;
    char buf[600];
    const int n = g_fleetUdp.read((uint8_t *)buf, sizeof(buf) - 1);
    if (n <= 0) break;
    buf[n] = 0;
    const String s(buf);
    if (s.indexOf("\"gti\":1") < 0) continue;
    fleetUpsert(s);
  }
}

static String fleetRosterJson(const String &selfName, bool busy,
                              const String &lastTarget, const String &lastResult) {
  fleetPrune();
  String j = "{\"self\":\"panel\",\"name\":\"" + fleetJesc(selfName) + "\",";
  j += "\"busy\":" + String(busy ? "true" : "false") + ",";
  j += "\"last_target\":\"" + fleetJesc(lastTarget) + "\",";
  j += "\"last_result\":\"" + fleetJesc(lastResult) + "\",";
  j += "\"devices\":[";
  for (int i = 0; i < g_fleetPeerN; i++) {
    const FleetPeer &p = g_fleetPeers[i];
    if (i) j += ",";
    j += "{\"id\":\"" + fleetJesc(p.id) + "\",\"name\":\"" + fleetJesc(p.name) +
         "\",\"ip\":\"" + fleetJesc(p.ip) + "\",\"board\":\"" + fleetJesc(p.board) +
         "\",\"fw\":\"" + fleetJesc(p.fw) +
         "\",\"hd\":" + (p.hd ? "true" : "false") +
         ",\"loaded\":" + (p.loaded ? "true" : "false") +
         ",\"disk\":\"" + fleetJesc(p.disk) +
         "\",\"tcp\":" + String(p.tcp) +
         ",\"age_s\":" + String((millis() - p.seen) / 1000) + "}";
  }
  j += "]}";
  return j;
}

// Push a raw disk image to a dongle. BLOCKING for the whole transfer —
// call from loop() only, never from an HTTP handler.
static bool fleetSendDisk(const String &ip, uint16_t port,
                          const uint8_t *data, uint32_t size, String &err) {
  WiFiClient c;
  if (!c.connect(ip.c_str(), port, 8000)) { err = "connect failed"; return false; }
  const uint8_t hdr[4] = { (uint8_t)(size >> 24), (uint8_t)(size >> 16),
                           (uint8_t)(size >> 8),  (uint8_t)size };
  c.write(hdr, 4);
  uint32_t sent  = 0;
  uint32_t stall = millis();
  while (sent < size) {
    if (!c.connected()) {
      err = "connection lost at " + String(sent) + "B";
      c.stop();
      return false;
    }
    uint32_t chunk = size - sent;
    if (chunk > 8192) chunk = 8192;
    const size_t w = c.write(data + sent, chunk);
    if (w == 0) {
      if (millis() - stall > 15000) {
        err = "send stalled at " + String(sent) + "B";
        c.stop();
        return false;
      }
      delay(2);
      continue;
    }
    sent += w;
    stall = millis();
  }
  // The dongle answers 0x01 + a 4-byte load id once the image is attached.
  const uint32_t t0 = millis();
  while (c.available() < 1 && millis() - t0 < 10000) {
    if (!c.connected()) break;
    delay(5);
  }
  const int a = c.available() >= 1 ? c.read() : -1;
  c.stop();
  if (a != 0x01) {
    err = (a < 0) ? "no ack from dongle" : "dongle refused (0x" + String(a, HEX) + ")";
    return false;
  }
  return true;
}

// Fire one escape command (eject etc.). BLOCKING but bounded (~8 s worst
// case); call from loop() only.
static bool fleetSendCommand(const String &ip, uint16_t port, uint8_t cmd, String &err) {
  WiFiClient c;
  if (!c.connect(ip.c_str(), port, 5000)) { err = "connect failed"; return false; }
  const uint8_t f[5] = { 0xFF, 0xFF, 0xFF, 0xFF, cmd };
  c.write(f, 5);
  const uint32_t t0 = millis();
  while (c.available() < 1 && millis() - t0 < 3000) {
    if (!c.connected()) break;
    delay(5);
  }
  const int a = c.available() >= 1 ? c.read() : -1;
  c.stop();
  if (a < 0) { err = "no reply"; return false; }
  return true;
}
