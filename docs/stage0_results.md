# Stage 0 — hardware verification results

**Board:** **Guition JC8012P4A1** — *not* the Waveshare part this was first written for
**Chip:** ESP32-P4 rev **v1.3**, MAC `80:f1:b2:d3:5a:0a`
**Toolchain:** ESP-IDF v5.5.5, run 2026-09-01

| Gate | Establishes | Status |
|---|---|---|
| 0.0 | PSRAM, flash, memory budget | ✅ **pass** (flash size corrected) |
| 0.1 | Panel init, backlight pin, colour, refresh | ✅ **pass — backlight GPIO 23, RGB order, no inversion** |
| 0.2 | Touch controller address | ✅ **GSL3680 at 0x40** |
| 0.3 | microSD mount + throughput | ✅ **pass — needed the LDO_VO4 rail enabled** |
| **0.4** | **PPA throughput** | ✅ **pass, and it inverts a plan assumption** |
| 0.5 | HW JPEG decode/encode | ✅ **pass** |
| 0.6 | esp_hosted / C6 WiFi | ⬜ not in harness yet |
| 0.7 | SW decode of a 12 MP photo | ✅ **pass — and it changes the ingest design** |

---

## 0.0 — environment

| | Spec sheet | **Measured** |
|---|---|---|
| Flash | 32 MB | **16 MB** ❗ |
| PSRAM | 32 MB | **32 MB** ✅ (33,181,980 B free to malloc) |
| Silicon rev | — | **v1.3** ❗ |
| Internal heap | — | 175,319 B |

**Two findings that stopped the first two boots dead:**

1. **The flash is 16 MB, not the 32 MB Waveshare advertises.** Booting an image
   whose header claims 32 MB aborts in `spi_flash` before `app_main` is ever
   reached: `Detected size(16384k) smaller than the size in the binary image
   header(32768k)`. `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` is now set, and the app
   partition was cut from 8 MB to 6 MB to match.

