# Bringing the network layer into GTi

**From:** Dimitri (OMEGAWARE) — `dimitrihilverda/Gotek-Touchscreen-interface`
**To:** Mez (`mesarim/Gotek-Touchscreen-interface`), and whoever is coding with you
**Status:** proposal, nothing pushed

> **Updated since the first draft (v0.25.0):** four things below were true when
> written and are not anymore, kept visible rather than silently rewritten.
>
> - **OTA exists on my side now** — firmware upload over the web interface, on
>   all four boards, streaming into the inactive slot with a magic-byte check.
>   Verified with five over-the-air flashes on real JC3248 hardware in one
>   evening. Your OTA-from-SD stays complementary for offline use.
> - **The screenless dongle is real**, not a question: SuperMini and XIAO
>   builds, no display, no SD, WebDAV + upload through the same web UI the
>   touchscreen serves. That answers my own question 3 below — WebDAV in a
>   dongle works, and the 5 V concern is handled (TX power cap, and it runs
>   happily on Amiga power with a 470 uF cap on the rail).
> - **HD images load now** — board profiles pick the volume geometry at compile
>   time (2 MB on 8 MB-PSRAM boards), so "my RAM disk is fixed at 1.44 MB"
>   below is stale.
> - **The client you'd be taking just survived a real hunt.** `_release()` had
>   an unbounded recursion that panicked at the end of any transfer whose
>   connection the server closed; found via a crash-surviving RTC log and
>   per-128 KB breadcrumbs, fixed, and verified with repeated inserts on two
>   boards. The pooled connection also learned to release its ~50 KB of
>   internal heap after 15 s idle. Tag `v0.25.0` carries all of it.

---

## The ask, in one paragraph

You want the WebDAV and web-interface work. I'd like to bring it over. My
proposal is that **your tree is the base** and my network layer arrives as a
**shared module included by every board sketch**, not as a copy pasted into each
one. The first step is deliberately small: one client file, no UI, no web
server, almost no contact with your globals. If that lands cleanly on two
boards, everything after it is a repeat of the same pattern.

I have push access to your repo (`push: true`). I have not used it and won't
until we've agreed on the shape.

---

## What I measured

Numbers first, so nobody has to take my word for it. Every one of these is
reproducible; the commands are at the bottom.

| Fact | Value |
|---|---|
| Common git ancestor between our trees | **none** — `git merge-base` returns nothing |
| Function names shared (your JC3248 ↔ my sketch) | 43 of 226 / 163 |
| `Gotek_JC3248.ino` ↔ `Gotek_JC4827W543.ino` identical lines | **3738 of 3852** |
| Function names shared between those two | **226 of 226** |
| `Gotek_JC3248.ino` ↔ `Gotek_7B.ino` identical lines | 898 |
| WebDAV/FTP/web-server hits in your JC3248 sketch | **0** |
| ESP-NOW hits in my sketch | **0** (OTA: added since — see the update note) |

Two things follow from that table.

**We barely overlap.** You built outward across hardware — seven targets, the
dongle protocol, OTA from SD, HD floppies, categories, favourites, five
languages, the Test Kit ADF. I built downward into one board — web server, web
UI, WebDAV, FTP, thumbnail cache, on-screen keyboard, theme assets. There is
very little to reconcile and a lot to combine.

**Your JC3248 and JC4827 are the same file with a different display driver.**
97% of lines identical, every function name shared. I mention it only because it
decides *how* my code should arrive: if the web server lands the way the rest of
the sketch is organised, it exists three times, and every fix to the WebDAV
parser has to be applied three times. That is the one structural thing I'd ask
to do differently, and it's in your interest more than mine.

---

## Why your tree should be the base

Not a code-quality judgement — I'd just rather weigh what is expensive to
recreate.

My side is five files with measurable coupling. Yours is seven hardware
bring-ups, a Pages flasher with beta channels, a pairing protocol with
owner-lock, documentation, and actual users. The first ports in weeks. The
second doesn't get rebuilt.

---

## Step 1 — `webdav_client.h` as a shared module

This is the whole first step, and I'd like to stop and check it works before
touching anything else.

The file is ~43 KB and self-contained. Its **entire** dependency on the rest of
my firmware is seven configuration strings and one logging function:

```
cfg_dav_enabled   cfg_dav_host   cfg_dav_https   cfg_dav_pass
cfg_dav_path      cfg_dav_port   cfg_dav_user    sdLog()
```

