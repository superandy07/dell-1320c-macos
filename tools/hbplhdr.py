# hbplhdr.py FILE... : print PJL lines and HBPL v1 record attributes (image data skipped)
import sys, re
SZ = {0xa1:1, 0xa2:2, 0xa4:4, 0xc2:4, 0xc3:4, 0xc4:8}
def parse(b):
    out = []; i = 0
    m = re.search(rb'@PJL ENTER LANGUAGE=HBPL\r?\n', b)
    pre, i = b[:m.end()], m.end()
    out += [("PJL", l) for l in pre.decode('latin1').splitlines() if '@PJL' in l and not re.search('DATE=|TIME=', l)]
    page = 0
    while i < len(b):
        t = b[i]; i += 1
        if t == 0x42:   # 'B' end of job
            out.append(("REC", "B")); break
        if t in (0x51, 0x53, 0x44):
            out.append(("REC", chr(t))); continue
        if t == 0x43: page += 1
        out.append(("REC", chr(t)))
        while i < len(b) and b[i] >= 0x80:
            tag = b[i]; ty = b[i+1]; i += 2
            if ty == 0xb1:    # array follows: next type/len, then data
                ty2 = b[i]; n = int.from_bytes(b[i+1:i+1+SZ[ty2]], 'little'); i += 1 + SZ[ty2]
                out.append(("ATTR", "p%d %s %02x data[%d]" % (page, chr(t), tag, n)))
                hdr = b[i:i+48]
                out.append(("ATTR", "p%d %s %02x body48=%s" % (page, chr(t), tag, hdr[:20].hex())))
                i += n; continue
            n = SZ[ty]; v = b[i:i+n]; i += n
            out.append(("ATTR", "p%d %s %02x %02x=%s" % (page, chr(t), tag, ty, v.hex())))
    tail = b[i:].decode('latin1')
    out += [("TAIL", l) for l in tail.splitlines() if '@PJL' in l]
    return out
for f in sys.argv[1:]:
    for k, v in parse(open(f, 'rb').read()):
        print(k, v)
