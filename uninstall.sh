#!/bin/bash
# Remove the HP LaserJet P1102w driver and any queues using it.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo: sudo $0" >&2; exit 1; }

PPD_DEST="/Library/Printers/PPDs/Contents/Resources/HP-LaserJet-P1102w-ZJS.ppd"
for q in $(lpstat -p 2>/dev/null | awk '{print $2}'); do
    if grep -q "HP_P1102w_ZJS" "/etc/cups/ppd/$q.ppd" 2>/dev/null; then
        echo "Removing queue $q"; lpadmin -x "$q"
    fi
done
rm -rf /Library/Printers/HP_P1102w_ZJS
rm -f "$PPD_DEST"
echo "Driver removed."
