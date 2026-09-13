#!/usr/bin/env bash
set -eu

CYCLES=("balanced-battery" "throughput-performance" "powersave")

current=""
if out=$(tuned-adm active 2>/dev/null); then
	current=$(echo "$out" | sed -n 's/.*profile: *//p' | tr -d '[:space:]')
fi

next="${CYCLES[0]}"
if [ -n "$current" ]; then
	for i in "${!CYCLES[@]}"; do
		if [ "${CYCLES[$i]}" = "$current" ]; then
			next="${CYCLES[$(( (i + 1) % ${#CYCLES[@]} ))]}"
			break
		fi
	done
fi

tuned-adm profile "$next" >/dev/null 2>&1 || true
pkill -RTMIN+8 waybar 2>/dev/null || true