No device state. No UI. No globals of yours. So the port is:

1. Move it somewhere every sketch can include from — `firmware/shared/` or
   similar, your call on the layout.
2. Replace those seven reads with a small config struct passed in at connect
   time, and `sdLog` with a `void(*)(const char*)` callback so it logs through
   whatever each board already uses.
3. Include it from `Gotek_JC3248.ino` and `Gotek_JC4827W543.ino`, and wire
   "load this remote file into the RAM disk" to your existing loader.

`ftp_client.h` is the same shape (six config fields + `sdLog`, ~14 KB) if you
want it, but WebDAV alone is the better first move — fewer moving parts and it's
the one you asked about.

**Done looks like:** a game loads from a WebDAV server on both boards, from one
copy of the client, with your UI unchanged.

### What's in it worth knowing

It's had a lot of hardening, most of it paid for in crashes:

- **Streaming PROPFIND parser** over an 8 KB sliding window. It does not hold
  the document. Tested against a 3000-entry / 688 KB response, and with socket
  reads chopped down to 1 byte to prove the window logic.
- **Listings live in PSRAM** via a throwing allocator, and entries are POD with
  a fixed name buffer. This was the fix for a reproducible `ESP_RST_PANIC`: 3000
  small Arduino `String`s get forced into internal SRAM by
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, and `String`'s move constructor
  isn't `noexcept`, so `std::vector` growth copied instead of moved. The panic
  reported as a plain abort with no mention of memory.
- **One body-framing implementation** shared by download and buffer-read, with
  chunked encoding handled properly. A `transfer-encoding` match that was too
  loose used to corrupt `.adf` files silently.
- **Explicit connect timeouts** on all three connect paths.

There's a host-side test suite for the parser — 304 assertions, gcc, no hardware
— which I'd bring along: `tools/test/run.sh`.

---

## What we have already pulled out

Rather than propose modularisation and leave it as an exercise, three pieces are
already standalone headers in our tree, each with host tests:

| Header | What it is | Why it left the sketch |
|---|---|---|
| `board_profile.h` | Board attributes + the FAT12 geometry they imply | The selector used to be the *display*, which falls apart on a screenless board. Now `-DACTIVE_BOARD=…` picks display/touch/SD presence and the volume size together. |
| `multipart_scan.h` | Streaming multipart body reader | It was binary-unsafe (`String::indexOf` → `strstr` stops at the first NUL, and an ADF is full of them) and blind to a delimiter split across TCP reads. |
| `webdav_client.h` | The WebDAV client | Already standalone: seven config fields and a log callback. |

`tools/test/check_board_profiles.py` validates every board's geometry at once —
cluster count under the FAT12 limit, FAT large enough, volume able to hold a
standard DD image, and no board claiming HD support its geometry cannot deliver.

This matters for the merge in a practical way: the same board-profile mechanism
is what a shared module needs in your tree, where seven targets differ in more
than the panel.

## Step 2 — the web server and the API

Bigger, and worth doing properly rather than quickly. The API layer touches 39
symbols of my firmware. That sounds worse than it is:

| | count |
|---|---|
| `cfg_*` configuration | 28 |
| Real device state | **11** |

The 11: `current_screen`, `dav_enabled`, `dav_entries`, `file_list`, `g_mode`,
`game_list`, `loaded_disk_index`, `nowPlaying`, `web_pending_dav_name`,
`web_pending_dav_path`, `web_pending_sd_load`.

Three of those are already an interface rather than state — the `web_pending_*`
trio is a one-slot command queue. An HTTP handler never blocks; it validates,
drops the request in the queue, and answers immediately, and the main loop picks
it up. That rule exists because doing network I/O inside a request handler locked
the panel for minutes at a time. **Whatever we agree on, please keep that
property.** It's the difference between a responsive device and a brick during a
3000-game index.

So the real work is a small interface over the remaining eight — something like:

```c
// Sketch of a direction, not a decree. You maintain this across seven
// targets, so the shape should be yours.
typedef struct {
  int  (*list)(const char *path, ListSink *out);   // SD, WebDAV, FTP
  int  (*fetch)(const char *path, uint8_t *dst, size_t cap);
  int  (*cover)(const char *path, uint8_t *dst, size_t cap);
  bool (*queue_insert)(const char *path);          // never blocks
} content_source;
```

