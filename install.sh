#!/bin/bash
# Install the HP LaserJet P1102w driver and create a USB print queue.
# Usage: make && sudo ./install.sh [--queue NAME] [--paper Letter|A4] [--no-queue]
set -euo pipefail

QUEUE="HP_LaserJet_P1102w"
PAPER="Letter"
MAKE_QUEUE=1
while [ $# -gt 0 ]; do
    case "$1" in
        --queue) QUEUE="$2"; shift 2;;
        --paper) PAPER="$2"; shift 2;;
        --no-queue) MAKE_QUEUE=0; shift;;
        *) echo "unknown option: $1" >&2; exit 2;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="/Library/Printers/HP_P1102w_ZJS"
PPD_NAME="HP-LaserJet-P1102w-ZJS.ppd"
PPD_DEST="/Library/Printers/PPDs/Contents/Resources/$PPD_NAME"

[ "$(id -u)" -eq 0 ] || { echo "Run with sudo: sudo $0 $*" >&2; exit 1; }
for f in foo2zjs rastertozjs; do
    [ -x "$HERE/build/$f" ] || { echo "build/$f missing - run 'make' (as yourself, not root) first" >&2; exit 1; }
done

echo "==> Installing driver to $DEST"
install -d -o root -g wheel -m 755 "$DEST/bin" "$DEST/filter"
install -o root -g wheel -m 755 "$HERE/build/foo2zjs"     "$DEST/bin/foo2zjs"
install -o root -g wheel -m 755 "$HERE/build/rastertozjs" "$DEST/filter/rastertozjs"
install -o root -g wheel -m 644 "$HERE/ppd/$PPD_NAME"     "$PPD_DEST"
install -o root -g wheel -m 644 "$HERE/LICENSE"           "$DEST/LICENSE"

echo "==> Self-test with an empty environment (as CUPS runs filters)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 -I "$HERE/tools/make_testpage.py" "$TMP/t.pdf" letter
env -i PPD="$PPD_DEST" /usr/libexec/cups/filter/cgpdftoraster 1 root t 1 "" "$TMP/t.pdf" > "$TMP/t.ras" 2>/dev/null
env -i PPD="$PPD_DEST" TMPDIR="$TMP" "$DEST/filter/rastertozjs" 1 root t 1 "" "$TMP/t.ras" > "$TMP/t.zjs" 2>"$TMP/t.log"
BYTES=$(wc -c < "$TMP/t.zjs" | tr -d ' ')
if [ "$BYTES" -lt 10000 ]; then
    echo "Self-test FAILED ($BYTES bytes). Filter log:" >&2; cat "$TMP/t.log" >&2; exit 1
fi
echo "    OK: $BYTES bytes of ZjStream"

[ "$MAKE_QUEUE" -eq 1 ] || { echo "Driver installed (no queue created)."; exit 0; }

echo "==> Looking for the printer on USB"
URI="$(lpinfo -v 2>/dev/null | awk '/^direct usb:.*(P%201102|P1102)/ {print $2; exit}')"
if [ -z "$URI" ]; then
    echo "No P1102 found on USB. Driver is installed; plug the printer in and re-run," >&2
    echo "or add it in System Settings > Printers & Scanners and choose this driver." >&2
    exit 1
fi
echo "    $URI"

if lpstat -p "$QUEUE" >/dev/null 2>&1; then
    echo "==> Replacing existing queue $QUEUE"
    lpadmin -x "$QUEUE"
fi
echo "==> Creating queue $QUEUE"
lpadmin -p "$QUEUE" -E -v "$URI" -P "$PPD_DEST" \
    -D "HP LaserJet P1102w" -L "USB" \
    -o PageSize="$PAPER" -o printer-is-shared=false
cupsenable "$QUEUE"
cupsaccept "$QUEUE"

echo
echo "Done. Print a test page with:"
echo "    lp -d $QUEUE \"$HERE/build/testpage.pdf\""
