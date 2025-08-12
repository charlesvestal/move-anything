#/bin/bash

# uncomment to debug
# set -x 

echo "Change Page to $1"
 
killall MoveLauncher MoveMessageDisplay Move

echo "Waiting for Move binaries to exit..."
sleep 0.5

if (($1 < 0)); then
    echo "Page is less than zero, ignoring..."
    exit
fi

DIR=UserLibrary_$1

echo $skip_launch

if [ ! -d "/data/UserData/$DIR" ]; then
    echo "Page $1 doesn't exist, creating..."
    mkdir -p /data/UserData/$DIR/Sets

    ln -s "/data/UserData/UserLibrary_base/Samples" "/data/UserData/$DIR/Samples"
    ln -s "/data/UserData/UserLibrary_base/Recordings" "/data/UserData/$DIR/Recordings"
    ln -s "/data/UserData/UserLibrary_base/Track Presets" "/data/UserData/$DIR/Track Presets"
    ln -s "/data/UserData/UserLibrary_base/Audio Effects" "/data/UserData/$DIR/Audio Effects"
 
fi

# killall MoveLauncher MoveMessageDisplay Move

# sleep 5

# ps aux | grep Move

echo $DIR
rm /data/UserData/UserLibrary
ln -s "$DIR" /data/UserData/UserLibrary

# sleep 5

# /opt/move/MoveLauncher &
# nohup /opt/move/MoveLauncher 2>/dev/null 1>/dev/null &

# refresh cache
# dbus-send --system --type=method_call --dest=com.ableton.move --print-reply /com/ableton/move/browser com.ableton.move.Browser.refreshCache
if [ $2 = "skipLaunch" ]; then
    echo "Skipping launch..."
else
    /opt/move/MoveLauncher
fi