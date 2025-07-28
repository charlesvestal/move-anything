#!/bin/bash
echo "Killing control_surface_move process on Move..."
ssh ableton@move.local killall control_surface_move
echo "Copying build to Move..."
scp  -r ./dist/* ableton@move.local:./