#!/usr/bin/env python
"""Every board profile must describe a legal FAT12 volume.

The geometry is a web of derived constants: change ROOT_ENTRIES or
SECTORS_PER_FAT and the cluster count moves, and at 4085 clusters or more a
reader is entitled to treat the volume as FAT16 and hand the Amiga garbage.

board_profile.h carries static_asserts, but those only fire for the board being
compiled. This checks every board at once, so a profile added with a broken
layout fails here instead of on someone's desk.
"""
import re
import sys

HDR = "Gotek_Touchscreen/board_profile.h"


def shared_constants(text):
    """The constants outside the per-board blocks."""
    out = {}
    for name in ("SECTORS_RESERVED", "SECTORS_PER_CLUST", "NUM_FATS", "ROOT_ENTRIES"):
        m = re.search(r"^#define\s+%s\s+(\d+)" % name, text, re.M)
        if not m:
            sys.exit("could not find #define %s in %s" % (name, HDR))
        out[name] = int(m.group(1))
    return out


def board_blocks(text):
    """Split out each `#if/#elif ACTIVE_BOARD == BOARD_x` block."""
    blocks = {}
    parts = re.split(r"^#(?:el)?if ACTIVE_BOARD == (BOARD_\w+)\s*$", text, flags=re.M)
    for i in range(1, len(parts), 2):
        name, body = parts[i], parts[i + 1]
        body = body.split("#else")[0].split("#endif")[0]
        vals = dict(re.findall(r"#define\s+(\w+)\s+(\S+)", body))
        blocks[name] = vals
    return blocks


def main():
    text = open(HDR, encoding="utf-8").read()
    shared = shared_constants(text)
    boards = board_blocks(text)
    if len(boards) < 2:
        sys.exit("parsed %d board blocks from %s - the format must have changed"
                 % (len(boards), HDR))

    rootdir_sectors = shared["ROOT_ENTRIES"] * 32 // 512
    failures = 0
    print("Board profiles")
    print("==============")

    for name, v in boards.items():
        def need(key):
            if key not in v:
                nonlocal failures
                failures += 1
                print("  FAIL [%s]: profile is missing %s" % (name, key))
                return None
            return v[key]

        total = need("SECTORS_TOTAL")
        spf = need("SECTORS_PER_FAT")
        hd = need("SUPPORTS_HD")
        if total is None or spf is None or hd is None:
            continue
        total, spf, hd = int(total), int(spf), int(hd)

        data_off = (shared["SECTORS_RESERVED"] + shared["NUM_FATS"] * spf
                    + rootdir_sectors) * 512
        usable = total * 512 - data_off
        clusters = (total - shared["SECTORS_RESERVED"] - shared["NUM_FATS"] * spf
                    - rootdir_sectors) // shared["SECTORS_PER_CLUST"]

        def check(cond, what):
            nonlocal failures
            if not cond:
                failures += 1
                print("  FAIL [%s]: %s" % (name, what))

        check(0 < clusters < 4085, "cluster count %d is outside 1..4084 (FAT12)" % clusters)
        check((clusters + 2) * 3 // 2 <= spf * 512,
              "FAT of %d sectors cannot describe %d clusters" % (spf, clusters))
        check(usable >= 901120,
              "cannot hold a standard 880 KB DD image (%d usable)" % usable)
        if hd:
            check(usable >= 1802240,
                  "claims HD support but only %d bytes usable" % usable)

        print("  %-16s %5d sectors  %8d usable  %5d clusters  HD:%s  screen:%s  SD:%s"
              % (name, total, usable, clusters,
                 "yes" if hd else "no ",
                 "yes" if v.get("HAS_DISPLAY") == "1" else "no ",
                 "yes" if v.get("HAS_SD") == "1" else "no "))

    print("%d board(s) checked, %d failure(s)" % (len(boards), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
