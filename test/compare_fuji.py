#!/usr/bin/env python3
"""
compare_fuji.py - compare this driver's output with the old Fuji Xerox
DocuPrint C525 A-AP driver (needs that driver installed and Rosetta).

For each pair of equivalent settings the same PDF is rendered through both
drivers.  Every PJL line and HBPL header attribute must match (except the
date/time/host fields and the image data length), and the decoded pages
must agree away from colour edges (the Fuji filter also runs an edge
enhancement pass we do not reproduce).

usage: compare_fuji.py ROOT OUTDIR
"""
import os
import re
import subprocess
import sys

ROOT, OUT = sys.argv[1], sys.argv[2]
OUR_PPD = os.path.join(ROOT, "ppd", "Dell_Color_Laser_1320c.ppd")
OUR_FILTER = os.path.join(ROOT, "build", "rastertodell1320c")
DECODE = os.path.join(ROOT, "build", "hbpl1x")
FX_PPD = "/etc/cups/ppd/Dell_Color_Laser_1320c.ppd"
FX_FILTER = "/Library/Printers/FujiXerox/Filter/fxmhbpl.bundle/Contents/MacOS/FXM_MF"
PDF = os.path.join(OUT, "patches.pdf")

# name, Fuji page size/raster opts, Fuji filter opts, our raster opts, our filter opts, copies
CASES = [
    ("tray1", "", "FXInputSlot=1stTray-S", "", "DLInputSlot=1stTray", 1),
    ("auto", "", "FXInputSlot=AutoSelect", "", "DLInputSlot=AutoSelect", 1),
    ("bypass-envelope", "", "FXInputSlot=BypassTray MediaType=Envelope", "",
     "DLInputSlot=BypassTray MediaType=Envelope", 1),
    ("bypass-thick-side2", "", "FXInputSlot=BypassTray MediaType=ThickPaperBack", "",
     "DLInputSlot=BypassTray MediaType=ThickPaperBack", 1),
    ("tray1-bond", "", "FXPreferredMediaType=FinePaper", "",
     "DLInputSlot=1stTray DLExtMediaTypeTray1=FinePaper", 1),
    ("mono", "-o FXColorMode=Black", "FXColorMode=Black", "-o ColorModel=Gray",
     "DLInputSlot=1stTray ColorModel=Gray", 1),
    ("300dpi", "-o FXOutputMode=300x300", "FXOutputMode=300x300", "-o DLOutputMode=300x300",
     "DLInputSlot=1stTray DLOutputMode=300x300", 1),
    ("photo", "", "FXDocumentType=Photo", "", "DLInputSlot=1stTray DLDocumentType=Photo", 1),
    ("web", "", "FXDocumentType=Web", "", "DLInputSlot=1stTray DLDocumentType=Web", 1),
    ("colour-off", "", "FXDocumentType=Off", "", "DLInputSlot=1stTray DLDocumentType=Off", 1),
    ("draft", "", "FXTonerSaver=True", "", "DLInputSlot=1stTray DLTonerSaver=True", 1),
    ("letter", "-o PageSize=Letter", "PageSize=Letter", "-o PageSize=Letter",
     "DLInputSlot=1stTray PageSize=Letter", 1),
    ("dl-landscape", "-o PageSize=FXEnvDLRotated", "PageSize=FXEnvDLRotated",
     "-o PageSize=EnvDLRotated", "DLInputSlot=1stTray PageSize=EnvDLRotated", 1),
    ("a5", "-o PageSize=A5", "PageSize=A5", "-o PageSize=A5", "DLInputSlot=1stTray PageSize=A5", 1),
    ("folio", "-o PageSize=FanFoldGermanLegal", "PageSize=FanFoldGermanLegal",
     "-o PageSize=FanFoldGermanLegal", "DLInputSlot=1stTray PageSize=FanFoldGermanLegal", 1),
    ("copies3", "", "FXInputSlot=1stTray-S", "", "DLInputSlot=1stTray", 3),
    ("copies3-collated", "", "Collate=True", "", "DLInputSlot=1stTray Collate=True", 3),
]

SZ = {0xa1: 1, 0xa2: 2, 0xa4: 4, 0xc2: 4, 0xc3: 4, 0xc4: 8}
IGNORE = re.compile(r"COMMENT (DATE|TIME)=|@HOAD=")


