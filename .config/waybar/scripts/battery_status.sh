#!/usr/bin/env bash
# ~/.local/bin/battery_status.sh
CAPACITY=$(cat /sys/class/power_supply/BAT0/capacity 2>/dev/null || echo "?")
STATUS=$(cat /sys/class/power_supply/BAT0/status 2>/dev/null || echo "Unknown")
PROFILE=$(tuned-adm active 2>/dev/null | awk -F': ' '{print $2}' | tr -d ' ')

case "$STATUS" in
    Charging)    TEXT="<span color='#76946a'>BAT</span> ${CAPACITY}%" ;;
    Discharging) TEXT="<span color='#c0a36e'>BAT</span> ${CAPACITY}%" ;;
    Full)        TEXT="<span color='#7e9cd8'>BAT</span> FULL" ;;
    *)           TEXT="<span color='#c0a36e'>BAT</span> ${CAPACITY}%" ;;
esac

printf '{"text":"%s","tooltip":"Capacity: %s%% | Status: %s | Profile: %s"}\n' \
    "$TEXT" "$CAPACITY" "$STATUS" "$PROFILE"
