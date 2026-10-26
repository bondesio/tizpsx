#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 "$ROOT/tools/make_icon.py"
make -C "$ROOT/app" all
make -C "$ROOT/launcher" all
printf 'Local TPK and launcher WGT build commands completed. No device operation was performed.\n'
