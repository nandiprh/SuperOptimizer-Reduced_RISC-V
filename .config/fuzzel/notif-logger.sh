#!/bin/sh
# ~/.local/bin/notif-logger.sh
LOGFILE="$HOME/.local/share/mako/notifications.log"
mkdir -p "$(dirname "$LOGFILE")"

dbus-monitor "interface='org.freedesktop.Notifications',member='Notify'" 2>/dev/null | \
awk '
/string "app_name"/ { getline; app=$0 }
/string "summary"/ { getline; summary=$0 }
/string "body"/ { getline; body=$0; 
    gsub(/^[[:space:]]*string[[:space:]]*"|"$/, "", app)
    gsub(/^[[:space:]]*string[[:space:]]*"|"$/, "", summary)
    gsub(/^[[:space:]]*string[[:space:]]*"|"$/, "", body)
    print strftime("%Y-%m-%d %H:%M:%S") " | " app " | " summary " | " body
    system("sync")
}' >> "$LOGFILE"
