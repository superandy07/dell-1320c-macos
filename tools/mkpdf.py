# mkpdf.py OUT.pdf [grid N] : 2-page patch test, or one-page RGB grid of N^3 patches
import sys
def pdf(pages, out, w=595, h=842):
    objs = ["<< /Type /Catalog /Pages 2 0 R >>", None,
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    kids = []
    for content in pages:
        c = content.encode()
        objs.append("<< /Length %d >>\nstream\n%s\nendstream" % (len(c), content))
        objs.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Contents %d 0 R /Resources << /Font << /F1 3 0 R >> >> >>" % (w, h, len(objs)))
        kids.append("%d 0 R" % len(objs))
    objs[1] = "<< /Type /Pages /Kids [%s] /Count %d >>" % (" ".join(kids), len(kids))
    b = bytearray(b"%PDF-1.4\n"); offs = []
    for i, o in enumerate(objs):
        offs.append(len(b)); b += ("%d 0 obj\n%s\nendobj\n" % (i + 1, o)).encode()
    x = len(b)
    b += ("xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)).encode()
    for o in offs: b += ("%010d 00000 n \n" % o).encode()
    b += ("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)).encode()
    open(out, "wb").write(b)
out = sys.argv[1]
if len(sys.argv) > 2 and sys.argv[2] == "grid":
    n = int(sys.argv[3]); s = 12  # patch size in pt
    ops = []
    for i in range(n**3):
        r, g, b = i // (n*n), (i // n) % n, i % n
        x, y = 40 + (i % 40) * s, 800 - (i // 40) * s
        ops.append("%.4f %.4f %.4f rg %d %d %d %d re f" % (r/(n-1), g/(n-1), b/(n-1), x, y - s, s, s))
    pdf(["/DeviceRGB cs\n" + "\n".join(ops)], out)
else:
    p1 = ["BT /F1 24 Tf 72 720 Td (Dell 1320c test) Tj ET"]
    for r in range(5):
        for g in range(5):
            p1.append("%.2f %.2f 0.5 rg %d %d 55 55 re f" % (r/4, g/4, 72+r*60, 400+g*60))
    p1.append("0 0 0 rg 72 300 400 20 re f")
    pdf(["\n".join(p1), "BT /F1 24 Tf 72 720 Td (Page two) Tj ET"], out)
