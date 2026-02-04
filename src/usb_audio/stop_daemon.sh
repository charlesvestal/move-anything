#!/bin/bash
# stop_daemon.sh - Stop the audio stream daemon gracefully

PID_FILE="/var/run/audio_stream_daemon.pid"

if [ -f "$PID_FILE" ]; then
    pid=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        echo "Stopping audio stream daemon (pid $pid)..."
        kill "$pid"
        sleep 1
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid"
        fi
        rm -f "$PID_FILE"
        echo "Stopped"
    else
        echo "Daemon not running (stale PID file)"
        rm -f "$PID_FILE"
    fi
else
    # Try to find by name
    pid=$(pidof audio_stream_daemon 2>/dev/null || true)
    if [ -n "$pid" ]; then
        echo "Stopping audio stream daemon (pid $pid)..."
        kill "$pid"
        sleep 1
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid"
        fi
        echo "Stopped"
    else
        echo "Audio stream daemon not running"
    fi
fi
