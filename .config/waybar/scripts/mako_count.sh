#!/usr/bin/env bash
# mako notification count for waybar
count=$(makoctl list -j 2>/dev/null | jq 'length' 2>/dev/null || echo 0)
if [ "$count" -gt 0 ]; then
    echo "{\"text\":\"$count\",\"class\":\"has-notifications\"}"
else
    echo "{\"text\":\"\",\"class\":\"no-notifications\"}"
fi