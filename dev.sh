#!/bin/bash
while inotifywait -e close_write src/*; do ./build.sh; done