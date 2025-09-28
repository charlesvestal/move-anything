#!/usr/bin/env bash
# set -x
ssh_quiet="ssh -o LogLevel=QUIET ableton@move.local"
$ssh_quiet ls /opt/move/MoveOriginal
$ssh_quiet md5sum /opt/move/MoveOriginal
$ssh_quiet md5sum /opt/move/Move        
$ssh_quiet md5sum /usr/lib/control_surface_move_shim.so
$ssh_quiet cat /opt/move/Move
$ssh_quiet ps aux | grep Move
$ssh_quiet ls ./control_surface_move
