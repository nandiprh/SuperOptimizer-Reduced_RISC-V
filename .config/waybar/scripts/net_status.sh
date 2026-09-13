#!/usr/bin/env bash
set -u

WIFI_G=$'\uF1EB'
PLUG_G=$'\uF1E6'
ALERT_G=$'\uF071'

while true; do
	state=$(nmcli -t -f DEVICE,TYPE,STATE device 2>/dev/null)
	wifi_dev=$(printf '%s\n' "$state" | awk -F: '$2=="wifi" && $3 ~ /^connected/ {print $1; exit}')
	eth_dev=$(printf '%s\n' "$state" | awk -F: '$2=="ethernet" && $3 ~ /^connected/ {print $1; exit}')

	if [ -n "$wifi_dev" ]; then
		info=$(nmcli -t -f ACTIVE,SSID,SIGNAL device wifi list 2>/dev/null \
			| awk -F: '$1=="yes" {print $2 "\x01" $3; exit}')
		ssid=${info%%$'\x01'*}
		sig=${info##*$'\x01'}
		[ -z "$sig" ] && sig=0
		printf '{"text":"%s %s %s%%","class":"wifi","alt":"wifi","tooltip":"Wi-Fi %s\\nSignal %s%%"}\n' \
			"$WIFI_G" "$ssid" "$sig" "$ssid" "$sig"
	elif [ -n "$eth_dev" ]; then
		printf '{"text":"%s wired","class":"ethernet","alt":"ethernet","tooltip":"Wired connection"}\n' \
			"$PLUG_G"
	else
		printf '{"text":"%s offline","class":"disconnected","alt":"disconnected","tooltip":"No network connection"}\n' \
			"$ALERT_G"
	fi
	sleep 5
done