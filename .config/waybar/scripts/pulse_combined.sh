#!/usr/bin/env bash
# Custom pulseaudio module with bluetooth detection
# Shows VOL for regular audio, VOL-B for bluetooth

get_volume() {
    wpctl get-volume @DEFAULT_SINK@ 2>/dev/null
}

get_mute() {
    wpctl get-volume @DEFAULT_SINK@ 2>/dev/null | grep -q MUTED && echo "1" || echo "0"
}

get_bluetooth() {
    # Check if the default sink is a bluetooth device
    SINK=$(wpctl get-volume @DEFAULT_SINK@ 2>/dev/null)
    # Check if any bluetooth nodes are active
    pactl list sinks 2>/dev/null | grep -q "bluez" && echo "1" || echo "0"
}

while true; do
    VOL=$(get_volume)
    MUTED=$(get_mute)
    BT=$(get_bluetooth)
    
    # Extract volume percentage
    VOL_PCT=$(echo "$VOL" | awk '{print $2}' | sed 's/%//')
    [ -z "$VOL_PCT" ] && VOL_PCT=0
    
    # Build display
    if [ "$MUTED" = "1" ]; then
        TEXT="<span color='#565f77'>MUTED</span>"
        CLASS="muted"
    elif [ "$BT" = "1" ]; then
        TEXT="<span color='#7aa2f7'>VOL-B</span> ${VOL_PCT}%"
        CLASS="bluetooth"
    else
        TEXT="<span color='#7aa2f7'>VOL</span> ${VOL_PCT}%"
        CLASS=""
    fi
    
    # Build tooltip
    TOOLTIP="Volume: ${VOL_PCT}%"
    [ "$MUTED" = "1" ] && TOOLTIP="$TOOLTIP (Muted)"
    [ "$BT" = "1" ] && TOOLTIP="$TOOLTIP | Bluetooth"
    
    printf '{"text":"%s","class":"%s","alt":"%s","tooltip":"%s"}\n' \
        "$TEXT" "$CLASS" "$CLASS" "$TOOLTIP"
    
    sleep 1
done