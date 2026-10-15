/*
 * hbpl1x - HBPLv1 decoder for the 24-byte "pixel interleave" image header
 * used by the Fuji Xerox DocuPrint C525A / Dell 1320c.
 * Derived from hbpldecode.c (foo2zjs, GPL-2): Dave Coffin's HBPLv1 decoder.
 *
 * usage: hbpl1x file.hbpl outbase   -> outbase-NN.pam
 *   colour pages: 4 bytes per pixel exactly as sent (c0..c3)
 *   mono pages:   1 byte per pixel
 *
 * Experiment switches (env):
 *   HBPL_PRED  0 = left neighbour (default), 1 = pixel above,
 *              2 = left but column 0 predicts from HBPL_BV
 *   HBPL_BV    border value for HBPL_PRED=2 and for the top border row
 *   HBPL_DIRS  "a,b;a,b;..." direction table, each entry a + b*width
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char *p, *end; unsigned buf, bits; } STREAM;

static unsigned getbits(STREAM *s, int n)
{
    while (s->bits < n) {
	s->buf = (s->buf << 8) + (s->p < s->end ? *s->p : 0xff);
	s->p++;
	s->bits += 8;
    }
    s->bits -= n;
    return s->buf << (32 - s->bits - n) >> (32 - n);
}

static int gethuff(STREAM *s, const char *huff)
{
    int i = getbits(s, huff[0]) * 2;
    s->bits += huff[0] - huff[i + 2];
    return huff[i + 1];
}

static unsigned get_len(STREAM *s)
{
    static const short code[] = { 3,3,3,4,5,6,9,14,26,34,42,50,66,98,162,674 };
    int i;
    switch (getbits(s, 3)) {
    case 0: case 1: s->bits++; return 1;
    case 2: return 2;
    case 3: return 3;
    case 4: return 4 + getbits(s, 1);
    case 5: return 6 + getbits(s, 2);
    case 6: return 10 + getbits(s, 4);
    }
    i = getbits(s, 3);
    return code[8 + i] + getbits(s, code[i]);
}

static int get_diff(STREAM *s)
{
    static const short code[] = { 1,2,3,5,5,6,2,4,8,16,48,80 };
    int i, sign;
    switch (i = getbits(s, 3)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return -1;
    case 7: i += getbits(s, 1);
    }
    sign = getbits(s, 1);
    i = code[i + 3] + getbits(s, code[i - 3]);
    return sign ? -i : i;
}

static int envint(const char *n, int def)
{
    const char *v = getenv(n);
    return v ? atoi(v) : def;
}

static void decode(unsigned char *in, int ilen, int page, int color,
		   int width, int height, const char *base)
{
    static const char huff[2][68] = {
	{ 5,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
	  0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
	  5,2,5,2,5,2,5,2,5,2,5,2,5,2,5,2,
	  1,3,1,3,1,3,1,3,2,5,3,5,4,5,6,5 },
	{ 5,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,
	  5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,
	  0,2,0,2,0,2,0,2,0,2,0,2,0,2,0,2,
	  1,3,1,3,1,3,1,3,2,5,3,5,4,5,6,5 },
    };
    union { unsigned i; unsigned char c[4]; } *buf, *px;
    int hdr = in[0] | in[1] << 8, deep = color ? 4 : 1;
    int dirs[] = { -1, 0, -1, 1, 2 }, hsel = 0, bit = 0, off = 0, r, s;
    int pm = envint("HBPL_PRED", 2), bv = envint("HBPL_BV", color ? 0xff : 0);
    char rotor[] = "01234", name[512];
    STREAM st = { 0 };
    FILE *fp;

    buf = calloc((size_t)width * (height + 2), 4);
    memset(buf, bv, (size_t)(width + 1) * 4);
    px = buf + width + 1;
    for (r = 1; r < 5; r++) dirs[r] -= width;
    if (!color) dirs[4] = -8;
    if (getenv("HBPL_DIRS")) {
	const char *q = getenv("HBPL_DIRS");
	int a, b;
	for (r = 0; r < 5 && q && sscanf(q, "%d,%d", &a, &b) == 2; r++) {
	    dirs[r] = a + b * width;
	    if ((q = strchr(q, ';'))) q++;
	}
    }
    st.p = in + hdr;
    st.end = in + ilen;

    while (st.p < st.end && off < width * height) {
	int token = gethuff(&st, huff[hsel]);
	if (token == 6) {
	    hsel = !hsel;
	    getbits(&st, 1);
	    continue;
	}
	if (token == 5) {
	    for (s = 0; s < deep; s++) {
		int pred;
		if (pm == 1)
		    pred = px[off - width].c[s];
		else if (pm == 2 && off % width == 0)
		    pred = color ? 0xff : bv;
		else
		    pred = px[off - 1].c[s];
		px[off].c[s] = pred + get_diff(&st);
	    }
	    off++;
	    bit = 0;
	    continue;
	}
	{
	    int run = get_len(&st), raw = token + bit;
	    int dir = dirs[rotor[raw] - '0'];
	    bit = run < 17057;
	    while (run-- && off < width * height) {
		int col = off % width, dx = ((dir % width) + width + width / 2) % width - width / 2;
		if (pm == 2 && (col + dx < 0 || col + dx >= width))
		    px[off].i = color ? 0xffffffffu : (unsigned)bv * 0x01010101u;
		else
		    px[off].i = px[off + dir].i;
		off++;
	    }
	    if (raw) {
		s = rotor[raw];
		for (r = raw; r; r--) rotor[r] = rotor[r - 1];
		rotor[0] = s;
	    }
	}
    }
    fprintf(stderr, "page %d: hdr=%d color=%d %dx%d decoded %d/%d, %ld bytes left\n",
	    page, hdr, color, width, height, off, width * height,
	    (long)(st.end - st.p));
    snprintf(name, sizeof name, "%s-%02d.pam", base, page);
    if (!(fp = fopen(name, "wb"))) { perror(name); exit(1); }
    fprintf(fp, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH %d\nMAXVAL 255\nTUPLTYPE %s\nENDHDR\n",
	    width, height, color ? 4 : 1, color ? "RGB_ALPHA" : "GRAYSCALE");
    for (off = 0; off < width * height; off++)
	fwrite(px[off].c, 1, color ? 4 : 1, fp);
    fclose(fp);
    free(buf);
}

static unsigned long le(const unsigned char *p, int n)
{
    unsigned long v = 0;
    while (n--) v = v << 8 | p[n];
    return v;
}

int main(int argc, char **argv)
{
    FILE *fp;
    long n;
    unsigned char *b, *p, *end;
    int page = 0, width = 0, height = 0, color = 0;

    if (argc != 3) {
	fprintf(stderr, "usage: hbpl1x file.hbpl outbase\n");
	return 1;
    }
    if (!(fp = fopen(argv[1], "rb"))) { perror(argv[1]); return 1; }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    rewind(fp);
    b = malloc(n + 1);
    if (fread(b, 1, n, fp) != (size_t)n) return 1;
    b[n] = 0;
    fclose(fp);
    end = b + n;
    p = memmem(b, n, "LANGUAGE=HBPL", 13);
    if (!p) { fprintf(stderr, "no HBPL section\n"); return 1; }
    p = memchr(p, '\n', end - p) + 1;
    while (p < end) {
	int rec = *p++;
	if (rec == 'B') break;
	while (p < end && *p >= 0x80) {
	    int tag = *p++, ty = *p++;
	    unsigned long v = 0, v2 = 0;
	    switch (ty) {
	    case 0xa1: v = p[0]; p += 1; break;
	    case 0xa2: v = le(p, 2); p += 2; break;
	    case 0xa4: v = le(p, 4); p += 4; break;
	    case 0xc2: v = le(p, 2); v2 = le(p + 2, 2); p += 4; break;
	    case 0xc3: v = le(p, 2); v2 = le(p + 2, 2); p += 4; break;
	    case 0xc4: v = le(p, 4); v2 = le(p + 4, 4); p += 8; break;
	    case 0xb1: {
		int t2 = *p++;
		unsigned long len = t2 == 0xa2 ? le(p, 2) : le(p, 4);
		p += t2 == 0xa2 ? 2 : 4;
		decode(p, (int)len, ++page, color, width, height, argv[2]);
		p += len;
		continue;
	    }
	    default:
		fprintf(stderr, "bad attribute type %02x at %ld\n", ty, (long)(p - b - 2));
		return 1;
	    }
	    if (rec == 'C' && tag == 0x9a) { width = (int)v; height = (int)v2; }
	    if (rec == 'C' && tag == 0x9d) color = v == 0x16;
	}
    }
    return 0;
}
