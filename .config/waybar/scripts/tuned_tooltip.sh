#!/usr/bin/env bash
# Simple tuned-adm profile name output for Waybar tooltip
profile=$(tuned-adm active 2>/dev/null | sed -n 's/.*profile: *//p' | tr -d '\r')
if [ -n "$profile" ] && [ "$profile" != "none" ]; then
    echo "$profile"
else
    echo "off"
fi