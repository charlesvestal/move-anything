#!/bin/bash
mkdir -p ./build/
echo "Building..."
gcc -g  src/control_surface_move.c -o build/control_surface_move -Ilibs/quickjs/quickjs-2025-04-26 -Llibs/quickjs/quickjs-2025-04-26 -lquickjs -lm
cp ./src/*.js ./src/*.mjs ./src/*.sh ./build/
./package.sh
./copy_to_move.sh