#!/bin/sh
# Switch over to the native driver: share and default the native queue,
# remove the old Fuji Xerox queue and the Fuji Xerox driver files.
#   sudo ./switch_to_native.sh
# Back up /Library/Printers/FujiXerox, its PPDs and your queue's PPD first if you
# might want the old driver back (see README).
set -eu

NATIVE=Dell_1320c_Native
OLD=Dell_Color_Laser_1320c
PPDDIR=/Library/Printers/PPDs/Contents/Resources

if [ "$(id -u)" != 0 ]; then
    echo "Please run with sudo: sudo $0" >&2
    exit 1
fi
if ! lpstat -p "$NATIVE" >/dev/null 2>&1; then
    echo "Queue $NATIVE not found - run install.sh first" >&2
    exit 1
fi

echo "Sharing $NATIVE and making it the system default"
lpadmin -p "$NATIVE" -o printer-is-shared=true
lpadmin -d "$NATIVE"

if lpstat -p "$OLD" >/dev/null 2>&1; then
    echo "Removing old queue $OLD"
    lpadmin -x "$OLD"
fi

if grep -l 'Library/Printers/FujiXerox/' /etc/cups/ppd/*.ppd >/dev/null 2>&1; then
    echo "Another queue still uses the Fuji Xerox driver - leaving it installed."
    exit 0
fi

echo "Removing the Fuji Xerox driver"
rm -rf /Library/Printers/FujiXerox
for f in "FX DocuPrint C1190 FS.gz" "FX DocuPrint C525 A-AP.gz" "FX DocuPrint CP105 b.gz" \
         "FX DocuPrint CP205 w.gz" "FX DocuPrint CP205.gz" "FX DocuPrint CP215.gz" \
         "FX DocuPrint CP215w.gz"; do
    rm -f "$PPDDIR/$f"
done
pkgutil --forget COM.FUJIXEROX.PKG.HKAPON >/dev/null 2>&1 || true
echo "Done."
