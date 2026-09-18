#!/bin/sh
# ~/.local/bin/notif-center.sh
LOGFILE="$HOME/.local/share/mako/notifications.log"

if [ ! -f "$LOGFILE" ] || [ ! -s "$LOGFILE" ]; then
    notify-send "Notifications" "No notifications logged yet"
    exit 0
fi

tail -n 50 "$LOGFILE" | \
    awk -F' \\| ' '{print NR". ["$1"] "$2": "$3}' | \
    fuzzel --dmenu \
        --prompt="  " \
        --anchor=center \
        --width=80 \
        --lines=15 \
        --font="JetBrains Mono Nerd Font:size=14" \
        --background=1a1b26aa \
        --text-color=c0caf5ff \
        --match-color=7aa2f7ff \
        --selection-color=2a2c3dff \
        --selection-text-color=c0caf5ff \
        --border-width=2 \
        --border-radius=12
