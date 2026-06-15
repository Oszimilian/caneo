#!/usr/bin/env bash
#
# Baut + installiert eine SELinux-Policy, damit der caneo-Service laufen darf
# (Binary ausfuehren, DBCs im Home lesen, Logs schreiben).
#
# Vorgehen: SELinux kurz auf permissive -> Service einmal laufen lassen
# (alle benoetigten Zugriffe werden geloggt) -> Policy aus den Logs bauen
# -> installieren -> wieder enforcing -> Service neu starten.
#
# Aufruf:  sudo ./fix-selinux.sh
#
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Bitte mit sudo ausfuehren:  sudo $0" >&2
    exit 1
fi

POLICY_NAME="caneo_local"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
cd "$WORKDIR"

ORIG_MODE="$(getenforce)"
echo ">> Aktueller SELinux-Modus: $ORIG_MODE"

# Rate-Limiter / Fail-State der vorherigen Loops zuruecksetzen
systemctl reset-failed caneo.service 2>/dev/null || true

# Marker-Zeitpunkt fuer ausearch, damit wir nur DIESEN Lauf einsammeln
MARK="$(date '+%m/%d/%Y %H:%M:%S')"

echo ">> SELinux temporaer auf permissive"
setenforce 0

echo ">> Service starten (sammelt benoetigte Zugriffe)"
systemctl restart caneo.service || true
# kurz laufen lassen, damit DBC-Read + Log-Write tatsaechlich passieren
sleep 4

echo ">> Service-Status (permissive):"
systemctl status caneo.service --no-pager -l || true

echo ">> SELinux zurueck auf enforcing"
setenforce 1

echo
echo ">> Denials seit Start einsammeln und Policy bauen ..."
AVC="$(ausearch -m avc -ts $MARK 2>/dev/null | grep -i 'caneo\|comm="caneo"' || true)"

if [[ -z "$AVC" ]]; then
    echo "   Keine caneo-Denials gefunden."
    echo "   -> Entweder war es doch nicht SELinux, oder der Service ist gar nicht gelaufen."
    echo "   Bitte oben den permissive-Status pruefen."
    exit 1
fi

echo "----- gefundene Denials -----"
echo "$AVC"
echo "-----------------------------"

echo "$AVC" | audit2allow -M "$POLICY_NAME"

echo
echo ">> Erzeugte Policy ($POLICY_NAME.te):"
cat "${POLICY_NAME}.te"
echo

echo ">> Policy installieren"
semodule -i "${POLICY_NAME}.pp"

echo ">> Service in enforcing neu starten"
systemctl reset-failed caneo.service 2>/dev/null || true
systemctl restart caneo.service
sleep 2

echo
echo "========================================================"
systemctl status caneo.service --no-pager -l || true
echo "========================================================"

# Pruefen ob jetzt im enforcing noch was denied wird
LEFT="$(ausearch -m avc -ts recent 2>/dev/null | grep -i caneo || true)"
if [[ -n "$LEFT" ]]; then
    echo
    echo "!! Es gibt noch weitere Denials -- Skript einfach nochmal laufen lassen:"
    echo "$LEFT"
else
    echo
    echo ">> Keine weiteren Denials. Service sollte jetzt laufen."
fi