That's the piece I'd rather agree with you on **before** writing it, because you
have to live with it on boards I don't own — including the ones with no screen
at all, where half of this is meaningless.

---

## Step 3 — the web UI

Last, and least urgent. It's ~130 KB of HTML that compiles into `webui.h` by a
generator with a CI freshness check, so once the server exists it comes along
almost for free.

One warning from experience: `webui.h` is generated and committed. It silently
drifted from source for several releases, and web fixes were tagged, flashed and
reported broken because the device was serving a stale page. If you take it,
take `tools/make_webui_header.py --check` in CI with it.

---

## What I'm not asking for

- **Don't reformat your code.** Your style is dense; mine is spaced and
  commented. Reformatting either way would make the diff unreviewable and it's
  not worth the friction. New shared modules can be in my style, your sketches
  stay in yours.
- **Themes: yours should win.** Your six palettes are twenty lines and work on
  every target. Mine needs PNG assets per resolution, which is dead weight on a
  XIAO dongle with no display. Mine can be an optional layer on the big screens
  or it can go; I'm not attached.
- **Version numbering: yours.** You're at 5.7.7 and shipping to users; my 0.20.0
  should disappear rather than confuse anyone.

---

## What I'd like from your side, regardless of any merge

Two things of yours are straight wins for me and I'd credit them clearly:

- **HD floppy support** — `ADF_HD_SIZE=1802240`, 22 sectors/track, `>1.2 MB ⇒
  HD`. *(Since the first draft I've built this too, via compile-time board
  profiles — but your geometry numbers were the reference, and credit stands.)*
- **OTA self-flash from SD** — drop a tagged `.bin` on the card and flash the
  inactive slot. That makes the whole tag → CI → download → USB-cable ritual
  optional for ordinary users, which is worth a lot.

---

## Decisions only you can make

1. Where do shared modules live, and how do the sketches include them?
2. Is the `content_source` direction right, or would you rather the web layer
   talk to a narrower/wider surface?
3. Should WebDAV be available in **wireless dongle mode** too, or local-USB
   only to start? (Fetch over WiFi and forward over ESP-NOW is a real question
   about bandwidth and the 5 V rail, not a formality.)
4. Do you want FTP at all, or is WebDAV enough?
5. Branch and review preference — a branch on your repo, or a PR from mine?

---

## A note on power, since it bit me repeatedly

An Amiga PSU that's thirty years old doesn't hold 5 V under load. The stack of
`tud_disconnect` + a 1.44 MB PSRAM clear + a full LCD repaint + a TLS handshake
+ sustained WiFi RX is the most likely thing in this firmware to brown out that
rail. I dim the backlight for the duration of every network transfer, and cap
WiFi TX power from config. On boards that feed off the Gotek, I'd keep that
behaviour, and I'd be careful about anything that adds a simultaneous radio
burst and a full-screen paint.

---

## Reproducing the measurements

```bash
git remote add upstream https://github.com/mesarim/Gotek-Touchscreen-interface.git
git fetch upstream

# no common ancestor
git merge-base HEAD upstream/main

# duplication between your two big sketches
git show upstream/main:firmware/Gotek_JC3248/Gotek_JC3248.ino      > /tmp/a.ino
git show upstream/main:firmware/Gotek_JC4827W543/Gotek_JC4827W543.ino > /tmp/b.ino
comm -12 <(sort /tmp/a.ino) <(sort /tmp/b.ino) | grep -c .

# what the WebDAV client actually depends on
grep -oE '\b(cfg_[a-z_]+|sdLog)\b' Gotek_Touchscreen/webdav_client.h | sort -u

# the API layer's coupling, split config vs device state
grep -oE '\b(g_[a-z_]+|cfg_[a-z_]+|dav_[a-z_]+|game_list|file_list|nowPlaying|loaded_disk_index|selected_index|current_screen|web_pending_[a-z_]+)\b' \
  Gotek_Touchscreen/api_handlers.h | sort -u
```

---

## Suggested first move

If the direction is agreeable, I'd open one branch on your repo containing
nothing but: the shared-module directory, `webdav_client.h` with its seven
config reads and `sdLog` replaced by injected parameters, and the include plus
loader wiring in `Gotek_JC3248.ino` and `Gotek_JC4827W543.ino`. No web server,
no UI, no changes to your themes, menus or dongle code.

Small enough to review in one sitting, and it either works on both boards or it
doesn't.
