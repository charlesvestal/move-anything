#!/usr/bin/env bash
set -x
ssh ableton@move.local killall MoveLauncher Move MoveOriginal MoveMessageDisplay
./clean.sh
./build.sh
./package.sh
./copy_to_move.sh
while inotifywait -e close_write src/*; do ./build.sh && ./package.sh && ./copy_to_move.sh; done
