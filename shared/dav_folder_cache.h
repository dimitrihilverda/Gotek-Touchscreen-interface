#pragma once
/*
  shared/dav_folder_cache.h
  =========================
  Per-folder WebDAV disk-list cache.
  Stores: HOST, DISK entries, COVER, NFO per folder.

  Storage backend is selected by a define in the device .ino:

    #define DAV_CACHE_FS  SD_MMC            // JC3248 — SD card
    #define DAV_CACHE_DIR "/DAV_FOLDER_CACHE"

    or

    #define DAV_CACHE_FS  SPIFFS            // Dongle — SPIFFS flash
    #define DAV_CACHE_DIR ""               // flat SPIFFS: use filename prefix

  File format:
    HOST=<dav_host>
    DISK=<filename>   (repeated for each disk image)
    COVER=<filename>  (or empty line)
    NFO=<filename>    (or empty line)

  Invalidates automatically when DAV_HOST changes.
  cfg_dav_host must be declared by the device .ino.
*/

#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Build the cache file path for a given folder
// ─────────────────────────────────────────────────────────────────────────────
inline String davFolderCachePath(const String &folderPath) {
#ifdef DAV_CACHE_DIR
  #if defined(DAV_CACHE_FS_IS_SPIFFS)
    // SPIFFS: flat namespace, prefix + sanitised name (max 28 chars path)
    String safe = "";
    for (unsigned int i = 0; i < folderPath.length() && (int)safe.length() < 20; i++) {
      char c = folderPath[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-') safe += c;
      else safe += '_';
    }
    return String("/dfc_") + safe + ".txt";
  #else
    // SD-MMC: use directory + sanitised folder name
    String safe = folderPath;
    // Replace leading slash for sub-path use
    while (safe.startsWith("/")) safe = safe.substring(1);
    // Replace remaining slashes with underscore
    safe.replace("/", "_");
    if (safe.length() == 0) safe = "root";
    if (safe.length() > 50) safe = safe.substring(0, 50);
    return String(DAV_CACHE_DIR) + "/" + safe + ".txt";
  #endif
#endif
  return "";
}

// ─────────────────────────────────────────────────────────────────────────────
// Save folder cache
// ─────────────────────────────────────────────────────────────────────────────
inline void davSaveFolderCache(const String &folderPath,
                                const std::vector<String> &disks,
                                const String &coverFile,
                                const String &nfoFile) {
  String path = davFolderCachePath(folderPath);
  if (path.length() == 0) return;

#if defined(DAV_CACHE_FS_IS_SPIFFS)
  if (!DAV_CACHE_FS.exists("/")) return;   // SPIFFS not mounted
#else
  // Ensure directory exists (SD)
  if (!DAV_CACHE_FS.exists(DAV_CACHE_DIR)) {
    DAV_CACHE_FS.mkdir(DAV_CACHE_DIR);
  }
#endif

  File f = DAV_CACHE_FS.open(path.c_str(), "w");
  if (!f) return;
  f.println("HOST=" + cfg_dav_host);
  for (const auto &d : disks) f.println("DISK=" + d);
  f.println("COVER=" + coverFile);
  f.println("NFO="   + nfoFile);
  f.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Load folder cache
// ─────────────────────────────────────────────────────────────────────────────
inline bool davLoadFolderCache(const String &folderPath,
                                std::vector<String> &disks,
                                String &coverFile,
                                String &nfoFile) {
  String path = davFolderCachePath(folderPath);
  if (path.length() == 0) return false;
  if (!DAV_CACHE_FS.exists(path.c_str())) return false;

  File f = DAV_CACHE_FS.open(path.c_str(), "r");
  if (!f) return false;

  disks.clear(); coverFile = ""; nfoFile = "";
  bool hostOk = false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if      (line.startsWith("HOST="))  { hostOk = (line.substring(5) == cfg_dav_host); }
    else if (line.startsWith("DISK=")  && hostOk) { String d = line.substring(5); if (d.length() > 0) disks.push_back(d); }
    else if (line.startsWith("COVER=") && hostOk) { coverFile = line.substring(6); }
    else if (line.startsWith("NFO=")   && hostOk) { nfoFile   = line.substring(4); }
  }
  f.close();

  if (!hostOk) { disks.clear(); coverFile = ""; nfoFile = ""; return false; }
  return true;
}
