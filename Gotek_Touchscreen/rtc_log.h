#pragma once
//
// A log that survives the reboot you are trying to explain.
//
// LOG.TXT is cleared on every boot, which is the right thing for a log you
// read while the device is running and exactly the wrong thing for a crash:
// the reboot wipes the evidence and you come back to a file containing nothing
// but the startup lines. That reads as "logging is broken" and is actually
// "the device restarted".
//
// So: a small ring buffer in RTC memory, and NOINIT specifically. RTC_NOINIT
// is kept across esp_restart() and across a panic, and is only lost when power
// actually goes away. At boot the survivors are replayed into the fresh
// LOG.TXT, so the file starts with what happened just before the crash.
//
// This is the pattern that turned "the log shows nothing" into a diagnosis on
// the dongle. That board still carries its own copy inside dongle_config.h;
// this header is the shared version it should move to, once it is not in the
// middle of being debugged.
//
// Sizing: 40 x 96 is 3840 bytes. RTC slow memory is 8 KB in total and the
// system wants some of it, so this is not a buffer to grow casually.

#include <Arduino.h>

#define RTCLOG_LINES     40
#define RTCLOG_LINE_LEN  96
#define RTCLOG_MAGIC     0x60EC1067u

RTC_NOINIT_ATTR static char     g_rtcLog[RTCLOG_LINES][RTCLOG_LINE_LEN];
RTC_NOINIT_ATTR static uint8_t  g_rtcLogHead;
RTC_NOINIT_ATTR static uint8_t  g_rtcLogCount;
RTC_NOINIT_ATTR static uint32_t g_rtcLogMagic;

// Writes are refused until rtcLogReset() arms them. That is deliberate: boot
// messages must not overwrite the survivors before anyone has read them.
static bool g_rtcLogArmed = false;

// Call first thing in setup(), before anything logs.
void rtcLogBegin() {
  if (g_rtcLogMagic != RTCLOG_MAGIC) {   // cold start: the buffer is garbage
    g_rtcLogMagic  = RTCLOG_MAGIC;
    g_rtcLogHead   = 0;
    g_rtcLogCount  = 0;
    memset(g_rtcLog, 0, sizeof(g_rtcLog));
  }
  // Paranoia: a corrupted index would walk off the array.
  if (g_rtcLogHead  >= RTCLOG_LINES) g_rtcLogHead  = 0;
  if (g_rtcLogCount >  RTCLOG_LINES) g_rtcLogCount = RTCLOG_LINES;
  g_rtcLogArmed = false;
}

uint8_t rtcLogCount() { return g_rtcLogCount; }

// Oldest first, so a replay reads in the order things happened.
const char *rtcLogLine(uint8_t i) {
  if (i >= g_rtcLogCount) return "";
  const uint8_t start = (uint8_t)((g_rtcLogHead + RTCLOG_LINES - g_rtcLogCount) % RTCLOG_LINES);
  return g_rtcLog[(start + i) % RTCLOG_LINES];
}

// Drop the survivors and start accepting new lines. Call once the old ones
// have been written somewhere durable.
void rtcLogReset() {
  g_rtcLogHead  = 0;
  g_rtcLogCount = 0;
  memset(g_rtcLog, 0, sizeof(g_rtcLog));
  g_rtcLogArmed = true;
}

void rtcLogAdd(const String &msg) {
  if (!g_rtcLogArmed) return;
  strncpy(g_rtcLog[g_rtcLogHead], msg.c_str(), RTCLOG_LINE_LEN - 1);
  g_rtcLog[g_rtcLogHead][RTCLOG_LINE_LEN - 1] = 0;
  g_rtcLogHead = (uint8_t)((g_rtcLogHead + 1) % RTCLOG_LINES);
  if (g_rtcLogCount < RTCLOG_LINES) g_rtcLogCount++;
}
