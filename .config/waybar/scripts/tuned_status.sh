#!/usr/bin/env bash
set -u

GEAR=$'\uF013'
BOLT=$'\uF0E7'

while true; do
	profile=$(tuned-adm active 2>/dev/null | sed -n 's/.*profile: *//p' | tr -d '\r')
	if [ -n "$profile" ] && [ "$profile" != "none" ]; then
		case "$profile" in
			*powersave*|*battery*) glyph=$BOLT ;;
			*) glyph=$GEAR ;;
		esac
		printf '{"text":"%s %s","class":"active","alt":"%s","tooltip":"tuned profile: %s\\nclick to cycle profile"}\n' \
			"$glyph" "$profile" "$profile" "$profile"
	else
		printf '{"text":"%s off","class":"inactive","alt":"off","tooltip":"tuned is off\\nclick to cycle profile"}\n' \
			"$GEAR"
	fi
	sleep 5
done