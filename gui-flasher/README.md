# GUI Flasher

A small point-and-click tool to merge, erase and flash an ESP32-P4 board,
without sourcing an ESP-IDF environment script or touching the IDF Python
environment at all. It shells out directly to the `esptool` CLI already
installed on this Mac and reads any ESP-IDF build directory's own
`flash_args` file for the offsets and files -- the same file `idf.py flash`
generates and uses -- so it never keeps its own copy of any project's
partition layout. Point it at a different project's `build/` directory and
it works unmodified.

## Run it

```bash
python3 flasher.py
```

or double-click `run.command` in Finder.

Requires only Python 3 with `tkinter` (already part of the standard macOS
Python 3 framework install) and the `esptool` CLI, which is auto-detected on
PATH or at `~/.local/bin/esptool`. If neither is found, the "esptool" field
at the bottom of the Connection panel is a plain text box -- type or Browse
to the executable directly.

## One file, on purpose -- and what that costs

Every flash goes through `esptool merge-bin` first: bootloader,
partition-table and app are combined into a single `merged-firmware.bin`
(written into the selected build directory) and that one file is what
actually gets written to the board, at one offset.

That single file's byte range typically spans any data partitions sitting
between the partition table and the app too (commonly `nvs` and `phy_init`)
-- there's no way to merge a contiguous image that skips over a gap in the
middle. **So every flash wipes whatever that project keeps in NVS** (WiFi
credentials, and any other settings it stores there). This is a deliberate
trade-off for having one simple file to distribute, not a bug -- the
confirmation dialog says so each time.

## What it does

1. **Firmware build** -- pick any ESP-IDF build directory containing a
   `flash_args` file (defaults to `../build/` relative to this script, since
   that's the common layout, but Browse… goes anywhere -- a different
   project's `build/` directory, a sub-project's own build output, or a
   firmware build copied elsewhere). The table shows the source files it
   will merge; a build directory missing `flash_args`, or missing one of the
   files it lists, shows the reason in red instead.
2. **Connection** -- pick the serial port (the dropdown lists everything
   under `/dev/cu.*`; type a path directly if the board isn't enumerating
   right now), baud rate (460800 by default, a common fast rate for these
   boards), and chip (`esp32p4` by default -- merging needs a concrete chip
   since there's no device to auto-detect it from; `auto` is still available
   for Erase and Flash, which do have a board attached, or pick a different
   chip entirely from the dropdown).
3. **Merge Firmware** -- runs `esptool merge-bin` and writes
   `merged-firmware.bin` into the build directory. No port needed, no device
   touched -- this is the "just give me a file to hand to someone else"
   action, and it doubles as a dry run for what Flash's own merge step will
   do.
4. **Erase Flash** -- erases the entire chip. Asks for confirmation first;
   this cannot be undone.
5. **Flash Firmware** -- merges, then writes the resulting single file to the
   board. Confirms first, since it wipes saved settings as described above.
6. **Erase && Flash** -- erases, merges, then flashes, stopping at the first
   step that fails.
7. **Flash an existing merged image** -- a separate panel, independent of the
   build directory above: point it at any `.bin` file and an offset (default
   `0x2000`, a common ESP32-P4 bootloader offset -- change it if a
   particular project's build uses a different one) and it writes exactly
   that file at exactly that offset, with no merge step. Useful for
   reflashing the same image to several boards without re-merging each time,
   or for flashing a file someone else sent you. A successful Merge or Flash
   above fills this in automatically, so the common "flash board two" case
   needs no browsing at all.

The exact command about to run is shown above the log, and the log itself is
esptool's own live output, not a summary of it.
