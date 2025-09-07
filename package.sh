#!/usr/bin/env bash
set -x
rm -fr ./dist/
mkdir -p ./dist/control_surface_move/
cp -rv ./build/* ./dist/control_surface_move/
strip ./dist/control_surface_move/control_surface_move
tar -C ./dist/ -cvf control_surface_move.tar.gz control_surface_move
