#!/bin/bash
set -x
echo "Killing control_surface_move process on Move..."
ssh ableton@move.local killall control_surface_move
echo "Copying build to Move..."
scp  -r ./dist/* ableton@move.local:./
ssh root@move.local 'command cp -aL /data/UserData/control_surface_move/control_surface_move_shim.so /usr/lib/'
ssh root@move.local 'command chmod u+s /usr/lib/control_surface_move_shim.so'
if ssh root@move.local "test ! -f /opt/move/MoveOriginal"; then
  ssh root@move.local 'command mv /opt/move/Move /opt/move/MoveOriginal'
fi

ssh root@move.local 'command chmod +x /data/UserData/control_surface_move/Move.sh'
ssh root@move.local 'command cp /data/UserData/control_surface_move/Move.sh /opt/move/Move'

ssh ableton@move.local 'nohup /opt/move/MoveLauncher >/dev/null 2>&1 &' &

# cp /data/UserData/control_surface_move/Move.sh /opt/move/Move
# /etc/suid-debug
# ssh ableton@move.local "nohup ~/control_surface_move/control_surface_move ~/control_surface_move/move_default.js 1>/dev/null 2>/dev/null &"
