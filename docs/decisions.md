# Decisions

Newest first. Records *why*, so a later reader does not undo a choice without
knowing what it cost.

---

## The web-flashable release is one merged image at `0x0`

`tools/make_release.sh` builds `release/` for
[ESPWebTool](https://esptool.spacehuhn.com/): a single
`photoframe-merged-0x0.bin` written at `0x0`, plus `separate-bins/` for anyone
using their own offsets.

Merged at `0x0` rather than at the bootloader's own `0x2000`, because the
website's address field is typed by hand and its help text quotes the *generic*
ESP-IDF offsets. Two of ours are not generic — the P4 bootloader is at `0x2000`,
not `0x0`, and the partition table is at `0xa000`, not `0x8000` (it is moved
because the PSRAM-enabled P4 bootloader runs past `0x8000`). One file at `0x0`
removes every chance of a typo putting the app over the partition table.

The build-time flash settings survive: ESPWebTool v0.2.1 drives
`esptool-js@0.7.1` and calls `writeFlash` with
`flashMode`/`flashFreq`/`flashSize` set to `"keep"`, and the first 8 KB of the
merged file is `0xFF` padding, so there is no image header at `0x0` for any tool
to rewrite. Verified in the shipped bundle, not assumed — `esptool-js@0.7.1`
carries `CHIP_NAME="ESP32-P4"` and both the `esp32p4` and `esp32p4-rev1` stubs.

Cost, and it is a real one: a contiguous merged image spans the `nvs` and
`phy_init` partitions between the table and the app, so **flashing it wipes
saved WiFi credentials and settings.** There is no way to merge a contiguous
image with a hole in the middle. `separate-bins/` is the route that preserves
them, and `gui-flasher/` already made this same trade for the same reason.

## `esp_hosted` is pinned to `~3.0.6`, not `"*"`

An open version range broke the build outright — configure died with `Missing
required kconfig option after retry.` 3.1.0 moved its USB transport
dependencies behind `if: ESP_HOSTED_HOST_USB_*_MANIFEST` rules in its own
manifest. With `"*"` the component manager evaluates 3.1.0's rules while the
locked 3.0.6's Kconfig is what is actually loaded, so those symbols resolve to
nothing and the retry loop gives up.

Pinning rather than updating is the deliberate half: every
`CONFIG_ESP_HOSTED_HOST_*` value in `sdkconfig.defaults` — the SDIO pin map, the
queue depths — is written against 3.0.6's post-rename `HOST_` symbol names, and
those were arrived at by measurement. Moving to 3.1.0 means re-checking all of
them, which is its own task.

## ESP-IDF, not Arduino — and why that is not a preference

Arduino-ESP32 has no MIPI-DSI driver for the P4. Beyond that, the hardware JPEG
codec and the PPA blitter are IDF-only, and those are precisely what a 1280×800
photo frame lives on. The CYD project is Arduino and that experience carries
over at the application layer, but the platform does not.

Pinned to **v5.5.5**: the Waveshare and Espressif BSP components track 5.4/5.5,
and 6.x brings API churn this project gains nothing from.

## Photos are cached fitted to a square 1280×1280 box, not to screen geometry

The obvious design caches at final screen size. Caching to a square box instead
buys two things:

1. **Orientation switching is free.** The rotation happens in the single
   per-photo PPA fit pass, so flipping landscape↔portrait needs no cache rebuild.
2. **Immich skips ingest entirely.** A `size=preview` rendition is ~1440 px on
   the long edge — already inside the HW decoder's range, so it joins the
   pipeline at playback.

Cost: cache entries are slightly larger than they strictly need to be.

## The cache is not an optimisation — it is the only way this works at all

The P4's hardware JPEG decoder has no downscale-during-decode. A 12 MP photo
decodes to ~24 MB of RGB565 and cannot be handed to it. Every photo therefore
passes through an ingest transcode once, and playback only ever reads cache
entries. Without this the feature is not slow, it is impossible.

## Two SDMMC slots, no contention

The C6 radio is on slot 1 via `esp_hosted`; the microSD is on slot 0, whose
pins are fixed in silicon. On many P4 boards these contend. Here they do not,
which is why the frame can serve a web upload while playing photos.

## Settings live in NVS, set from a web page

Carried over from the CYD project unchanged, for the same two reasons:
re-flashing to change a setting is a poor repair path for something hanging on
a wall, and a binary built from a header full of secrets carries those secrets.

## The web UI gets a password; the setup AP does not

New, and a departure from CYD. This device accepts photo uploads and stores an
Immich API key, so LAN-trusted is no longer a reasonable default. The setup AP
stays open so a forgotten password is always recoverable by holding a finger on
the panel through power-on.
