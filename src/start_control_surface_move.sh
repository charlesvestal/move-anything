#!/bin/bash
killall MoveLauncher MoveMessageDisplay Move
echo "Waiting 1 second for Move binaries to exit..."
sleep 0.5
echo "Launching Hello World display control surface..."
cd /data/UserData/control_surface_move
./control_surface_move ./hello_world_display.js
cd - >/dev/null 2>&1
/opt/move/MoveLauncher
