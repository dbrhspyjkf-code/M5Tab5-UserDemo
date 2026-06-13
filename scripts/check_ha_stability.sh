#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
view="$root/app/apps/app_ha/view/view.cpp"
app="$root/app/apps/app_ha/app_ha.cpp"
client="$root/app/apps/app_ha/ha_client.cpp"

card_allocs="$(grep -c 'new CardData' "$view" || true)"
action_binds="$(grep -c '^[[:space:]]*_bind_action(' "$view" || true)"
if [[ "$card_allocs" -ne "$action_binds" ]]; then
    echo "CardData allocations must be bound through _bind_action()" >&2
    echo "allocations=$card_allocs action_binds=$action_binds" >&2
    exit 1
fi

if grep -q '}).detach();' "$app" "$client"; then
    echo "HA worker threads must be owned and joined, not detached" >&2
    exit 1
fi

if ! grep -q '_ha->destroy()' "$app"; then
    echo "AppHA::onClose must stop the HA polling thread" >&2
    exit 1
fi
