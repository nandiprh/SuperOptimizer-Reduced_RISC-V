#!/usr/bin/env bash
# ~/.config/fuzzel/clipboard.sh

if ! cliphist list 2>/dev/null | head -1 | grep -q .; then
    notify-send "Clipboard" "No clipboard history available" -i edit-clipboard
    exit 0
fi

# Store entries with original cliphist IDs
ENTRIES=$(cliphist list | head -50)

# Number them 1-50, stripping cliphist's own ID
NUMBERED=$(echo "$ENTRIES" | nl -ba -nrz -w2 -v1 | awk '{$1=""; print NR". "substr($0,2)}' | head -50)

# Show numbered list in fuzzel
choice=$(echo "$NUMBERED" | fuzzel \
    --dmenu \
    --prompt="  " \
    --anchor=top-right \
    --width=60 \
    --lines=15 \
    --font="JetBrains Mono Nerd Font:size=12" \
    --background=1e1e2eaa \
    --text-color=cdd6f4ff \
    --prompt-color=bac2deff \
    --placeholder-color=7f849cff \
    --input-color=cdd6f4ff \
    --match-color=b4befeff \
    --selection-color=585b70ff \
    --selection-text-color=cdd6f4ff \
    --selection-match-color=b4befeff \
    --counter-color=7f849cff \
    --border-width=2 \
    --border-radius=24)

[ -z "$choice" ] && exit 0

# Extract line number from selection, get original cliphist entry
LINENUM=$(echo "$choice" | grep -oP '^\d+')
ORIGINAL=$(echo "$ENTRIES" | sed -n "${LINENUM}p")

# Decode and copy
echo "$ORIGINAL" | cliphist decode | wl-copy
notify-send "Clipboard" "Copied to clipboard" -i edit-clipboard
