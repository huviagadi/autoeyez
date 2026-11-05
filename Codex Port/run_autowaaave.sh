#!/bin/bash
# Wait for X to be ready
sleep 1

export DISPLAY=:0

# Wait for display to be available
for i in {1..30}; do
    if xdpyinfo >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

cd /home/admin/openFrameworks/apps/myApps/AUTO_WAAAVE_4_5
exec ./bin/AUTO_WAAAVE_4_5
