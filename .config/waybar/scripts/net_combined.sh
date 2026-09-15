#!/usr/bin/env bash
# Network module with speed info in tooltip
# Uses nmcli for connection info and /sys/class/net for speed stats

LAST_RX=0
LAST_TX=0
LAST_TIME=0

get_active_iface() {
    nmcli -t -f DEVICE,TYPE,STATE device 2>/dev/null | \
        awk -F: '$3 ~ /^connected/ {print $1; exit}'
}

format_speed() {
    local bytes=$1
    if [ "$bytes" -ge 1073741824 ]; then
        awk "BEGIN {printf \"%.1f GB/s\", $bytes/1073741824}"
    elif [ "$bytes" -ge 1048576 ]; then
        awk "BEGIN {printf \"%.1f MB/s\", $bytes/1048576}"
    elif [ "$bytes" -ge 1024 ]; then
        awk "BEGIN {printf \"%.1f KB/s\", $bytes/1024}"
    else
        echo "${bytes} B/s"
    fi
}

IFACE=$(get_active_iface)

if [ -z "$IFACE" ]; then
    echo '{"text":"<span color=\"#f7768e\">OFFLINE</span>","class":"disconnected","alt":"disconnected","tooltip":"No network connection"}'
    exit 0
fi

# Get connection info
TYPE=$(nmcli -t -f DEVICE,TYPE,STATE device 2>/dev/null | grep "^$IFACE:" | cut -d: -f2)
SSID=$(nmcli -t -f ACTIVE,SSID,SIGNAL device wifi list 2>/dev/null | awk -F: '$1=="yes" {print $2; exit}')
SIGNAL=$(nmcli -t -f ACTIVE,SSID,SIGNAL device wifi list 2>/dev/null | awk -F: '$1=="yes" {print $3; exit}')
IP=$(ip -4 addr show "$IFACE" 2>/dev/null | awk '/inet/ {print $2}' | cut -d/ -f1)

RX_FILE="/sys/class/net/$IFACE/statistics/rx_bytes"
TX_FILE="/sys/class/net/$IFACE/statistics/tx_bytes"

while true; do
    NOW=$(date +%s%N)
    RX=$(cat "$RX_FILE" 2>/dev/null || echo 0)
    TX=$(cat "$TX_FILE" 2>/dev/null || echo 0)
    
    if [ "$LAST_RX" -ne 0 ] && [ "$LAST_TX" -ne 0 ]; then
        TIME_DIFF=$((NOW - LAST_TIME))
        if [ "$TIME_DIFF" -gt 0 ]; then
            RX_DIFF=$((RX - LAST_RX))
            TX_DIFF=$((TX - LAST_TX))
            RX_SPEED=$((RX_DIFF * 1000000000 / TIME_DIFF))
            TX_SPEED=$((TX_DIFF * 1000000000 / TIME_DIFF))
        else
            RX_SPEED=0
            TX_SPEED=0
        fi
    else
        RX_SPEED=0
        TX_SPEED=0
    fi
    
    LAST_RX=$RX
    LAST_TX=$TX
    LAST_TIME=$NOW
    
    RX_FMT=$(format_speed $RX_SPEED)
    TX_FMT=$(format_speed $TX_SPEED)
    
    # Build tooltip
    TOOLTIP="Interface: $IFACE"
    [ -n "$IP" ] && TOOLTIP="$TOOLTIP | IP: $IP"
    [ -n "$SSID" ] && TOOLTIP="$TOOLTIP | SSID: $SSID"
    [ -n "$SIGNAL" ] && TOOLTIP="$TOOLTIP | Signal: $SIGNAL%"
    TOOLTIP="$TOOLTIP | Down: $RX_FMT | Up: $TX_FMT"
    
    # Build display text
    if [ "$TYPE" = "wifi" ]; then
        DISPLAY="<span color='#7aa2f7'>WIFI</span>"
        CLASS="wifi"
    elif [ "$TYPE" = "ethernet" ]; then
        DISPLAY="<span color='#9ece6a'>ETH</span>"
        CLASS="ethernet"
    else
        DISPLAY="<span color='#7aa2f7'>NET</span>"
        CLASS="connected"
    fi
    
    printf '{"text":"%s","class":"%s","alt":"%s","tooltip":"%s"}\n' \
        "$DISPLAY" "$CLASS" "$CLASS" "$TOOLTIP"
    
    sleep 2
done