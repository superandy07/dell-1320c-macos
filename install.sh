#!/bin/sh
# Install the native Dell Color Laser 1320c driver and add a print queue.
#
#   sudo ./install.sh            install, and add queue "Dell_1320c_Native"
#   sudo ./install.sh --no-queue install the driver only
#
# The existing Fuji Xerox-based queue is left untouched, so you can compare
# the two and fall back if needed.
set -eu

QUEUE=Dell_1320c_Native
DEST=/Library/Printers/Dell1320c
PPDDIR="/Library/Printers/PPDs/Contents/Resources"
PPDNAME="Dell Color Laser 1320c (native).gz"
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" != 0 ]; then
    echo "Please run with sudo: sudo $0 $*" >&2
    exit 1
fi
if [ ! -x "$HERE/build/rastertodell1320c" ]; then
    echo "Build first: make" >&2
    exit 1
fi

echo "Installing filter to $DEST"
mkdir -p "$DEST"
install -o root -g wheel -m 755 "$HERE/build/rastertodell1320c" "$DEST/rastertodell1320c"
install -o root -g wheel -m 644 "$HERE/ppd/Dell_Color_Laser_1320c.ppd" "$DEST/Dell_Color_Laser_1320c.ppd"

echo "Installing PPD to $PPDDIR"
mkdir -p "$PPDDIR"
gzip -9 -c "$HERE/ppd/Dell_Color_Laser_1320c.ppd" > "$PPDDIR/$PPDNAME"
chown root:wheel "$PPDDIR/$PPDNAME"
chmod 644 "$PPDDIR/$PPDNAME"

if [ "${1:-}" = "--no-queue" ]; then
    echo "Driver installed (no queue added)."
    exit 0
fi

# Reuse the USB address of the printer as macOS already knows it.
URI=$(lpstat -v 2>/dev/null | sed -n 's/^device for [^:]*: \(usb:\/\/Dell\/Color%20Laser%201320c[^ ]*\).*/\1/p' | head -1)
if [ -z "$URI" ]; then
    URI=$(lpinfo -v 2>/dev/null | sed -n 's/^direct \(usb:\/\/Dell\/Color%20Laser%201320c[^ ]*\).*/\1/p' | head -1)
fi
if [ -z "$URI" ]; then
    echo "Could not find the printer's USB address. Connect and switch on the" >&2
    echo "printer, then run this again (or add it in System Settings and pick" >&2
    echo "\"Dell Color Laser 1320c (native)\")." >&2
    exit 1
fi

echo "Adding queue $QUEUE -> $URI"
lpadmin -p "$QUEUE" -E -v "$URI" -P "$DEST/Dell_Color_Laser_1320c.ppd" \
    -D "Dell Color Laser 1320c (native)" -L "USB" \
    -o printer-is-shared=false
cupsenable "$QUEUE"
cupsaccept "$QUEUE"
echo "Done. Print a test page with:"
echo "  lp -d $QUEUE \"$HERE/test/out/patches.pdf\""
