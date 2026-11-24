# ESP32-P4 10.1" Digital Photo Frame

A digital photo frame for the **Guition JC8012P4A1** — a 10.1" 800×1280
ESP32-P4 touch panel. It plays photos from a microSD card and from
[Immich](https://immich.app/), takes uploads over your network, and carries a
clock / weather / sun / air-quality overlay along the bottom fifth of the
screen.

Everything runs on the device. There is no companion app and no cloud service:
the frame serves its own web UI, and the sun and moon figures are computed
locally, so they stay correct with the network down.

> **Status: working on hardware.** Panel, touch, PPA blitter and the hardware
> JPEG codec are all verified on a real board — see
> [docs/stage0_results.md](docs/stage0_results.md) for the measured numbers.

---

## Flash it

You do not need a toolchain. Grab `photoframe-merged-0x0.bin` from the
[latest release](../../releases/latest) and write it with
**[ESPWebTool](https://esptool.spacehuhn.com/)** at address **`0x0`**.

1. Open <https://esptool.spacehuhn.com/> in **desktop Chrome, Edge or Opera**
   (it uses Web Serial; Safari, Firefox and mobile browsers cannot flash).
2. Click **Connect** and pick the board's serial port. It should identify as
   **ESP32-P4**.
3. Add `photoframe-merged-0x0.bin` and set the address to **`0x0`** — zero, not
   `0x1000` or `0x10000`.
4. Click **Program**. It writes about 2.2 MB, so give it a minute.

If no port appears, hold **BOOT**, tap **RESET**, release **BOOT**, and connect
again.

The release also contains the three separate bins for anyone who would rather
use their own offsets, plus SHA-256 sums. Note that the merged image spans the
`nvs` partition, so **flashing it clears saved WiFi credentials**; the separate
bins keep them. Full notes ship in the release as `README.md`.

> Built for **ESP32-P4 silicon revision v1.x** and **16 MB of flash**. A v3.x
> P4 rejects this image at the bootloader — the two revisions are not
> compatible — and a 32 MB header aborts in `spi_flash` before `app_main`.

## First run

On first boot the frame has no credentials, so it opens its own setup network:

1. Join the open WiFi network **`PhotoFrame-XXXX`** (the suffix is from the
   board's MAC).
2. Browse to **<http://192.168.4.1>**.
3. Pick your network from the scan list and enter the password. The frame joins
   in the background and drops the setup network once it succeeds.

The setup network is deliberately open — there is no shared secret to put on a
sticker, and it only exists until the frame is on your WiFi.

From then on the web UI lives at the frame's address on your network, which the
on-screen overlay shows. You can set a password for it, and from there you can:

| | |
|---|---|
| Upload photos | straight onto the card over the network |
| Browse and delete | what is on the card |
| Connect Immich | server URL, API key, and pick albums |
| Set location | latitude / longitude and timezone, for weather and sun times |
| Tune the overlay | which panels show, and how long each page holds |

## Photos on the card

Put `.jpg` / `.jpeg` files anywhere on the card — the frame scans recursively
from the root, up to 4 directories deep, and up to 512 photos. The extension
gets a file considered; its header decides, so a PNG named `.jpg` is rejected
rather than shown broken. Dotfiles are skipped, which also means the
AppleDouble and Spotlight droppings a Mac leaves on a card are ignored.

Any size goes in. Large photos are transcoded once in the background and
played from a cache afterwards — see below for why that is not optional.

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
          └─ HW JPEG encode → cache entry

PLAYBACK (per photo, ~15 ms)
  cache entry → HW decode → ONE PPA scale+rotate → photo buffer
```

Caching to a *square box* rather than to screen geometry is what makes the
landscape/portrait setting free — the rotation lives in the per-photo fit pass,
so switching orientation rebuilds nothing. It also means Immich `preview`
renditions (~1440 px) skip ingest entirely.

## Hardware

| | |
|---|---|
| SoC | ESP32-P4, dual RISC-V, **16 MB flash** (measured; the spec sheet says 32), **32 MB PSRAM** |
| WiFi | ESP32-C6-MINI over SDIO **slot 1** (`esp_hosted`), WiFi 6 / BLE 5 |
| Panel | JD9365 10.1", **800×1280 native portrait**, 2-lane MIPI-DSI @ 1500 Mbps |
| Touch | **GSL3680** @ 0x40, reset GPIO 22, INT GPIO 21 |
| Backlight | plain **GPIO 23** |
| Storage | microSD on SDMMC **slot 0** (pins fixed in silicon) |
| Audio | ES8311 + ES7210 — present, unused |

The card and the radio are on **different SDMMC slots**, so they do not
contend. On many P4 boards they do; here the frame can serve an upload while
playing.

Pin map: [main/board/board.h](main/board/board.h). Every constant there is
marked `[VENDOR]` (read out of a vendor's published components), `[SOC]`
(fixed by silicon), or `[STAGE0]` (a hypothesis awaiting measurement) — so a
dark screen traces to the right line.

> This was first written for a **Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1**,
> which turned out to be a different board, and every backlight and touch value
> was wrong as a result. The gate harness below is what caught it. That history
> is kept rather than erased, because it is the reason the pin map carries
> provenance at all.

---

## Building it yourself

ESP-IDF **v5.5.5**, target `esp32p4`. There are **two independent IDF projects**
in the tree:

| Project | Where | What it is |
|---|---|---|
| `photoframe` | repo root | The application |
| `stage0` | `stage0/` | A standalone hardware gate harness |

```bash
source tools/env.sh
```

```bash
idf.py build
```

`tools/env.sh` sources ESP-IDF and pins `SSL_CERT_FILE` to certifi's bundle —
a python.org 3.12 framework install ships without the OpenSSL cert symlink, and
without that both the tool installer and the component manager fail with
`CERTIFICATE_VERIFY_FAILED`. The fix stays scoped to the shell rather than
modifying the system Python.

To produce a flashable release of your own build:

```bash
./tools/make_release.sh
```

That writes `release/` — the merged image, the separate bins, instructions and
a manifest. It reads the build's own `flash_args` for the offsets, so it holds
no second copy of the partition layout, and it needs no sourced IDF
environment. `gui-flasher/` is a point-and-click alternative for flashing over
USB.

### Why there is a Stage 0 at all

Each gate establishes a hardware fact as a **value the firmware then uses**,
rather than a vague "it seems to work":

```bash
cd stage0 && idf.py flash monitor
```

Two gates matter more than the rest:

**0.4 — PPA throughput.** The transition design assumes ten hardware-blitted
transitions and a smooth full-screen crossfade. Crossfade reads two
framebuffers and writes one — about 5.9 MiB of PSRAM traffic per frame — so
whether it holds 30 fps or 18 fps is a measurement, not a guess. The measured
value sets each transition's duration and step count.

**0.1 — the backlight pin.** Vendor BSPs use different backlight pins per
model, and none was published for this panel. Get it wrong and the DSI log
looks perfect while the screen stays black. Gate 0.1 walks the candidates with
the panel painted white. It is **GPIO 23**.

## Layout

```
main/
  board/board.h        THE pin map, with provenance on every value
  photo/               compositor, transitions, playlist, cache, ingest
  net/                 wifi, httpd + upload, immich, weather, airquality
  ui/                  overlay pages, on-device settings, theme
stage0/                the gate harness (builds and runs standalone)
docs/
  stage0_results.md    measured hardware results, gate by gate
  decisions.md         why things are the way they are, newest first
tools/
  env.sh               sources ESP-IDF and fixes the macOS cert issue
  make_release.sh      build/ -> release/, web-flashable
gui-flasher/           point-and-click erase/merge/flash over USB
```

[docs/decisions.md](docs/decisions.md) is worth a look before changing
anything — several settings contradict the vendor documentation and are the way
they are because of a measurement.

## Credits

The overlay's data layer (`sun_moon`, `units`, and the Open-Meteo fetch
semantics) comes from
[CYD-2.8-Wifi-Weather-Clock](https://github.com/Garionhk/CYD-2.8-Wifi-Weather-Clock),
along with the engineering rules this project runs on. Weather, air quality and
UV data are from [Open-Meteo](https://open-meteo.com/).

Bundled fonts are **IBM Plex Sans Condensed**, © IBM Corp., licensed under the
[SIL Open Font License 1.1](main/ui/fonts/OFL.txt).