2. **The chip is rev v1.3, and ESP-IDF defaults the P4 to v3.1+.** Espressif's
   own Kconfig says rev ≥3.0 and rev <3.0 "have huge hardware difference" and
   are *mutually exclusive*. The first flash was refused outright:
   `requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)`.
   Fixed with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` +
   `CONFIG_ESP32P4_REV_MIN_100=y`.

   **Consequence worth knowing:** this build now targets v1.0–v1.99 and will
   **not** run on a v3.x P4. Flash is also a Boya part (`CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP`).

**Memory budget: 11.7 MiB of 32 MB** — 2 panel FB (1.95 MiB ea) + 2 photo
buffers (3.91) + LVGL ARGB layer (3.91). Comfortable, as designed.

---

## 0.4 — PPA throughput ✅ **and it inverts the plan's assumption**

800×1280 RGB565 in PSRAM, mean of 5:

| Operation | Time | Throughput | → fps |
|---|---|---|---|
| **blend fullscreen (crossfade)** | **18.72 ms** | **313.0 MiB/s** | **53.4** |
| **blend alpha=255 (copy)** | **18.50 ms** | **316.7 MiB/s** | **54.0** |
| srm 1:1 copy | 34.51 ms | 113.2 MiB/s | 29.0 |
| srm rotate 90° (orientation) | 33.72 ms | 115.9 MiB/s | 29.7 |
| srm scale 1.15× (zoom) | 28.42 ms | 137.4 MiB/s | 35.2 |

**Crossfade: 53.4 fps — comfortably above the ≥30 fps band.** A 600 ms
crossfade gets ~32 steps. The plan's fallbacks are not needed.

### The assumption that was wrong

The plan predicted crossfade would be the *expensive* case, because it reads
two framebuffers and writes one (3 × 1.95 MiB) while geometric transitions
read parts of two and write one (2 × 1.95 MiB) — so slides ought to run ~1.5×
faster than crossfade.

**Measured, it is the other way round.** The PPA's blend engine sustains
313 MiB/s; its SRM engine manages only 113 MiB/s — **2.8× slower per byte**,
even at 1:1 with no scaling and no rotation. Byte counts were never the
constraint; the two engines simply have very different throughput.

**Design consequence:** slides, wipes, pushes and blinds must **not** be built
on `ppa_do_scale_rotate_mirror`. They involve no scaling, so they should be
expressed as **blend operations with alpha = 255** (or plain 2D-DMA copies),
which puts them on the 313 MiB/s path alongside crossfade. Built the obvious
way on SRM they would be the slowest transitions in the set at 29 fps, and the
crossfade — supposedly the expensive one — would be the smoothest.

**Confirmed by measurement:** a blend with alpha = 255 *is* a copy, and it runs
at **54.0 fps against SRM's 29.0 — 1.87× faster**, despite moving 3 framebuffers
to SRM's 2. So **every transition in the catalogue is built on `ppa_do_blend`**;
`ppa_do_scale_rotate_mirror` is reserved for the two jobs that genuinely need
geometry: the per-photo fit/rotate pass, and the zoom transition.

Built the naive way on SRM, slides and wipes would have run at 29 fps — the
slowest things in the set, and half the speed of the transition the plan called
expensive. This is the single most valuable thing Stage 0 produced.

**Rotation is cheap where it matters:** 33.7 ms, paid **once per photo** in the
fit pass, not per frame. The runtime landscape/portrait setting stands.

---

## 0.5 — hardware JPEG codec ✅

| Case | Shape | Encode | Size | Decode |
|---|---|---|---|---|
| Landscape cache entry | 1280×800 | 18.44 ms | 62,715 B | **10.22 ms** |
| Cache box worst case | 1280×1280 | 29.28 ms | 94,180 B | **16.18 ms** |
| Immich preview | 1440×1080 | 28.07 ms | 93,158 B | **15.48 ms** |

Playback decode is **10–16 ms**, against a 20 s dwell, and it is prefetched
during the previous photo — invisible. Ingest encode is 18–29 ms, paid once.
Cache entries land at **60–95 KB**, far below the 250–400 KB the plan assumed,
so a 16 GB card holds a very large library.

**The whole cache design is vindicated:** the fast path is genuinely fast, and
the reason it must exist is unchanged — the HW decoder cannot downscale, so a
4000×3000 photo (~24 MB decoded) can never go through it directly.

**Gate bug found and fixed:** the decoder pads YUV420 output up to 16-pixel
boundaries, so 1440×1080 actually lands as 1440×**1088**. Allocating `w*h*2` is
short by exactly that padding and the decode is rejected. **The real ingest and
playback code must pad both axes to 16.**

---

## 0.1 — panel ⚠️ needs eyes on the screen

**The panel is alive.** `LCD ID: 93 65 04` — the JD9365 answered, init
succeeded, two 1.95 MiB framebuffers allocated, DPHY LDO ch3 at 2500 mV.
Vendor timings and the 2-lane 1500 Mbps config in `board.h` are correct.

CPU-bound full-screen fill: 60 frames in 1747 ms = **34.4 fps** (that is the
fill loop, not scan-out; vendor timings predict 68.7 Hz).

### ✅ Confirmed on hardware, 2026-09-01

- **Backlight: GPIO 23.** Driving it lights the panel.
- **Element order: RGB.** The stacked colour bars read red, green, blue top to
  bottom — so no BGR swap.
- **Inversion: not needed.** Worth recording explicitly: the sibling CYD
  project's panel needed inversion ON despite being documented otherwise, which
  is exactly why this is measured rather than assumed.

Both are now in `board.h` as `PANEL_RGB_ELEMENT_ORDER` and
`PANEL_NEEDS_INVERSION`.

*Method note:* the first colour test flashed each colour for 1.2 s, which is
easy to miss while also watching for a backlight. Three stacked bars answer the
same question from a single glance and stay on screen. A full-white screen —
the obvious "is it alive" test — cannot distinguish RGB from BGR from inversion
at all, since white is every channel at once.

---

## THE BOARD WAS MISIDENTIFIED

Three runs produced a black screen on a panel that was answering perfectly
(`LCD ID 93 65 04`). The cause was not a pin hypothesis being slightly wrong —
it was the wrong board. This is a **Guition JC8012P4A1**, not a Waveshare
ESP32-P4-WIFI6-Touch-LCD-10.1.

Stage 0 flagged all three symptoms before the truth was known, which is the
whole point of running it:

| Symptom | What it actually meant |
|---|---|
| Flash measured 16 MB against a 32 MB spec | W25Q128 on the Guition board |
| No touch at any GT911 address | It is a **GSL3680**, and it was answering at 0x40 all along |
| Black screen on every Waveshare backlight candidate | Backlight is plain **GPIO 23** |
| Unknown device at 0x32 | **RX8025T RTC**, which the Guition board has and the Waveshare does not |

Corrected values now come from the board's own BSP
([profi-max/JC8012P4A1_BSP_ESP32P4](https://github.com/profi-max/JC8012P4A1_BSP_ESP32P4)):

| | Value |
|---|---|
| Backlight | **GPIO 23** (plain GPIO — not the Waveshare I2C controller at 0x45) ✅ confirmed |
| LCD reset | GPIO 27 |
| Touch | **GSL3680 @ 0x40**, reset GPIO 22, INT GPIO 21 |
| I2C | SDA 7 / SCL 8 |
| SD | slot 0, GPIO 43/44 + 39–42, 5.1k pull-ups |
| C6 WiFi | SDIO slot 1, 4-bit @ 40 MHz |

**The 0x40 mistake is worth naming:** the GSL3680 and the ES7210 codec share
that address. It was in every scan from the first run, labelled as the codec.

---

## 0.2 — touch ✅ GSL3680 at 0x40

I2C bus (SDA 7 / SCL 8 @ 400 kHz) is healthy — 4 devices, all now identified
except one:

| Address | Identification |
|---|---|
| 0x18 | ES8311 audio codec |
| 0x32 | RX8025T RTC (battery-backed) |
| 0x36 | still unidentified — reads all zeros; likely a battery fuel gauge |
| **0x40** | **GSL3680 touch** |

**Touch will not report anything yet.** The GSL3680 is not a register-only
part: it needs its reset toggled (GPIO 22) and then an **~88 KB firmware blob
pushed over I2C** before it produces a single coordinate. That is real work to
schedule, not a driver include.


**Open:** whether the touch FPC is seated, and what 0x32 / 0x36 actually are.
0x36 is the classic MAX17048 battery-fuel-gauge address, which would fit this
board's optional battery support.

---

## 0.3 — microSD ✅ pass

**Mounted first try at 4-bit / 40 MHz** — once the card was actually powered.

### The card was never unpowered-by-accident; it was unpowered by omission

Every mount attempt failed with `ESP_ERR_TIMEOUT` on `send_op_cond`, at **every
bus width and every clock down to 400 kHz**. That ruled out pins, bus width and
signal integrity, and it also ruled out the filesystem — reformatting the card
FAT32 changed nothing, because the card was never answering in the first place.

The schematic (sheet `4_CONN`, "TF CARD") gives the answer: the card's VDD
**and all six 5.1k bus pull-ups** are fed from `ESP_LDO_VO4` — the ESP32-P4's
own internal LDO channel 4, not an always-on 3V3 rail. It has to be switched on
in software, exactly like the MIPI DPHY rail on channel 3 that the display gate
already enables:

```c
sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = 4 };
sd_pwr_ctrl_new_on_chip_ldo(&ldo, &sd_pwr);
host.pwr_ctrl_handle = sd_pwr;          // without this, nothing answers
```

Recorded as `BSP_SD_LDO_CHAN` in `board.h`. **An unpowered card is
indistinguishable from a missing or dead one**, which is why the mount ladder
now reports the rail state before it reports a failure.

### Measured throughput — and a caveat that matters

| | Measured |
|---|---|
| Card | **SDSC, 1892 MB** (an old 2 GB standard-capacity card) |
| Bus | 4-bit @ 40 MHz |
| Sequential write | **0.57 MiB/s** |
| Sequential read | **0.66 MiB/s** |

**This is slow even for a 2 GB card**, and it is the card, not the bus — the bus
negotiated its full 40 MHz. Consequences at this speed:

- A 5 MB web upload takes **~9 s**.
- A cache entry (measured at 60–95 KB in gate 0.5) loads in **~100–140 ms** —
  still comfortably inside a 20 s dwell, so prefetch is unaffected.
- Ingest of a full card would be dominated by I/O rather than by decode.

**Recommendation: use a modern SDHC card** (8–32 GB, Class 10 / U1). Expect
10–30× these figures, and 2 GB is a thin library for a photo frame besides.
These numbers should be re-measured on the card that will actually be used
before any I/O-dependent decision is made.

---

## 0.6 — C6 WiFi ⬜ not yet written

`esp_hosted` on SDMMC slot 1. The classic silent failure is a slave-firmware
version mismatch, and reflashing the C6 needs a wire to the 4-pin pads.

---

## 0.7 — software decode with downscale ✅ pass

Measured against a real 11.6 MP phone photo found on the card
(`IMG00001.jpg`, 4032×2880, baseline SOF0, 1.26 MiB).

| Scale | Output | Time | Output buffer |
|---|---|---|---|
| 1/1 | 4032×2880 | **2311 ms** | 22.15 MiB |
| 1/2 | 2016×1440 | **2010 ms** | 5.54 MiB |
| **1/4** | **1008×720** | **1485 ms** | **1.38 MiB** |
| 1/8 | 504×360 | **1350 ms** | 0.35 MiB |

### Downscaling saves MEMORY, not TIME

This is the finding, and it was not what the plan assumed. Going from 1/1 to
1/8 cuts the output buffer by **63×** but the decode time by only **1.7×**.
Entropy decoding — Huffman plus IDCT over every MCU in the file — dominates,
and the scaler only saves work in the output stage.

Consequences:

- **1/4 is the right ratio** and the plan was right to pick it, but for the
  memory reason alone. Choosing 1/8 to "go faster" would buy ~9% and throw away
  resolution the 800×1280 panel can actually show.
- **Ingest is ~1.5 s per photo regardless of scale.** A 500-photo card takes
  **~12 minutes** to fully cache. Playback starts as soon as the first few land,
  so this is a background cost, not a wait — but the web status page should show
  progress, because 12 minutes of silence looks broken.

### Ingest must use block mode

At 1.5 s per photo, ingest cannot simply yield *between* photos — that would
stall the compositor for a second and a half at a time and the transitions would
visibly hitch. But `jpeg_dec_process()` is a single blocking call, so there is
nowhere to yield inside it.

`esp_new_jpeg` has the answer: **`block_enable`** in `jpeg_dec_config_t` emits
8 or 16 lines per call instead of the whole image, which gives ingest a natural
yield point per MCU row. The catch, from the component's own docs: block mode
**cannot be combined with scale, clipper or rotate**. So ingest becomes:

1. decode in block mode at full resolution, yielding per block, into PSRAM
2. PPA-scale the result down to the cache box (33.7 ms, already measured)

rather than the single scaled decode this gate benchmarks.

*Caveat on the buffer column:* it measures the output allocation only. The
decoder's internal working memory is allocated inside `jpeg_dec_open()`, before
the measurement starts, so true peak is somewhat higher than shown.

### Extension is not format — 2 of 7 files were PNGs

| Files on the card | |
|---|---|
| Baseline JPEG (usable) | **5** |
| **PNG named `.jpg` / `.jpeg`** | **2** |
| Progressive JPEG | 0 |

Two `ComfyUI_*.jpeg` files are PNGs — magic bytes `89 50 4E 47`. Renaming a
file does not convert it, and the decoder correctly refused them.

**Ingest must sniff magic bytes, never trust the extension.** Without it a
whole class of library hands garbage to the decoder. The gate now classifies
every file it finds, and the same classifier belongs in `ingest.c`.

Worth noting the sniffing has to be real: progressive JPEGs (SOF2) share the
same `FF D8` magic as baseline but **cannot** be decoded by `esp_new_jpeg`, so
the check has to walk to the SOF marker rather than stop at the first two bytes.
None were on this card, but a phone library will contain them.

---

## 0.4 revisited — the benchmark measured the wrong conditions

Gate 0.4 reported a full-screen blend at **18.72 ms (53.4 fps)**. On the real
compositor the same operation takes **28.3 ms (35.3 fps)** — 51% slower.

The gate is not wrong; it measured something the application never does. **Gate
0.4 runs before `display_init()`**, deliberately: it was ordered early so the
PPA number would be readable even on a board whose panel never lit. But that
means it measured PPA throughput with the **DSI idle**.

In the running frame the panel is continuously scanning out 1.95 MiB at 68.7 Hz
— about **134 MiB/s of PSRAM read bandwidth that never stops**. The PPA and the
display controller share one memory bus, so the PPA's effective throughput
drops from 313 MiB/s to roughly 207 MiB/s. The two figures add up to about what
the bus can sustain, which is the tell.

**The lesson is the ordering decision, not the number.** Running 0.4 first made
it robust against an unlit panel and simultaneously made it unrepresentative. A
peripheral benchmark taken with the rest of the system idle is a ceiling, not a
prediction.

### Measured on the real compositor, panel live

| Transition | fps | render/frame |
|---|---|---|
| slide-up / slide-down / crossfade | **35.3** | 28.3 ms |
| push-left / dissolve-bands | 34.3 | 29.2 ms |
| slide-left / slide-right / blinds | 33.8–34.1 | 29.3–29.6 ms |
| wipe-left | 33.4 | 29.9 ms |
| barn-open | **32.3** | 31.0 ms |

`display_present()` costs **0.01 ms** — `draw_bitmap` on a DPI panel just swaps
the framebuffer pointer and returns, so nothing is blocked on vsync. All of the
budget is PPA work.

**34 fps is the real ceiling for full-screen transitions**, and it is smooth.
The conclusion that every transition belongs on the blend path still holds — it
was a relative comparison, and both sides moved together.

### What the exact-tiling fix was worth

The first implementation blitted the whole outgoing frame and then painted the
incoming one over it, writing ~1.5 screens per frame (blinds: up to 2).

| | Before | After |
|---|---|---|
| slides | 23–25 fps | 33.8–35.3 fps |
| blinds | 24.5 fps | 33.8 fps |
| push-left (already exact) | 34.4 fps | 34.3 fps |

Push-left was the control: it was already writing exactly one screen, it was
already the fastest, and it did not move. Everything else converged on it.
