#pragma once
//
// The RAM disk: a FAT12 volume in PSRAM holding exactly one disk image, served
// to the Gotek over USB Mass Storage.
//
// Split out of the sketch because it is the whole product on a screenless
// dongle, where none of the display or SD code around it exists. Nothing in
// here touches the panel or the card; writing a save to SD is the sketch's job,
// reached through the dirty map this module maintains.
//
// Geometry comes from board_profile.h — how large the volume can be is a
// property of the board's PSRAM.

#include <Arduino.h>
#include "USBMSC.h"
#include "board_profile.h"

// TinyUSB's connect/disconnect. Detaching before rewriting the volume is what
// stops the Gotek reading a half-swapped image, so it belongs with the disk.
extern "C" {
  bool tud_mounted(void);
  void tud_disconnect(void);
  void tud_connect(void);
  void *ps_malloc(size_t size);
}

// RAM disk variables
String g_mountFilename = "";

// ── Disk saves ───────────────────────────────────────────────────────────
//
// When the Amiga writes to the mounted disk — a high score, a save game, a
// preferences file — the Gotek writes into the image FILE on this RAM volume.
// Those writes used to live and die in RAM: onWrite copied them into the buffer
// and nothing ever put them anywhere. The Amiga believed it had saved.
//
// So: mark which image sectors were touched, wait for the writes to stop, and
// put them on the card. A settle timer rather than an immediate write, because
// a single save is a burst of dozens of sector writes and flushing on each one
// would hammer the card and stall USB.
//
// COPY is the default deliberately: it writes <name>.sav.adf beside the
// original and leaves your collection untouched. OVERWRITE is opt-in.
enum SavesMode { SAVES_OFF = 0, SAVES_COPY = 1, SAVES_OVERWRITE = 2 };
int    cfg_saves_mode  = SAVES_COPY;

#define SV_SETTLE_MS   3000
#define SV_IMG_SECTORS (SECTORS_TOTAL - DATA_LBA)

// The map has to cover every sector onWrite can possibly mark, or a save to the
// tail of a full-size image would write past the end of the bitmap.
static_assert((MAX_IMAGE_BYTES + 511) / 512 <= SV_IMG_SECTORS,
              "dirty map too small for the largest image the volume accepts");
static uint8_t  g_sv_dirty[(SV_IMG_SECTORS + 7) / 8];
static volatile uint16_t g_sv_dirty_count = 0;
static volatile uint32_t g_sv_last_write  = 0;
String   g_mountPath  = "";     // SD path of the mounted image, "" if remote or none
String   g_mountLabel = "";     // what to call it in a saves filename
uint32_t g_mountBytes = 0;      // image size, so we never write past its end

