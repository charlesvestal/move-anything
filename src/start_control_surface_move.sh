#!/bin/bash
killall MoveLauncher
killall Move
echo "Waiting 2 seconds for Move binaries to exit..."
sleep 2
./control_surface_move move_m8_lpp.js
