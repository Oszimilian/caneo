#!/usr/bin/env bash
#
# Installiert caneo als systemd-Service (headless --log).
# Aufruf:  sudo ./install-service.sh
#
set -euo pipefail

REPO_DIR="/home/maximilian/Git/caneo"
BIN_SRC="${REPO_DIR}/build/caneo"
BIN_DST="/usr/local/bin/caneo"
SERVICE_SRC="${REPO_DIR}/caneo.service"
SERVICE_DST="/etc/systemd/system/caneo.service"

if [[ $EUID -ne 0 ]]; then
    echo "Bitte mit sudo ausfuehren:  sudo $0" >&2
    exit 1
fi

if [[ ! -x "$BIN_SRC" ]]; then
    echo "Binary nicht gefunden: $BIN_SRC" >&2
    echo "Erst bauen:  cmake --build build" >&2
    exit 1
fi

echo ">> Binary nach $BIN_DST kopieren"
install -m 0755 "$BIN_SRC" "$BIN_DST"

# SELinux-Context auf bin_t setzen, damit systemd das Binary ausfuehren darf
if command -v restorecon >/dev/null 2>&1; then
    echo ">> SELinux-Context setzen (restorecon)"
    restorecon -v "$BIN_DST" || true
fi

echo ">> CAN-Setup-Skript nach /usr/local/bin kopieren"
install -m 0755 "${REPO_DIR}/caneo-can-up.sh" /usr/local/bin/caneo-can-up.sh

echo ">> Service-Datei nach $SERVICE_DST kopieren"
install -m 0644 "$SERVICE_SRC" "$SERVICE_DST"

echo ">> systemd neu einlesen"
systemctl daemon-reload

# evtl. getriggerten Rate-Limiter / Fail-State zuruecksetzen
systemctl reset-failed caneo.service 2>/dev/null || true

echo ">> Service aktivieren + starten"
systemctl enable --now caneo.service

sleep 2
echo
echo "========================================================"
systemctl status caneo.service --no-pager -l || true
echo "========================================================"

# Auf SELinux-Denials pruefen (DBCs im Home lesen / Logs schreiben)
if command -v ausearch >/dev/null 2>&1; then
    echo
    echo ">> Pruefe auf SELinux-Denials (letzte Minute):"
    if ausearch -m avc -ts recent 2>/dev/null | grep -i caneo; then
        echo
        echo "!! SELinux-Denials gefunden. Saubere Policy erzeugen mit:"
        echo "   ausearch -m avc -ts recent | grep caneo | audit2allow -M caneo_local"
        echo "   semodule -i caneo_local.pp"
    else
        echo "   Keine Denials gefunden. Alles gut."
    fi
fi

echo
echo "Fertig. Steuern mit:"
echo "  systemctl status caneo      # Status"
echo "  journalctl -u caneo -f      # Live-Logs"
echo "  systemctl restart caneo     # nach jedem Rebuild + cp build/caneo /usr/local/bin/"
