# ESP32-P4 10.1" Photo Frame — flashable firmware

Flash this with **ESPWebTool**: <https://esptool.spacehuhn.com/>

`MANIFEST.txt` in this folder records the exact build these files came from,
plus SHA-256 sums.

## Before you start

ESPWebTool uses the **Web Serial API**, so it needs a desktop build of
**Chrome, Edge or Opera**. Safari and Firefox cannot flash, and neither can
any mobile browser.

Plug the board into a USB port and have nothing else holding the serial port
open — in particular, close `idf.py monitor`, the Arduino IDE's serial
monitor, `screen`, or the GUI flasher in `gui-flasher/`. Only one program can
have the port at a time.

## The easy way — one file at `0x0`

1. Open <https://esptool.spacehuhn.com/> and click **Connect**.
2. Pick the board's serial port in the browser's dialog. On macOS it is
   usually something like `cu.usbmodem…`; on Windows a `COM…` port.
   The chip is detected automatically and should report as **ESP32-P4**.
3. Add the file **`photoframe-merged-0x0.bin`** and set its address to
   **`0x0`** (zero — not `0x1000`, not `0x10000`).
4. Click **Program** and wait. It writes a few megabytes, so give it a
   minute or two.
5. The board reboots into the photo frame on its own.

That single file already contains the bootloader, the partition table and the
app at their correct offsets, which is why one file at one address is enough.

The flash settings baked in at build time — 16 MB, 80 MHz, DIO — are what the
board boots with. ESPWebTool asks esptool-js to `keep` the mode, frequency and
size rather than substitute its own, and on top of that a tool only rewrites
those bytes in an image header sitting at address `0x0`, where this file has
8 KB of `0xFF` padding (the ESP32-P4's bootloader starts at `0x2000`). So there
is nothing for the website to override.

> **This erases saved settings.** The merged image spans the `nvs` partition
> that sits between the partition table and the app, so flashing it clears
> stored WiFi credentials and any other saved preferences. That is unavoidable
> when merging one contiguous image — use the three separate files below if you
> need to keep them.

## The other way — three files at their own offsets

Everything in `separate-bins/` goes to a specific address. This route leaves
`nvs` alone, so the board keeps its WiFi credentials and settings.

| Address   | File                   |
|-----------|------------------------|
| `0x2000`  | `bootloader.bin`       |
| `0xa000`  | `partition-table.bin`  |
| `0x20000` | `photoframe.bin`       |

Two offsets here are **not** the ESP-IDF defaults, and the website's own help
text quotes the defaults, so do not go by it:

- The bootloader is at `0x2000`, not `0x0` — that is the ESP32-P4's offset.
- The partition table is at `0xa000`, not `0x8000`. This project moves it,
  because the P4 bootloader built with PSRAM support is large enough to run
  past `0x8000` and would be overwritten by a table at the default offset.

If you only changed application code, writing `photoframe.bin` at `0x20000`
by itself is enough; the bootloader and partition table rarely change.

## If it does not work

**"No compatible devices found" / the port is missing.** The board needs to be
in download mode. Hold **BOOT**, tap **RESET**, then release **BOOT**, and
click **Connect** again.

**It flashes, then the board reboots in a loop.** Check the serial output —
ESPWebTool shows it after programming, and the **Output** checkbox keeps it
visible. Two causes are worth knowing about:

- *Silicon revision.* This firmware is built for **ESP32-P4 revision v1.x**
  and is rejected by the bootloader on a v3.x chip. The two revisions have
  substantial hardware differences and Espressif treats them as mutually
  exclusive; a v3.x board needs a rebuild without
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`.
- *Flash size.* It is built for **16 MB**. The vendor page for this board
  advertises 32 MB, but the chip on it reports 16384 KB, and a 32 MB image
  header aborts in `spi_flash` before the app ever starts.

**It stalls partway through.** Try a different USB cable or a port directly on
the machine rather than through a hub. If it still stalls, tick ESPWebTool's
**erase the whole flash before programming** option and flash again.
