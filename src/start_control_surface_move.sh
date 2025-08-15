#!/bin/bash
killall MoveLauncher MoveMessageDisplay Move
echo "Waiting 1 second for Move binaries to exit..."
sleep 0.5
echo "Launching!"
/data/UserData/control_surface_move/control_surface_move /data/UserData/control_surface_move/move_m8_vlpp.js
/opt/move/MoveLauncher