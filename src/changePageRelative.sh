#!/usr/bin/env bash
echo ">>>>>>>>>>>>>>>>>>>>>>>>Change Page Relative by $0 $1 $2"
delta=$1
current_page=$(echo `ls -la /data/UserData/UserLibrary` | grep -o '[0-9]*$')
echo $current_page
new_page=$((current_page + delta))

echo $new_page
/data/UserData/control_surface_move/changePage.sh $new_page
