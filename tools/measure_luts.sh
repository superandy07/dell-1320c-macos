#!/bin/sh
# Re-measure the colour tables from the old Fuji Xerox C525A driver.
# Needs that driver installed (and Rosetta), and a Fuji-driver queue PPD.
#
#   tools/measure_luts.sh [fuji.ppd]
#
# Writes data/json/*.json, then run tools/genlut.py to rebuild src/lut_data.h.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FXPPD=${1:-/etc/cups/ppd/Dell_Color_Laser_1320c.ppd}
FX=/Library/Printers/FujiXerox/Filter/fxmhbpl.bundle/Contents/MacOS/FXM_MF
W=$(mktemp -d)
OUT="$ROOT/data/json"
mkdir -p "$OUT"
make -C "$ROOT" build/hbpl1x >/dev/null

# 1. a real raster header for an A4 600 dpi RGB page
python3 -I "$ROOT/tools/mkpdf.py" "$W/one.pdf" grid 2
cupsfilter -p "$FXPPD" -m application/vnd.cups-raster "$W/one.pdf" 2>/dev/null | head -c 1800 > "$W/hdr.bin"

# 2. a synthetic page: 33^3 RGB grid + 7 ramps of 256 steps
python3 -I "$ROOT/tools/lutpage.py" "$W/hdr.bin" "$W/grid.ras" "$W/layout.json"

# 3. run it through the Fuji filter for each image type, and in black mode
run() { PPD="$FXPPD" "$FX" 1 u t 1 "$2" "$W/grid.ras" > "$W/$1.hbpl" 2>/dev/null; }
for t in Normal Photo Presentation Web POP; do run "dt_$t" "FXDocumentType=$t"; done
run mono "FXColorMode=Black"

# 4. decode and sample the centre of every patch
for f in dt_Normal dt_Photo dt_Presentation dt_Web dt_POP mono; do
    "$ROOT/build/hbpl1x" "$W/$f.hbpl" "$W/$f" 2>/dev/null
    python3 -I - "$W/layout.json" "$W/$f-01.pam" "$OUT/$f.json" <<'EOF'
import sys, json
lay = json.load(open(sys.argv[1]))
raw = open(sys.argv[2], 'rb').read(); i = raw.index(b'ENDHDR\n')
bpp = 4 if b'DEPTH 4' in raw[:i] else 1; d = raw[i + 7:]; W = 4776
res = []
for x, y, c in lay:
    v = d[(y * W + x) * bpp:(y * W + x) * bpp + bpp]
    res.append([c, list(v[1:]) if bpp == 4 else [v[0]]])
json.dump(res, open(sys.argv[3], 'w'))
EOF
    echo "measured $f"
done
rm -r "$W"
echo "now run: tools/genlut.py data/json"
