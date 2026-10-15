#!/bin/sh
# Remove the native Dell Color Laser 1320c driver and its queue.
#   sudo ./uninstall.sh
set -eu

QUEUE=Dell_1320c_Native

if [ "$(id -u)" != 0 ]; then
    echo "Please run with sudo: sudo $0" >&2
    exit 1
fi
if lpstat -p "$QUEUE" >/dev/null 2>&1; then
    echo "Removing queue $QUEUE"
    lpadmin -x "$QUEUE"
fi
rm -f "/Library/Printers/PPDs/Contents/Resources/Dell Color Laser 1320c (native).gz"
rm -f /Library/Printers/Dell1320c/rastertodell1320c /Library/Printers/Dell1320c/Dell_Color_Laser_1320c.ppd
rmdir /Library/Printers/Dell1320c 2>/dev/null || true
echo "Removed."
