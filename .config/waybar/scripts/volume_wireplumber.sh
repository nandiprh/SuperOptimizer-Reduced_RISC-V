#!/usr/bin/env bash
# Custom volume module with bluetooth detection for pipewire/wireplumber
# Hardcoded internal speaker name

INTERNAL_SPEAKER="alsa_output.pci-0000_00_1f.3-platform-skl_hda_dsp_generic.HiFi__Speaker__sink"

get_sink() {
    # Get the active/default sink name
    wpctl get-volume @DEFAULT_SINK@ 2>/dev/null
    # Try to get the actual sink name from wpctl
    wpctl status 2>/dev/null | grep -oP 'Sink: \K\S+' || echo "$INTERNAL_SPEAKER"
}

get_volume() {
    wpctl get-volume @DEFAULT_SINK@ 2>/dev/null
}

get_mute() {
    wpctl get-volume @DEFAULT_SINK@ 2>/dev/null | grep -q MUTED && echo "1" || echo "0"
}

while true; do
    VOL=$(get_volume)
    MUTED=$(get_mute)
    
    # Check if the active sink is the internal speaker
    # If it's not the internal speaker, it's bluetooth
    SINK_NAME=$(wpctl status 2>/dev/null | grep -oP 'Sink: \K\S+' || echo "$INTERNAL_SPEAKER")
    
    if [ "$SINK_NAME" = "$INTERNAL_SPEAKER" ]; then
        IS_BT=0
    else
        IS_BT=1
    fi
    
    # Extract volume percentage
    VOL_PCT=$(echo "$VOL" | awk '{print $2}' | sed 's/%//')
    [ -z "$VOL_PCT" ] && VOL_PCT=0
    
    # Build display
    if [ "$MUTED" = "1" ]; then
        TEXT="<span color='#565f77'>MUTED</span>"
        CLASS="muted"
    elif [ "$IS_BT" = "1" ]; then
        TEXT="<span color='#7aa2f7'>VOLB</span> ${VOL_PCT}%"
        CLASS="bluetooth"
    else
        TEXT="<span color='#7aa2f7'>VOL</span> ${VOL_PCT}%"
        CLASS=""
    fi
    
    printf '{"text":"%s","class":"%s","alt":"%s"}\n' "$TEXT" "$CLASS" "$CLASS"
    
    sleep 1
done