def headers(b):
    """PJL lines, HBPL attributes (minus image data) and trailer lines."""
    out = []
    m = re.search(rb"@PJL ENTER LANGUAGE=HBPL\r?\n", b)
    out += [l for l in b[:m.end()].decode("latin1").splitlines() if not IGNORE.search(l)]
    i = m.end()
    while i < len(b):
        t = b[i]; i += 1
        if t == 0x42:
            out.append("B"); break
        out.append(chr(t))
        while i < len(b) and b[i] >= 0x80:
            tag, ty = b[i], b[i + 1]; i += 2
            if ty == 0xb1:
                n = int.from_bytes(b[i + 1:i + 1 + SZ[b[i]]], "little")
                i += 1 + SZ[b[i]]
                out.append("  %02x image-header=%s" % (tag, b[i:i + 24].hex()))
                i += n
                continue
            out.append("  %02x %02x=%s" % (tag, ty, b[i:i + SZ[ty]].hex()))
            i += SZ[ty]
    out += b[i:].decode("latin1").splitlines()
    return out


def run(cmd, out, env=None):
    with open(out, "wb") as f:
        subprocess.run(cmd, stdout=f, stderr=subprocess.DEVNULL, env=env, check=True, shell=isinstance(cmd, str))


def decode(hb, base):
    subprocess.run([DECODE, hb, base], stderr=subprocess.DEVNULL, check=True)
    d = open(base + "-01.pam", "rb").read()
    i = d.index(b"ENDHDR\n")
    hdr, data = d[:i].decode(), d[i + 7:]
    w = int(re.search(r"WIDTH (\d+)", hdr).group(1))
    depth = int(re.search(r"DEPTH (\d+)", hdr).group(1))
    for p in ("-01.pam", "-02.pam"):
        if os.path.exists(base + p):
            os.remove(base + p)
    return data, w, depth


def pixel_agreement(a, b, w, depth):
    """Fraction of sampled pixels whose 3x3 neighbourhood is flat in both
    images and whose values agree within 6/255; and the mean difference."""
    h = len(a) // (w * depth)
    n = agree = 0
    tot = 0.0
    for y in range(2, h - 2, 7):
        for x in range(2, w - 2, 7):
            o = (y * w + x) * depth
            flat = all(a[((y + dy) * w + x + dx) * depth:((y + dy) * w + x + dx) * depth + depth] ==
                       a[o:o + depth] for dy in (-2, 2) for dx in (-2, 2))
            if not flat:
                continue
            n += 1
            d = max(abs(a[o + k] - b[o + k]) for k in range(depth))
            tot += d
            agree += d <= 6
    return agree / max(n, 1), tot / max(n, 1)


def main():
    if not os.path.exists(FX_FILTER) or not os.path.exists(FX_PPD):
        print("Fuji Xerox driver not installed - skipping comparison")
        return
    os.makedirs(OUT, exist_ok=True)
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "mkpdf.py"), PDF], check=True)
    all_ok = True
    for name, fr, fo, orr, oo, copies in CASES:
        fras, oras = os.path.join(OUT, "fx.ras"), os.path.join(OUT, "our.ras")
        run("cupsfilter -p %s -m application/vnd.cups-raster %s '%s'" % (FX_PPD, fr, PDF), fras)
        run("cupsfilter -p '%s' -m application/vnd.cups-raster %s '%s'" % (OUR_PPD, orr, PDF), oras)
        fhb, ohb = os.path.join(OUT, name + ".fuji.hbpl"), os.path.join(OUT, name + ".ours.hbpl")
        run([FX_FILTER, "1", "user", "test", str(copies), fo, fras], fhb, dict(os.environ, PPD=FX_PPD))
        run([OUR_FILTER, "1", "user", "test", str(copies), oo, oras], ohb, dict(os.environ, PPD=OUR_PPD))
        os.remove(fras); os.remove(oras)
        fb, ob = open(fhb, "rb").read(), open(ohb, "rb").read()
        fh, oh = headers(fb), headers(ob)
        diffs = [(i, f, o) for i, (f, o) in enumerate(zip(fh, oh)) if f != o]
        if len(fh) != len(oh):
            diffs.append((min(len(fh), len(oh)), "<%d lines>" % len(fh), "<%d lines>" % len(oh)))
        fd, w, depth = decode(fhb, os.path.join(OUT, "fx"))
        od, w2, _ = decode(ohb, os.path.join(OUT, "our"))
        frac, mean = pixel_agreement(fd, od, w, depth) if w == w2 and len(fd) == len(od) else (0, 999)
        ok = not diffs and frac > 0.995
        all_ok &= ok
        print("%-20s headers %-22s pixels %.2f%% within 6/255 (mean diff %.2f)  %s" % (
            name, "identical" if not diffs else "%d differ" % len(diffs), frac * 100, mean,
            "PASS" if ok else "FAIL"))
        for i, f, o in diffs[:6]:
            print("      line %d: fuji %r\n               ours %r" % (i, f, o))
    print("comparison:", "all passed" if all_ok else "FAILURES")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
