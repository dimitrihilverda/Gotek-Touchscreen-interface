#pragma once
//
// Board profiles and the FAT12 volume geometry they imply.
//
// Split out of the sketch so the numbers can be checked on the host for every
// target at once — see tools/test/test_board_profiles.cpp — and so a screenless
// build can pick up the same definitions without dragging in the display code.
//
// Select with -DACTIVE_BOARD=<n>. Adding a board is a block below, not a new
// #ifdef somewhere in 6800 lines.

// ============================================================================
// BOARD PROFILES
// ============================================================================
//
// One place that answers everything that differs between targets. The selector
// used to be the DISPLAY, which stops working the moment a board has none: on a
// screenless dongle the panel is one absent feature among several, not the axis
// the build turns on.
//
// A new board is a block below, not #ifdefs scattered through 6800 lines.
//
// The volume size is dictated by PSRAM, and it is the one number that cannot be
// wished away: the RAM disk is a single contiguous allocation, so a 2 MB volume
// needs a part with more than 2 MB of PSRAM. See the per-board notes.

#define BOARD_JC3248     1   // Guition JC3248W535C — 8MB PSRAM, 480x320, SD
#define BOARD_WAVESHARE  2   // Waveshare ESP32-S3-Touch-LCD-2.8 — 320x240, SD
#define BOARD_XIAO       3   // Seeed XIAO ESP32S3 — 8MB PSRAM, no screen, no SD
#define BOARD_SUPERMINI  4   // ESP32-S3FH4R2 SuperMini — 2MB PSRAM, no screen, no SD

// Kept so the display-conditional code below reads naturally.
#define DISPLAY_NONE      0
#define DISPLAY_JC3248    1
#define DISPLAY_WAVESHARE 2

// SELECT YOUR BOARD HERE.
// The CI workflow (.github/workflows/build-release.yml) overrides this via
// --build-property to produce per-variant release binaries — leave the
// #ifndef guard in place or those builds will all turn into JC3248.
#ifndef ACTIVE_BOARD
  // Compatibility: older build commands pass -DACTIVE_DISPLAY. Honour it.
  #if defined(ACTIVE_DISPLAY) && ACTIVE_DISPLAY == DISPLAY_WAVESHARE
    #define ACTIVE_BOARD BOARD_WAVESHARE
  #else
    #define ACTIVE_BOARD BOARD_JC3248
  #endif
#endif

#if ACTIVE_BOARD == BOARD_JC3248
  #define BOARD_NAME      "JC3248"
  #define HAS_DISPLAY     1
  #define HAS_TOUCH       1
  #define HAS_SD          1
  #define SECTORS_TOTAL   4096   // 2 MB volume — 8 MB PSRAM, room to spare
  #define SECTORS_PER_FAT 12
  #define SUPPORTS_HD     1
  #undef  ACTIVE_DISPLAY
  #define ACTIVE_DISPLAY  DISPLAY_JC3248

#elif ACTIVE_BOARD == BOARD_WAVESHARE
  #define BOARD_NAME      "WAVESHARE"
  #define HAS_DISPLAY     1
  #define HAS_TOUCH       1
  #define HAS_SD          1
  #define SECTORS_TOTAL   4096
  #define SECTORS_PER_FAT 12
  #define SUPPORTS_HD     1
  #undef  ACTIVE_DISPLAY
  #define ACTIVE_DISPLAY  DISPLAY_WAVESHARE

#elif ACTIVE_BOARD == BOARD_XIAO
  // 8 MB octal PSRAM, so full parity with the big boards. Needs PSRAM=OPI.
  #define BOARD_NAME      "XIAO"
  #define HAS_DISPLAY     0
  #define HAS_TOUCH       0
  #define HAS_SD          0
  #define SECTORS_TOTAL   4096
  #define SECTORS_PER_FAT 12
  #define SUPPORTS_HD     1
  #undef  ACTIVE_DISPLAY
  #define ACTIVE_DISPLAY  DISPLAY_NONE

