#!/usr/bin/env bash
# Double-click launcher for Finder. Runs flasher.py from wherever this file
# actually lives, so it works regardless of the project folder's path.
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
exec python3 flasher.py
