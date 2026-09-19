#!/usr/bin/env bash
# mako notification count for waybar (from log file)
LOGFILE="$HOME/.local/share/mako/notifications.log"

if [ -f "$LOGFILE" ] && [ -s "$LOGFILE" ]; then
    count=$(wc -l < "$LOGFILE")
else
    count=0
fi

if [ "$count" -gt 0 ]; then
    echo "{\"text\":\"$count\",\"tooltip\":\"$count notifications\",\"class\":\"has-notifications\"}"
else
    echo "{\"text\":\"\",\"tooltip\":\"No notifications\",\"class\":\"no-notifications\"}"
fi