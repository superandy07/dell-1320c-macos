#!/usr/bin/env python3
"""
roundtrip.py - lossless round-trip test of the HBPL encoder.

For each case: build a synthetic CUPS raster page (noise, gradients, flat
areas and content touching every edge), run the filter with colour tables
disabled (DELL1320C_TEST_RAW=1), decode the result with build/hbpl1x and
check the decoded pixels are byte-identical to the input.

usage: roundtrip.py ROOT OUTDIR
"""
import os
import random
import struct
import subprocess
import sys

ROOT, OUT = sys.argv[1], sys.argv[2]
PPD = os.path.join(ROOT, "ppd", "Dell_Color_Laser_1320c.ppd")
FILTER = os.path.join(ROOT, "build", "rastertodell1320c")
DECODE = os.path.join(ROOT, "build", "hbpl1x")
BLANK = os.path.join(OUT, "blank.pdf")


def blank_pdf():
    objs = ["<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>"]
    b = bytearray(b"%PDF-1.4\n"); offs = []
    for i, o in enumerate(objs):
        offs.append(len(b)); b += ("%d 0 obj\n%s\nendobj\n" % (i + 1, o)).encode()
    x = len(b)
    b += ("xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)).encode()
    for o in offs:
        b += ("%010d 00000 n \n" % o).encode()
    b += ("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)).encode()
    open(BLANK, "wb").write(b)


def raster_header(opts):
    """Render a blank page through macOS to get a real raster header."""
    r = subprocess.run(["cupsfilter", "-p", PPD, "-m", "application/vnd.cups-raster"] +
                       sum([["-o", o] for o in opts], []) + [BLANK],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True)
    hdr = r.stdout[:1800]
    assert hdr[:4] == b"3SaR", hdr[:4]
    u = lambda o: struct.unpack("<I", hdr[4 + o:8 + o])[0]
    return hdr, u(372), u(376), u(392)


def make_page(w, h, bpl, seed):
    rnd = random.Random(seed)
    img = bytearray(b"\xff" * (bpl * h))

    def rect(x0, y0, x1, y1, rgb):
        x0, x1, y0, y1 = max(0, x0), min(w, x1), max(0, y0), min(h, y1)
        line = bytes(rgb) * (x1 - x0)
        for y in range(y0, y1):
            img[y * bpl + x0 * 3: y * bpl + x1 * 3] = line

    # content touching all four edges and corners
    rect(0, 0, w, 3, (0, 0, 0))
    rect(0, h - 3, w, h, (10, 200, 30))
    rect(0, 0, 5, h, (250, 0, 120))
    rect(w - 5, 0, w, h, (0, 90, 255))
    # flat patches of random colours
    for _ in range(300):
        x, y = rnd.randrange(w), rnd.randrange(h)
        rect(x, y, x + rnd.randrange(1, 300), y + rnd.randrange(1, 200),
             (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)))
    # horizontal and vertical gradients
    for y in range(400, 700):
        if y < h:
            img[y * bpl: y * bpl + min(w, 1200) * 3] = bytes(
                v for x in range(min(w, 1200)) for v in ((x * 255) // 1199, y % 256, 255 - (x * 255) // 1199))
    # pure noise blocks (worst case for the literal coder), incl. at the right edge
    for (x0, y0) in ((900, 900), (w - 160, 1500), (0, 2000)):
        for y in range(y0, min(h, y0 + 120)):
            for x in range(x0, min(w, x0 + 160)):
                o = y * bpl + x * 3
                img[o:o + 3] = bytes((rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)))
    # sparse single pixels and thin lines (exercise every copy direction)
    for _ in range(4000):
        x, y = rnd.randrange(w), rnd.randrange(h)
        o = y * bpl + x * 3
        img[o:o + 3] = bytes((rnd.randrange(256),) * 3)
    for k in range(0, min(w, h), 7):
        o = k * bpl + k * 3
        img[o:o + 3] = b"\x00\x00\x00"
    return bytes(img)


def check(name, opts, color, seed):
    hdr, w, h, bpl = raster_header(opts)
    page = make_page(w, h, bpl, seed)
    ras = os.path.join(OUT, name + ".ras")
    with open(ras, "wb") as f:
        f.write(hdr + page)
    env = dict(os.environ, PPD=PPD, DELL1320C_TEST_RAW="1")
    fopts = " ".join(opts + ([] if color else ["ColorModel=Gray"]))
    hb = os.path.join(OUT, name + ".hbpl")
    with open(hb, "wb") as f:
        subprocess.run([FILTER, "1", "test", name, "1", fopts, ras], stdout=f,
                       stderr=subprocess.DEVNULL, env=env, check=True)
    base = os.path.join(OUT, name + "-dec")
    r = subprocess.run([DECODE, hb, base], stderr=subprocess.PIPE, check=True)
    dec = open(base + "-01.pam", "rb").read()
    dec = dec[dec.index(b"ENDHDR\n") + 7:]
    W = (w + 7) & ~7
    bad = 0
    for y in range(h):
        src = page[y * bpl: y * bpl + w * 3]
        if color:
            row = dec[y * W * 4: (y * W + w) * 4]
            got = bytes(b for i in range(0, len(row), 4) for b in row[i + 1:i + 4])
            pad = set(row[0::4])
        else:
            got = dec[y * W: y * W + w]
            src = src[0::3]
            pad = {255}
        if got != src or pad != {255}:
            bad += 1
    os.remove(ras)
    os.remove(base + "-01.pam")
    status = "PASS" if bad == 0 else "FAIL (%d rows differ)" % bad
    print("%-28s %5dx%-5d %6d bytes  %s" % (name, w, h, os.path.getsize(hb), status))
    return bad == 0


def main():
    os.makedirs(OUT, exist_ok=True)
    blank_pdf()
    cases = [
        ("a4-600-colour", [], True),
        ("a4-600-mono", [], False),
        ("a4-300-colour", ["DLOutputMode=300x300"], True),
        ("letter-600-colour", ["PageSize=Letter"], True),
        ("dl-landscape-colour", ["PageSize=EnvDLRotated"], True),
        ("a5-600-mono", ["PageSize=A5"], False),
    ]
    ok = all([check(n, o, c, i) for i, (n, o, c) in enumerate(cases)])
    print("round-trip:", "all passed" if ok else "FAILURES")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
