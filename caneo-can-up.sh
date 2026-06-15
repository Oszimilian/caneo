#!/usr/bin/env bash
#
# Bringt die realen CAN-Interfaces idempotent hoch, BEVOR caneo startet.
# Wird vom Service als root aufgerufen (ExecStartPre=+ ...), damit caneo
# selbst kein "sudo ip link" mehr braucht.
#
# Bitrate muss zu den 'baudrate'-Werten in caneo.yaml passen.
set -u

BITRATE=1000000
IFACES=(can0 can1)

for IF in "${IFACES[@]}"; do
    # Interface vorhanden?
    if [[ ! -d "/sys/class/net/${IF}" ]]; then
        echo "caneo-can-up: ${IF} existiert nicht, ueberspringe"
        continue
    fi

    state="$(cat "/sys/class/net/${IF}/operstate" 2>/dev/null || echo unknown)"
    if [[ "$state" == "up" ]]; then
        echo "caneo-can-up: ${IF} ist bereits up"
        continue
    fi

    echo "caneo-can-up: konfiguriere ${IF} (bitrate ${BITRATE}) und bringe es up"
    ip link set "${IF}" down 2>/dev/null || true
    ip link set "${IF}" type can bitrate "${BITRATE}"
    ip link set "${IF}" up
done

exit 0
