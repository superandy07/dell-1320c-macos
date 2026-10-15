# lutpage.py hdr.bin out.ras layout.json : synthetic RGB raster with a 17^3 grid + 7 x 256 ramps
import sys, json
hdr = open(sys.argv[1], 'rb').read(1800)
W, H, BPL = 4770, 6829, 14310
N, P = 33, 24
lv = [min(255, round(i * 255 / (N - 1))) for i in range(N)]
pat = []                                   # (x, y, w, h, rgb)
per = W // P
for i in range(N ** 3):
    r, g, b = lv[i // (N * N)], lv[(i // N) % N], lv[i % N]
    pat.append(((i % per) * P, 200 + (i // per) * P, P, P, (r, g, b)))
y0 = 200 + ((N ** 3 + per - 1) // per) * P + 60
ramps = [lambda v: (v, v, v), lambda v: (v, 0, 0), lambda v: (0, v, 0), lambda v: (0, 0, v),
         lambda v: (255 - v, 255, 255), lambda v: (255, 255 - v, 255), lambda v: (255, 255, 255 - v)]
for k, f in enumerate(ramps):
    for v in range(256):
        pat.append((v * 18, y0 + k * 60, 18, 48, f(v)))
img = bytearray(b'\xff' * (BPL * H))
for x, y, w, h, c in pat:
    row = bytes(c) * w
    for yy in range(y, y + h):
        o = yy * BPL + x * 3; img[o:o + w * 3] = row
open(sys.argv[2], 'wb').write(hdr + img)
json.dump([(x + w // 2, y + h // 2, c) for x, y, w, h, c in pat], open(sys.argv[3], 'w'))
print(len(pat), 'patches, ramps end at', y0 + 7 * 60)
