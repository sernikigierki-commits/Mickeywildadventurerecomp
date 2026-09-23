#!/usr/bin/env sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CUE=$(find "$HERE/disc" -maxdepth 1 -type f -iname '*.cue' -print -quit)
if [ -z "$CUE" ]; then
  echo 'No CUE file found in the disc folder.' >&2
  exit 1
fi
cd "$HERE"
exec "$HERE/MickeyWildAdventureRecompiled" --game "$HERE/game.toml" --disc "$CUE"