static inline bool svGet(uint32_t i) { return (g_sv_dirty[i >> 3] >> (i & 7)) & 1; }
static inline void svSet(uint32_t i) { g_sv_dirty[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static void svReset() {
  memset(g_sv_dirty, 0, sizeof(g_sv_dirty));
  g_sv_dirty_count = 0;
  g_sv_last_write  = 0;
}

uint8_t *ram_disk = NULL;
const char *ram_mount_point = "/ramdisk";

// USB MSC
USBMSC msc;
uint32_t msc_block_count;

// ============================================================================
// FAT12 FILESYSTEM EMULATION
// ============================================================================

void build_boot_sector(uint8_t *buf) {
  memset(buf, 0, 512);
  buf[0x00] = 0xEB; buf[0x01] = 0x3C; buf[0x02] = 0x90;
  memcpy(&buf[0x03], "MSDOS5.0", 8);
  *(uint16_t *)&buf[0x0B] = 512;                    // bytes per sector
  buf[0x0D] = SECTORS_PER_CLUST;
  *(uint16_t *)&buf[0x0E] = SECTORS_RESERVED;
  buf[0x10] = NUM_FATS;
  *(uint16_t *)&buf[0x11] = ROOT_ENTRIES;
  *(uint16_t *)&buf[0x13] = SECTORS_TOTAL;
  buf[0x15] = MEDIA_DESCRIPTOR;
  *(uint16_t *)&buf[0x16] = SECTORS_PER_FAT;
  *(uint16_t *)&buf[0x18] = 18;                     // geometry hints, unused by
  *(uint16_t *)&buf[0x1A] = 2;                      // anything that reads this
  *(uint32_t *)&buf[0x20] = 0;
  buf[0x24] = 0x00;  // BS_DrvNum: 0x00 = floppy
  buf[0x25] = 0x00;  // BS_Reserved1
  buf[0x26] = 0x29;  // BS_BootSig: marks volume label & FS type as valid
  buf[0x27] = 0x47;  // BS_VolID (serial number bytes)
  buf[0x28] = 0x4F;
  buf[0x29] = 0x54;
  buf[0x2A] = 0x4B;
  memcpy(&buf[0x2B], "GOTEK      ", 11);
  memcpy(&buf[0x36], "FAT12   ", 8);
  buf[510] = 0x55;
  buf[511] = 0xAA;
}

void fat12_set(uint8_t *fat, int idx, uint16_t val) {
  if (idx % 2 == 0) {
    fat[idx * 3 / 2] = val & 0xFF;
    fat[idx * 3 / 2 + 1] = (fat[idx * 3 / 2 + 1] & 0xF0) | ((val >> 8) & 0x0F);
  } else {
    fat[idx * 3 / 2] = (fat[idx * 3 / 2] & 0x0F) | ((val & 0x0F) << 4);
    fat[idx * 3 / 2 + 1] = (val >> 4) & 0xFF;
  }
}

void build_fat(uint8_t *fat) {
  memset(fat, 0, SECTORS_PER_FAT * 512);
  fat12_set(fat, 0, 0xF00 | MEDIA_DESCRIPTOR);   // entry 0 mirrors the media byte
  fat12_set(fat, 1, 0xFFF);
  // Cluster 2+ left as 0x000 (free) until a file is loaded
}

void make_83_name(const char *src, uint8_t *dst) {
  memset(dst, ' ', 11);
  // Find last dot for extension
  const char *dot = strrchr(src, '.');
  int nameLen = dot ? (int)(dot - src) : (int)strlen(src);
  // Copy name part (max 8 chars)
  for (int i = 0, j = 0; i < nameLen && j < 8; i++) {
    dst[j++] = toupper(src[i]);
  }
  // Copy extension (max 3 chars)
  if (dot) {
    dot++;
    for (int j = 8; *dot && j < 11; dot++) {
      dst[j++] = toupper(*dot);
    }
  }
}

void build_root(uint8_t *root) {
  memset(root, 0, ROOTDIR_SECTORS * 512);
  // An ejected drive has no file at all. Writing a zero-length entry for the
  // previous image left the Amiga looking at a 0 KB ghost of the last game.
  if (g_mountFilename.length() == 0) return;
  uint8_t fname[11];
  make_83_name(g_mountFilename.c_str(), fname);
  memcpy(&root[0], fname, 11);
  root[11] = 0x20;             // Archive attribute
  *(uint16_t *)&root[26] = 0;  // Start cluster = 0 (no data yet)
  *(uint32_t *)&root[28] = 0;  // File size = 0
}


void build_volume_with_file() {
  memset(ram_disk, 0, RAM_DISK_SIZE);
  build_boot_sector(&ram_disk[0]);
  build_fat(&ram_disk[FAT1_OFFSET]);
  build_fat(&ram_disk[FAT2_OFFSET]);
  build_root(&ram_disk[ROOTDIR_OFFSET]);

  msc_block_count = RAM_DISK_SIZE / 512;
}

// ============================================================================
// USB MSC CALLBACKS
// ============================================================================

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
  if (!ram_disk || addr + bufsize > RAM_DISK_SIZE) return -1;
  memcpy(&ram_disk[addr], buffer, bufsize);

  // Note which sectors of the IMAGE this touched. Writes to the boot sector,
  // the FATs or the directory are the Gotek keeping its own housekeeping up to
  // date; they are not part of the .adf and must not be written back into it.
  if (cfg_saves_mode != SAVES_OFF && g_mountBytes > 0) {
    const uint32_t first = addr / 512;
    const uint32_t last  = (addr + bufsize - 1) / 512;
    const uint32_t imgEnd = DATA_LBA + (g_mountBytes + 511) / 512;
    bool touchedImage = false;
    for (uint32_t sec = first; sec <= last; sec++) {
      if (sec < DATA_LBA || sec >= imgEnd) continue;
      touchedImage = true;
      const uint32_t i = sec - DATA_LBA;
      // Assignment rather than ++: C++20 deprecates ++ on a volatile.
      if (!svGet(i)) { svSet(i); g_sv_dirty_count = g_sv_dirty_count + 1; }
    }
    // Only image writes hold the settle window open. Letting housekeeping
    // writes restart the timer would let a chatty Gotek postpone the save
    // indefinitely, which is the one way this feature quietly does nothing.
    if (touchedImage) g_sv_last_write = millis();
  }
  return bufsize;
}

