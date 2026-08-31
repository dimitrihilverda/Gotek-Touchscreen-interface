#pragma once
//
// Power save — go quiet when the Amiga is not there.
//
// This only means anything on a board with a LiPo fitted to P5. Without a cell,
// losing USB power means losing power, and there is nothing left running to
// switch off. With one, the device cheerfully keeps its access point up for
// hours in a drawer and you come back to a flat battery — which turns the cell
// from a fix for brownouts into a new daily chore.
//
// Off by default. It has to be asked for with POWER_SAVE=1 in CONFIG.TXT,
// because the detection below has never run on hardware with a battery fitted.
// See the note on psHostPresent().
//
// Include AFTER webserver.h: it uses stopWiFiAP(), initWiFiAP() and the
// wifi_ap_active / wifi_sta_connected flags that live there.

#include <Arduino.h>

// ── The ladder ───────────────────────────────────────────────────────────
//
// Three steps rather than one hard cut, each taking out the largest remaining
// consumer. Minutes apart, so a Gotek that simply has not read a sector for a
// while is never mistaken for a Gotek that has been unplugged.
#define PS_DIM_MS    (1UL * 60 * 1000)   // backlight off
#define PS_RADIO_MS  (2UL * 60 * 1000)   // WiFi + web server down
#define PS_DOZE_MS   (5UL * 60 * 1000)   // CPU down, loop throttled

#define PS_DOZE_MHZ  80
#define PS_FULL_MHZ  240

enum PsStage : uint8_t {
  PS_ACTIVE = 0,
  PS_DIMMED = 1,
  PS_RADIO_OFF = 2,
  PS_DOZING = 3
};

static PsStage  psStage        = PS_ACTIVE;
static uint32_t psLastActiveMs = 0;
static bool     psWifiWasUp    = false;

// ── Is a host out there? ─────────────────────────────────────────────────
//
// tud_mounted() is the real signal: it is true while a USB host has us
// enumerated and configured, and it should go false when VBUS disappears and
// only the battery is holding us up. THAT is the untested half — on a board
// powered from the same port it is watching, the case never arises, so nobody
// has ever seen it happen. Verify it before trusting this feature.
//
// Sector reads are the second opinion. A disk swap calls tud_disconnect() for
// a moment, and without this a swap right on a threshold could read as an
// unplug; a read within the last half minute outlives that gap.
static bool psHostPresent() {
  if (tud_mounted()) return true;
  return g_lastHostReadMs != 0 && (millis() - g_lastHostReadMs) < 30000UL;
}

// Everything comes back in the reverse order it went away. Paint first, then
// fade the backlight in, so the panel never shows the stale frame it went to
// sleep on.
static void psWakeUp() {
  if (psStage == PS_ACTIVE) return;

  if (psStage >= PS_DOZING) setCpuFrequencyMhz(PS_FULL_MHZ);

  if (psStage >= PS_RADIO_OFF && psWifiWasUp) {
    initWiFiAP();
    startWebServer();
  }
  psWifiWasUp = false;

  if (psStage >= PS_DIMMED) {
    redrawCurrentScreen();
    rampBacklight(cfg_backlight);
  }

  psStage = PS_ACTIVE;
  sdLog("Power save: awake");
}

void powerSaveInit() {
  psLastActiveMs = millis();
  psStage = PS_ACTIVE;
  if (cfg_power_save) {
    sdLog("Power save: armed (only useful with a battery on P5)");
  }
}

// Called once per loop, with whether a finger is on the glass.
//
// Web traffic counts as activity too. Without that, browsing from a phone with
// no Amiga attached would dim the screen and then cut the radio out from under
// the very session doing the browsing — which is precisely the bench case the
// battery makes possible in the first place.
void powerSaveService(bool userActive) {
  if (cfg_power_save == 0) {
    if (psStage != PS_ACTIVE) psWakeUp();
    return;
  }

  const uint32_t now = millis();
  const bool webBusy = g_lastWebActivityMs != 0 &&
                       (now - g_lastWebActivityMs) < 30000UL;

  if (userActive || webBusy || psHostPresent()) {
    psLastActiveMs = now;
    if (psStage != PS_ACTIVE) psWakeUp();
    return;
  }

  const uint32_t idle = now - psLastActiveMs;

  if (psStage < PS_DIMMED && idle >= PS_DIM_MS) {
    setBacklight(0);
    psStage = PS_DIMMED;
    sdLog("Power save: no host for 1 min, backlight off");
  }

  if (psStage < PS_RADIO_OFF && idle >= PS_RADIO_MS) {
    psWifiWasUp = wifi_ap_active || wifi_sta_connected;
    if (psWifiWasUp) {
      stopWebServer();
      stopWiFiAP();
    }
    psStage = PS_RADIO_OFF;
    sdLog("Power save: radio off");
  }

  if (psStage < PS_DOZING && idle >= PS_DOZE_MS) {
    setCpuFrequencyMhz(PS_DOZE_MHZ);
    psStage = PS_DOZING;
    sdLog("Power save: dozing at " + String(PS_DOZE_MHZ) + " MHz");
  }

  // Throttle the loop while dozing. Touch polling drops to ~20 Hz, which is
  // imperceptible on a wake-up gesture and is the difference between a loop
  // spinning flat out and one that is mostly idle.
  if (psStage == PS_DOZING) delay(50);
}