#elif ACTIVE_BOARD == BOARD_SUPERMINI
  // ESP32-S3FH4R2: 2 MB QUAD PSRAM — set PSRAM=QSPI, not OPI. Get that wrong
  // and the chip reports no PSRAM at all, which looks like a dead board.
  //
  // 2 MB of PSRAM cannot host a 2 MB volume: the RAM disk is one contiguous
  // allocation and there would be nothing left for the heap. So 1 MB, which
  // holds a standard 880 KB DD image but not an HD or PC 1.44 MB one.
  #define BOARD_NAME      "SUPERMINI"
  #define HAS_DISPLAY     0
  #define HAS_TOUCH       0
  #define HAS_SD          0
  #define SECTORS_TOTAL   2048
  #define SECTORS_PER_FAT 6
  #define SUPPORTS_HD     0
  #undef  ACTIVE_DISPLAY
  #define ACTIVE_DISPLAY  DISPLAY_NONE

#else
  #error "ACTIVE_BOARD is not a known board - see the BOARD_* list above"
#endif

// ── FAT12 volume geometry ───────────────────────────────────────────────
//
// Exactly one file ever lives on this volume, so the layout is chosen to
// maximise data space rather than to imitate a real floppy.
//
// SECTORS_TOTAL and SECTORS_PER_FAT come from the board profile: how large the
// volume can be is a property of the board's PSRAM, not of this layout. The
// 8 MB boards get 4096 sectors (2 MB), enough for an Amiga HD image at 1760 KB;
// a 2 MB SuperMini gets half that, and HD does not fit.
//
// Every offset below is derived, and the layout is checked at compile time —
// at 1 sector per cluster a 4096-sector volume lands at 4057 clusters, under
// the FAT12 limit of 4085 but close enough that a change to ROOT_ENTRIES or
// SECTORS_PER_FAT could tip it into a volume a reader treats as FAT16.
#define SECTORS_RESERVED   1
#define SECTORS_PER_CLUST  1
#define NUM_FATS           2
#define ROOT_ENTRIES       224
#define ROOTDIR_SECTORS    (ROOT_ENTRIES * 32 / 512)
#define MEDIA_DESCRIPTOR   0xF8            // fixed disk; 0xF0 would claim 1.44 MB floppy

#define RAM_DISK_SIZE      (SECTORS_TOTAL * 512)
// Volume layout, all derived. On a 4096-sector board that works out as:
//   sector 0        boot sector
//   sectors 1-12    FAT1
//   sectors 13-24   FAT2
//   sectors 25-38   root directory (224 entries)
//   sectors 39+     data — the mounted image, ~1.98 MB usable
#define FAT1_OFFSET    (SECTORS_RESERVED * 512)
#define FAT2_OFFSET    (FAT1_OFFSET + SECTORS_PER_FAT * 512)
#define ROOTDIR_OFFSET (FAT2_OFFSET + SECTORS_PER_FAT * 512)
#define DATA_OFFSET    (ROOTDIR_OFFSET + ROOTDIR_SECTORS * 512)
#define DATA_LBA       (DATA_OFFSET / 512)
#define MAX_IMAGE_BYTES (RAM_DISK_SIZE - DATA_OFFSET)

// A FAT12 volume must stay under 4085 clusters; at 4085 or more a reader is
// entitled to treat it as FAT16 and see garbage.
static_assert((SECTORS_TOTAL - SECTORS_RESERVED - NUM_FATS * SECTORS_PER_FAT
               - ROOTDIR_SECTORS) / SECTORS_PER_CLUST < 4085,
              "FAT12 cluster limit exceeded - raise SECTORS_PER_CLUST");
// ...and the FAT has to be big enough to describe them, 1.5 bytes per cluster.
static_assert(((SECTORS_TOTAL - SECTORS_RESERVED - NUM_FATS * SECTORS_PER_FAT
               - ROOTDIR_SECTORS) / SECTORS_PER_CLUST + 2) * 3 / 2
              <= SECTORS_PER_FAT * 512,
              "SECTORS_PER_FAT too small for the cluster count");
// A standard double-density Amiga image is 901,120 bytes. A board that cannot
// hold one is not a Gotek companion, whatever else it can do.
static_assert(MAX_IMAGE_BYTES >= 901120,
              "volume too small for a standard DD image - board profile is wrong");
#if SUPPORTS_HD
static_assert(MAX_IMAGE_BYTES >= 1802240,
              "profile claims HD support but the volume cannot hold 1760 KB");
#endif
