# ESP32-P4 10.1" Digital Photo Frame

Firmware for a **Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1**: a photo frame that
plays from a microSD card and from Immich, accepts uploads over the network, and
carries a clock / weather / sun / air-quality overlay along the bottom fifth of
the screen.

Sibling project: `../CYD 2.8 Weather Clock`, which supplies the overlay's data
layer (`sun_moon`, `units`, and the Open-Meteo fetch semantics) and the
engineering rules this project runs on.

> **Status: Stage 0 substantially passed** on a **Guition JC8012P4A1**. Panel,
> touch, PPA and JPEG all verified on hardware. See
> [docs/stage0_results.md](docs/stage0_results.md).

---

## Quick start

There are **two IDF projects here**, and they build independently:

| Project | Where | What it is |
|---|---|---|
| `stage0` | `stage0/` | The hardware gate harness. Run this first. |
| `photoframe` | repo root | The application. A skeleton until the gates report. |

Always source the environment first — `idf.py` is not on `PATH` otherwise:

```bash
source tools/env.sh
```

Then, for the gates:

```bash
cd stage0 && idf.py flash monitor
```

Transcribe every number the run prints into
[docs/stage0_results.md](docs/stage0_results.md). Application code is not
written against a gate that has not produced a value.

The application itself builds from the repo root:

```bash
idf.py build
```

### Flashing from a browser

`tools/make_release.sh` turns a finished build into `release/`, which is what
gets handed to anyone who is not set up to build:

```bash
./tools/make_release.sh
```

It needs no sourced environment — it shells out to the `esptool` CLI and reads
the build's own `flash_args` for the offsets, the same way
`gui-flasher/flasher.py` does.

The output is one merged image to write at `0x0` with
[ESPWebTool](https://esptool.spacehuhn.com/) (desktop Chrome, Edge or Opera —
it needs Web Serial), the three separate bins for anyone who would rather use
their own offsets, flashing instructions, and a manifest with SHA-256 sums.

Merging at `0x0` rather than at the bootloader's `0x2000` is deliberate: one
file at one well-known address is the route the website documents, and its
address field is typed by hand, so there is no offset left to get wrong.

The flash settings survive the trip. ESPWebTool (v0.2.1, driving
`esptool-js@0.7.1` from unpkg) calls `writeFlash` with
`flashMode/flashFreq/flashSize: "keep"`, and a tool only rewrites those bytes
in an image header found at address `0x0` — which here is `0xFF` padding. The
16 MB / 80 MHz / DIO values this board needs are the ones it boots with.

The trade-off is that a merged image spans the `nvs` partition between the
partition table and the app, so flashing it clears saved WiFi credentials.
`release/separate-bins/` is the route that keeps them.

---

## Why there is a Stage 0 at all

The same rule the CYD project used: each gate establishes a hardware fact as a
**value the firmware then uses**, rather than a vague "it seems to work". That
project ended up with two constants that contradict the usual advice for its
board — because they were measured rather than assumed.

Two gates matter more than the rest here:

**0.4 — PPA throughput.** The whole transition design assumes ten hardware-blitted
transitions and a smooth full-screen crossfade. Crossfade reads two framebuffers
and writes one — about 5.9 MiB of PSRAM traffic per frame — so whether it holds
30 fps or 18 fps is a measurement, not a guess. The measured value sets each
transition's duration and step count.

**0.1 — the backlight pin.** Waveshare's own BSPs use different backlight pins
per model (26/27 on the 3.4"/4" board, 32/33 on the 7"), and none is published
for the 10.1". Get it wrong and the DSI log looks perfect while the screen stays
black. Gate 0.1 walks the candidates with the panel painted white.

---

## The constraint that shapes everything

The P4's hardware JPEG decoder has **no downscale during decode** and is
characterised only to ~1920×1080. A 12 MP phone photo decodes to ~24 MB of
RGB565 and cannot be handed to it — not slowly, not at all.

So every photo passes through a cache, and playback only ever touches the cache:

```
INGEST (background, once per photo)
  SD original (4000×3000)
    └─ SW decode at 1/4 scale          caps peak RAM
       └─ PPA fit into a 1280×1280 box + EXIF orientation
          └─ HW JPEG encode → /.frame/cache/<hash>.jpg

PLAYBACK (per photo, ~15 ms)
  cache entry → HW decode → ONE PPA scale+rotate → photo buffer
```

Caching to a *square box* rather than to screen geometry is what makes the
landscape/portrait setting free — the rotation lives in the per-photo fit pass,
so switching orientation rebuilds nothing. It also means Immich `preview`
renditions (~1440 px) skip ingest entirely.

---

## Hardware

| | |
|---|---|
| SoC | ESP32-P4, dual RISC-V, 32 MB flash, **32 MB PSRAM** |
| WiFi | ESP32-C6-MINI over SDIO **slot 1** (`esp_hosted`), WiFi 6 / BLE 5 |
| Panel | JD9365 10.1", **800×1280 native portrait**, 2-lane MIPI-DSI @ 1500 Mbps |
| Touch | GT911, 10-point |
| Storage | microSD on SDMMC **slot 0** (pins fixed in silicon) |
| Audio | ES8311 + ES7210 — present, unused |

The card and the radio are on **different SDMMC slots**, so they do not contend.
On many P4 boards they do; here the frame can serve an upload while playing.

Pin map: [main/board/board.h](main/board/board.h). Every constant there is
marked `[VENDOR]` (read out of Waveshare's published components), `[SOC]`
(fixed by silicon), or `[STAGE0]` (a hypothesis awaiting measurement) — so a
dark screen traces to the right line.

---

## Layout

```
main/
  board/board.h        THE pin map, with provenance on every value
  photo/               compositor, transitions, playlist, cache, ingest
  net/                 wifi, httpd + upload, immich, weather, airquality
  ui/                  overlay pages, on-device settings, theme
stage0/                the gate harness (builds and runs standalone)
docs/
  stage0_results.md    the live gate table — fill it in
  decisions.md         why things are the way they are, newest first
tools/
  env.sh               sources ESP-IDF and fixes the macOS cert issue
  make_release.sh      build/ -> release/, web-flashable
  release_README.md    the flashing instructions shipped in release/
gui-flasher/           point-and-click erase/merge/flash over USB
release/               generated; ignored by git
```

## Toolchain

ESP-IDF **v5.5.5** at `~/esp/esp-idf`, target `esp32p4`.

`tools/env.sh` pins `SSL_CERT_FILE` to certifi's bundle — the python.org 3.12
framework on this machine ships without the OpenSSL cert symlink, and without
that both the tool installer and the component manager fail with
`CERTIFICATE_VERIFY_FAILED`. The fix stays scoped to the shell rather than
modifying the system Python.
