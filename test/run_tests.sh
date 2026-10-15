#!/bin/sh
# Run all offline tests (nothing is sent to a printer).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/test/out"
mkdir -p "$OUT"

echo "== PPD check"
cupstestppd -W translations -W filters "$ROOT/ppd/Dell_Color_Laser_1320c.ppd" | head -1

echo "== Encoder round trip (lossless)"
python3 -I "$ROOT/test/roundtrip.py" "$ROOT" "$OUT/roundtrip"

echo "== Comparison with the old Fuji Xerox driver"
python3 -I "$ROOT/test/compare_fuji.py" "$ROOT" "$OUT/cmp"
