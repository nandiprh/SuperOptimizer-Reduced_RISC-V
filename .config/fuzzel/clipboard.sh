#!/usr/bin/env bash
# Clipboard launcher using cliphist + fuzzel

# Check if clipboard has entries
if ! cliphist list 2>/dev/null | head -1 | grep -q .; then
    notify-send "Clipboard" "No clipboard history available" -i edit-clipboard
    exit 0
fi

# Launch fuzzel in dmenu mode with clipboard entries
choice=$(cliphist list | fuzzel --dmenu --prompt="Clipboard " --width=40% --lines=10)

if [ -z "$choice" ]; then
    exit 0
fi

# Decode and copy the selected entry to clipboard
cliphist decode "$choice" | wl-copy

# Show confirmation
notify-send "Clipboard" "Copied to clipboard" -i edit-clipboard