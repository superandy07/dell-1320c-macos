/*
 * rastertodell1320c - native CUPS raster filter for the Dell Color Laser
 * 1320c (a rebadged Fuji Xerox DocuPrint C525 A-AP).
 *
 * The printer speaks HBPL version 1: a PJL job header, a stream of tagged
 * binary records, and per page an image compressed with Fuji Xerox's
 * "LRLE2" scheme (five-direction run copying plus Huffman-coded literal
 * differences).  The 1320c wants the variant with a 24-byte image header
 * and a single bitstream with the literal differences interleaved, sending
 * 8-bit RGB (colour) or 8-bit grey (black) and doing its own colour
 * separation and halftoning.
 *
 * The run/literal encoder is derived from foo2hbpl1.c by Dave Coffin
 * (foo2zjs project).  The job/page headers were mapped from the output of
 * the Fuji Xerox / Dell drivers.  The colour tables in lut_data.h were
 * measured from the Fuji Xerox driver, see tools/measure_luts.sh.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * usage (CUPS filter): rastertodell1320c job user title copies options [file]
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lut_data.h"

#define MAXRUN 17057

/* ------------------------------------------------------------------ */
/* Job options                                                          */
/* ------------------------------------------------------------------ */

enum { TRAY_AUTO, TRAY_BYPASS, TRAY_1 };

typedef struct {
    const char *name;		/* DLDocumentType choice */
    int cpr, trc;		/* @xCPR/@xUCR and @xTRC/@xSCR values */
    const unsigned char *lut;	/* NULL = no host colour correction */
} doctype_t;

static const doctype_t doctypes[] = {
    { "Normal",       24,  9, lut_normal },
    { "Photo",        22, 11, lut_photo },
    { "Presentation", 26, 13, lut_presentation },
    { "Web",          23,  9, lut_web },
    { "POP",          32,  9, lut_pop },
    { "Off",          24,  9, NULL },
};

typedef struct {
    int color;			/* 1 = colour, 0 = black only */
    int tray;			/* TRAY_* */
    const char *msip;		/* manual feeder media type */
    int backface;		/* manual feeder "Side 2" media */
    const char *apsp;		/* tray 1 media type */
    const doctype_t *doc;
    int turn;			/* rotate 180 degrees */
    int skip_blank;
    int toner_saver;
    int collate;
    int copies;
} job_t;

/* PPD media choice -> printer media name */
static const char *media_name(const char *choice, int *back)
{
    static const struct { const char *choice, *name; } map[] = {
	{ "Stationary", "NORMAL" }, { "FinePaper", "FINE" },
	{ "Envelope", "ENVELOPE" }, { "Postcard", "PCMEDIA" },
	{ "ThickPaper", "THICK" }, { "ThickPaper2", "THICK2" },
	{ "CoatingPaper1", "COAT1" }, { "CoatingPaper2", "COAT2" },
	{ "CoatingPaper3", "COAT3" }, { "LabelPaper", "LABEL" },
	{ "Recycled", "RECYCLE" }, { "Transparency", "TRANSPARENCY" },
    };
    char base[64];
    size_t n;
    unsigned i;

    *back = 0;
    if (!choice)
	return "NORMAL";
    snprintf(base, sizeof base, "%s", choice);
    n = strlen(base);
    if (n > 4 && !strcmp(base + n - 4, "Back")) {
	base[n - 4] = 0;
	*back = 1;
    }
    for (i = 0; i < sizeof map / sizeof *map; i++)
	if (!strcmp(base, map[i].choice))
	    return map[i].name;
    *back = 0;
    return "NORMAL";
}

static const char *marked(ppd_file_t *ppd, const char *option)
{
    ppd_choice_t *c = ppd ? ppdFindMarkedChoice(ppd, option) : NULL;
    return c ? c->choice : NULL;
}

static int is_true(const char *s)
{
    return s && (!strcasecmp(s, "True") || !strcasecmp(s, "On") ||
		 !strcasecmp(s, "Yes"));
}

static void get_job_options(job_t *job, int argc, char **argv)
{
    int num_options = 0, back;
    cups_option_t *options = NULL;
    ppd_file_t *ppd = NULL;
    const char *v;
    unsigned i;

    if (argc > 5)
	num_options = cupsParseOptions(argv[5], 0, &options);
    if (getenv("PPD") && (ppd = ppdOpenFile(getenv("PPD")))) {
	ppdMarkDefaults(ppd);
	cupsMarkOptions(ppd, num_options, options);
    }

    /* standard ColorModel (RGB/Gray); DLColorMode is the old Dell name */
    v = cupsGetOption("DLColorMode", num_options, options);
    if (!v)
	v = marked(ppd, "ColorModel");
    job->color = !(v && (!strcmp(v, "Gray") || !strcmp(v, "Black")));

    v = marked(ppd, "DLInputSlot");
    job->tray = !v || !strcmp(v, "AutoSelect") ? TRAY_AUTO
	      : !strcmp(v, "BypassTray") ? TRAY_BYPASS : TRAY_1;

    job->msip = "NORMAL";
    job->backface = 0;
    if (job->tray == TRAY_BYPASS) {
	job->msip = media_name(marked(ppd, "MediaType"), &back);
	job->backface = back;
    }
    v = marked(ppd, "DLExtMediaTypeTray1");
    job->apsp = !v || !strcmp(v, "Default") ? "OFF" : media_name(v, &back);

    job->doc = &doctypes[0];
    if ((v = marked(ppd, "DLDocumentType")))
	for (i = 0; i < sizeof doctypes / sizeof *doctypes; i++)
	    if (!strcmp(v, doctypes[i].name))
		job->doc = &doctypes[i];

    job->turn = is_true(marked(ppd, "DLTurnPage"));
    job->skip_blank = is_true(marked(ppd, "DLSkip"));
    job->toner_saver = is_true(marked(ppd, "DLTonerSaver"));
    job->collate = is_true(marked(ppd, "Collate")) ||
		   is_true(cupsGetOption("collate", num_options, options));
    job->copies = argc > 4 ? atoi(argv[4]) : 1;
    if (job->copies < 1)
	job->copies = 1;
    if (job->copies > 999)
	job->copies = 999;

    cupsFreeOptions(num_options, options);
    if (ppd)
	ppdClose(ppd);
}

/* ------------------------------------------------------------------ */
/* Output buffer                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *buf;
    size_t len, size;
} obuf_t;

static void ob_put(obuf_t *o, const void *p, size_t n)
{
    if (o->len + n > o->size) {
	o->size = (o->len + n) * 2 + 65536;
	if (!(o->buf = realloc(o->buf, o->size))) {
	    fputs("ERROR: out of memory\n", stderr);
	    exit(1);
	}
    }
    memcpy(o->buf + o->len, p, n);
    o->len += n;
}

static void ob_byte(obuf_t *o, int b)
{
    unsigned char c = (unsigned char)b;
    ob_put(o, &c, 1);
}

static void ob_le(obuf_t *o, unsigned long v, int n)
{
    while (n--) {
	ob_byte(o, v & 0xff);
	v >>= 8;
    }
}

/* ------------------------------------------------------------------ */
/* Bit writer and LRLE2 codes (from foo2hbpl1)                          */
/* ------------------------------------------------------------------ */

typedef struct {
    obuf_t *o;
    uint64_t acc;
    int n;
} bits_t;

static void putbits(bits_t *s, unsigned val, int nbits)
{
    s->acc = (s->acc << nbits) | (val & ((1u << nbits) - 1));
    s->n += nbits;
    while (s->n >= 8) {
	s->n -= 8;
	ob_byte(s->o, (int)(s->acc >> s->n));
    }
}

/* Pad the last byte with 1 bits and add a 0xff byte, as the Fuji driver does. */
static void flushbits(bits_t *s)
{
    if (s->n)
	putbits(s, 0xff, 8 - s->n);
    ob_byte(s->o, 0xff);
}

/*
 * Run lengths 1..17057:
 *   1 00, 2 010, 3 011, 4-5 100x, 6-9 101xx, 10-25 110xxxx,
 *   26-33 111000xxx, 34-41 111001xxx, 42-49 111010xxx, 50-65 111011xxxx,
 *   66-97 111100xxxxx, 98-161 111101xxxxxx, 162-673 111110x(9),
 *   674-17057 111111x(14)
 */
static void put_len(bits_t *s, unsigned val)
{
    static const unsigned code[] = {
	  1, 0, 2,
	  2, 2, 3,
	  4, 8, 4,
	  6, 0x14, 5,
	 10, 0x60, 7,
	 26, 0x1c0, 9,
	 50, 0x3b0, 10,
	 66, 0x780, 11,
	 98, 0xf40, 12,
	162, 0x7c00, 15,
	674, 0xfc000, 20,
	17058
    };
    int c = 0;

    while (val >= code[c + 3])
	c += 3;
    putbits(s, val - code[c] + code[c + 1], (int)code[c + 2]);
}

/*
 * Byte differences -128..128:
 *   0 000, +1 001, -1 010, 2-3 011sx, 4-7 100sxx, 8-15 101sxxx,
 *   16-47 110sxxxxx, 48-79 1110sxxxxx, 80-128 1111sxxxxxx
 */
static void put_diff(bits_t *s, int val)
{
    static const unsigned short code[] = {
	 2,  3, 3, 1,
	 4,  4, 3, 2,
	 8,  5, 3, 3,
	16,  6, 3, 5,
	48, 14, 4, 5,
	80, 15, 4, 6,
	129
    };
    int sign, abs, c = 0;

    switch (val) {
    case  0: putbits(s, 0, 3); return;
    case  1: putbits(s, 1, 3); return;
    case -1: putbits(s, 2, 3); return;
    }
    abs = (sign = val < 0) ? -val : val;
    while (abs >= code[c + 4])
	c += 4;
    putbits(s, code[c + 1], code[c + 2]);
    putbits(s, (unsigned)sign, 1);
    putbits(s, (unsigned)(abs - code[c]), code[c + 3]);
}

/* Huffman codes for tokens 0-4 (copy directions), 5 (literal), 6 (switch). */
static const unsigned short huff[2][7] = {
    { 0x01, 0x63, 0x1c5, 0x1d5, 0x1e5, 0x22, 0x3e6 },	/* text & graphics */
    { 0x22, 0x63, 0x1c5, 0x1d5, 0x1e5, 0x01, 0x3e6 },	/* images */
};

#define put_token(s, h, x) putbits(s, huff[h][x] >> 4, huff[h][x] & 15)

/*
 * Encode one page of 32-bit pixel units (w x h, w a multiple of 8).
 *
 * The printer treats the page as a true 2-D image: references to the
 * left of column 0, right of the last column, or above row 0 do not wrap
 * around.  The encoder therefore never copies from outside the page, and
 * a literal in column 0 is predicted from the border value (0xff in each
 * colour byte, 0 for grey) - both established by decoding the Fuji
 * driver's output.
 */
static void encode_page(obuf_t *o, const uint32_t *img, int w, int h, int color)
{
    /* direction table: dx, dy (dy is 0 or -1) */
    const int dx[5] = { -1, 0, -1, 1, color ? 2 : -8 };
    const int dy[5] = {  0, -1, -1, -1, color ? -1 : 0 };
    const int deep = color ? 4 : 1;
    const unsigned border = color ? 0xff : 0;
    long total = (long)w * h, off = 0;
    int rotor[5] = { 0, 1, 2, 3, 4 };
    int i, bit = 0, hsel = 0, stat = 0;
    bits_t bs = { o, 0, 0 };

    while (off < total) {
	int bdir = 0, brun = 0, dir;
	long y = off / w;
	int x = (int)(off % w);

	for (dir = 0; dir < 5; dir++) {
	    int d = rotor[dir], cx = x, run = 0;
	    long delta = dx[d] + (long)dy[d] * w, cy = y;
	    long lim = total - off;

	    if (lim > MAXRUN)
		lim = MAXRUN;
	    while (run < lim) {
		int rx = cx + dx[d];
		if (rx < 0 || rx >= w || cy + dy[d] < 0)
		    break;
		if (img[off + run] != img[off + run + delta])
		    break;
		run++;
		if (++cx == w) {
		    cx = 0;
		    cy++;
		}
	    }
	    if (run > brun) {
		bdir = dir;
		brun = run;
	    }
	}

	if (brun == 0) {
	    /* literal pixel: per-byte difference from the left neighbour */
	    uint32_t cur = img[off], prev = x ? img[off - 1] : border * 0x01010101u;
	    put_token(&bs, hsel, 5);
	    for (i = 0; i < deep; i++)
		put_diff(&bs, (signed char)((cur >> (8 * i)) - (prev >> (8 * i))));
	    bit = 0;
	    off++;
	    stat--;
	    continue;
	}

	if (bdir) {
	    int j = rotor[bdir];
	    for (i = bdir; i; i--)
		rotor[i] = rotor[i - 1];
	    rotor[0] = j;
	}
	if (off && (off - 1 + brun) / w != (off - 1) / w) {
	    if (abs(stat) > 8 && ((stat >> 31) & 1) != hsel) {
		hsel ^= 1;
		put_token(&bs, hsel, 6);	/* same code in both tables */
	    }
	    stat = 0;
	}
	stat += bdir == bit;
	put_token(&bs, hsel, bdir - bit);
	put_len(&bs, (unsigned)brun);
	bit = brun < MAXRUN;
	off += brun;
    }
    flushbits(&bs);
}

/* ------------------------------------------------------------------ */
/* Colour conversion                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char idx[256];
    unsigned short frac[256];	/* 0..4096 */
} axis_t;

static void init_axis(axis_t *a)
{
    int v, i;

    for (v = 0; v < 256; v++) {
	for (i = 0; i < LUT_N - 2 && lut_nodes[i + 1] <= v; i++)
	    ;
	a->idx[v] = (unsigned char)i;
	a->frac[v] = (unsigned short)(((v - lut_nodes[i]) * 4096 +
		    (lut_nodes[i + 1] - lut_nodes[i]) / 2) /
		    (lut_nodes[i + 1] - lut_nodes[i]));
    }
}

/* Tetrahedral interpolation in a 33^3 grid with ch output channels. */
static void lut_lookup(const axis_t *a, const unsigned char *lut, int ch,
		       int r, int g, int b, int *out)
{
    int i = a->idx[r], j = a->idx[g], k = a->idx[b];
    int fr = a->frac[r], fg = a->frac[g], fb = a->frac[b];
    const unsigned char *c000 = lut + ((i * LUT_N + j) * LUT_N + k) * ch;
    const int sr = LUT_N * LUT_N * ch, sg = LUT_N * ch, sb = ch;
    const unsigned char *p1, *p2, *c111 = c000 + sr + sg + sb;
    int w1, w2, w3, n;

    if (fr >= fg && fg >= fb)      { p1 = c000 + sr; p2 = p1 + sg; w1 = fr; w2 = fg; w3 = fb; }
    else if (fr >= fb && fb >= fg) { p1 = c000 + sr; p2 = p1 + sb; w1 = fr; w2 = fb; w3 = fg; }
    else if (fb >= fr && fr >= fg) { p1 = c000 + sb; p2 = p1 + sr; w1 = fb; w2 = fr; w3 = fg; }
    else if (fg >= fr && fr >= fb) { p1 = c000 + sg; p2 = p1 + sr; w1 = fg; w2 = fr; w3 = fb; }
    else if (fg >= fb && fb >= fr) { p1 = c000 + sg; p2 = p1 + sb; w1 = fg; w2 = fb; w3 = fr; }
    else                           { p1 = c000 + sb; p2 = p1 + sg; w1 = fb; w2 = fg; w3 = fr; }

    for (n = 0; n < ch; n++) {
	int v = c000[n] * 4096 + w1 * (p1[n] - c000[n]) +
		w2 * (p2[n] - p1[n]) + w3 * (c111[n] - p2[n]);
	v = (v + 2048) >> 12;
	out[n] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
}

/* ------------------------------------------------------------------ */
/* Paper sizes (codes and limits verified against the Fuji driver)      */
/* ------------------------------------------------------------------ */

static const struct { int w, h, code; } papers[] = {
    { 612,  792, 0x00 },	/* Letter */
    { 612, 1008, 0x01 },	/* Legal */
    { 595,  842, 0x02 },	/* A4 */
    { 522,  756, 0x03 },	/* Executive */
    { 297,  684, 0x06 },	/* #10 envelope */
    { 279,  540, 0x07 },	/* Monarch envelope */
    { 459,  649, 0x08 },	/* C5 envelope */
    { 312,  624, 0x09 },	/* DL envelope */
    { 516,  729, 0x0b },	/* B5 (JIS) */
    { 283,  420, 0x0d },	/* Japanese postcard */
    { 421,  595, 0x0f },	/* A5 */
    { 612,  936, 0xcd },	/* Folio */
    { 323,  459, 0xe0 },	/* C6 envelope */
    { 278,  420, 0xe1 },	/* Youkei 3 */
    { 340,  666, 0xe4 },	/* Choukei 3 */
};

static int paper_code(int w, int h)
{
    int s = w < h ? w : h, l = w < h ? h : w;
    unsigned i;

    for (i = 0; i < sizeof papers / sizeof *papers; i++)
	if (abs(s - papers[i].w) <= 2 && abs(l - papers[i].h) <= 2)
	    return papers[i].code;
    return -1;
}

/* ------------------------------------------------------------------ */
/* PJL                                                                  */
/* ------------------------------------------------------------------ */

/* Make a string safe for a PJL value: printable, no quotes, bounded. */
static void pjl_clean(char *dst, size_t size, const char *src)
{
    size_t n = 0;

    for (; src && *src && n + 1 < size; src++) {
	unsigned char c = (unsigned char)*src;
	if (c < 0x20 || c == 0x7f || c == '"')
	    c = '_';
	dst[n++] = (char)c;
    }
    dst[n] = 0;
}

static void host_address(char *buf, size_t size)
{
    struct ifaddrs *ifa, *p;
    unsigned long a = 0x7f000001;

    if (!getifaddrs(&ifa)) {
	for (p = ifa; p; p = p->ifa_next)
	    if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET) {
		unsigned long v = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
		if ((v >> 24) != 127) {
		    a = v;
		    break;
		}
	    }
	freeifaddrs(ifa);
    }
    snprintf(buf, size, "I%lX%lX%lX%lX", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

static void write_pjl_header(FILE *fp, const job_t *job, const char *user,
			     const char *title, int lef, int res)
{
    char u[128], t[128], host[256], addr[32], datestr[16], timestr[16];
    const char *s;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int n;

    pjl_clean(u, sizeof u, user);
    pjl_clean(t, sizeof t, title);
    if (gethostname(host, sizeof host))
	strcpy(host, "localhost");
    host[sizeof host - 1] = 0;
    pjl_clean(host, sizeof host, host);
    host_address(addr, sizeof addr);
    strftime(datestr, sizeof datestr, "%m/%d/%Y", tm);
    strftime(timestr, sizeof timestr, "%H:%M:%S", tm);

    fprintf(fp, "\033%%-12345X@PJL JOB MODE=PRINTER\n");
    fprintf(fp, "@PJL SET STRINGCODESET=UTF8\n");
    fprintf(fp, "@PJL COMMENT DATE=%s\n", datestr);
    fprintf(fp, "@PJL COMMENT TIME=%s\n", timestr);
    fprintf(fp, "@PJL COMMENT DNAME=%s\n", t);
    fprintf(fp, "@PJL SET JOBATTR=\"@LUNA=%s\"\n", u);
    if (job->collate && job->copies > 1)
	fprintf(fp, "@PJL SET QTY=1\n");
    else
	fprintf(fp, "@PJL SET COPIES=%d\n", job->copies);
    fprintf(fp, "@PJL SET JOBATTR=\"@TRCH=%s\"\n", job->tray == TRAY_AUTO ? "ON" : "OFF");
    if (job->tray == TRAY_1)
	fprintf(fp, "@PJL SET JOBATTR=\"@INPT=INTRAY1\"\n");
    fprintf(fp, "@PJL SET DUPLEX=OFF\n");
    fprintf(fp, "@PJL SET JOBATTR=\"@SPSE=AUTO\"\n");
    fprintf(fp, "@PJL SET JOBATTR=\"@MSIP=%s\"\n", job->msip);
    if (job->backface)
	fprintf(fp, "@PJL SET MEDIAFACE=BACKFACE\n");
    fprintf(fp, "@PJL SET RENDERMODE=%s\n", job->color ? "COLOR" : "GRAYSCALE");
    fprintf(fp, "@PJL SET ECONOMODE=%s\n", job->toner_saver ? "ON" : "OFF");
    fprintf(fp, "@PJL SET JOBATTR=\"@IREC=OFF\"\n");
    fprintf(fp, "@PJL SET JOBATTR=\"@NLPP=1\"\n");
    fprintf(fp, "@PJL SET JOBATTR=\"@HOAD=%s\"\n", addr);
    fprintf(fp, "@PJL SET JOBATTR=\"@JOAU=%s\"\n", u);
    fprintf(fp, "@PJL SET JOBATTR=\"@CNAM=%s\"\n", host);
    fprintf(fp, "@PJL SET OUTBIN=FACEDOWN\n");
    fprintf(fp, "@PJL SET JOBATTR=\"@PODR=NORMAL\"\n");
    fprintf(fp, "@PJL SET SLIPSHEET=OFF\n");
    fprintf(fp, "@PJL SET PAPERDIRECTION=%s\n", lef ? "LEF" : "SEF");
    fprintf(fp, "@PJL SET JOBATTR=\"@APSP=%s\"\n", job->apsp);
    fprintf(fp, "@PJL SET RESOLUTION=%d\n", res);
    fprintf(fp, "@PJL SET BITSPERPIXEL=8\n");
    fprintf(fp, "@PJL SET JOBATTR=\"@DRDM=RASTER\"\n");
    if (job->color) {	/* colour profile and UCR tables; not sent for black */
	for (s = "TGI"; *s; s++)
	    fprintf(fp, "@PJL SET JOBATTR=\"@%cCPR=%d\"\n", *s, job->doc->cpr);
	for (s = "TGI"; *s; s++)
	    fprintf(fp, "@PJL SET JOBATTR=\"@%cUCR=%d\"\n", *s, job->doc->cpr);
    }
    n = job->color ? job->doc->trc : 59;
    for (s = "TGI"; *s; s++)
	fprintf(fp, "@PJL SET JOBATTR=\"@%cTRC=%d\"\n", *s, n);
    for (s = "TGI"; *s; s++)
	fprintf(fp, "@PJL SET JOBATTR=\"@%cSCR=%d\"\n", *s, n);
    if (!job->color)
	fprintf(fp, "@PJL SET JOBATTR=\"@PBLK=OFF\"\n");
    fprintf(fp, "@PJL SET JOBATTR=\"@BANR=DEVICE\"\n");
    if (!job->doc->lut)
	fprintf(fp, "@PJL SET JOBATTR=\"@FCMS=ON\"\n");
    fprintf(fp, "@PJL SET IWAMANUALDUP=OFF\n");
    fprintf(fp, "@PJL SET IWAJAMRECOVERY=AUTO\n");
    fprintf(fp, "@PJL ENTER LANGUAGE=HBPL\n");
}

/* ------------------------------------------------------------------ */
/* Pages                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    obuf_t data;		/* C..D records, page number patched later */
    size_t pageno_at;		/* offset of the 4-byte page number */
    char info[96];		/* FXJOBINFO PAGEINFO value */
} page_t;

static int build_page(page_t *pg, const job_t *job, cups_page_header2_t *hdr,
		      const unsigned char *raster, const axis_t *axis)
{
    int cw = (int)hdr->cupsWidth, ch = (int)hdr->cupsHeight;
    int w = (cw + 7) & ~7, res = (int)hdr->HWResolution[0];
    int pw = (int)hdr->PageSize[0], ph = (int)hdr->PageSize[1];
    int lef = pw > ph, code = paper_code(pw, ph), bpp = (int)hdr->cupsBitsPerPixel / 8;
    int gray_in = hdr->cupsNumColors == 1 || bpp == 1;
    const unsigned char *lut = job->color ? job->doc->lut : lut_mono;
    int raw = getenv("DELL1320C_TEST_RAW") != NULL;	/* tests: no colour tables */
    uint32_t white = job->color ? 0xffffffffu : 0xffu, *img;
    unsigned last_rgb = 0xffffffffu;
    uint32_t last_px = white;
    int x, y, blank = 1, out[3];
    long n;
    obuf_t *o = &pg->data;

    if (code < 0) {
	fprintf(stderr, "ERROR: Unsupported page size %dx%d points\n", pw, ph);
	return -1;
    }
    if (!(img = malloc(sizeof *img * (size_t)w * ch))) {
	fputs("ERROR: out of memory\n", stderr);
	exit(1);
    }

    for (y = 0; y < ch; y++) {
	const unsigned char *row = raster + (size_t)y * hdr->cupsBytesPerLine;
	uint32_t *dst = img + (size_t)y * w;

	for (x = 0; x < cw; x++) {
	    unsigned r, g, b, key;
	    uint32_t v;

	    if (gray_in)
		r = g = b = row[x];
	    else {
		r = row[x * bpp];
		g = row[x * bpp + 1];
		b = row[x * bpp + 2];
	    }
	    key = r << 16 | g << 8 | b;
	    if (key == last_rgb)
		v = last_px;
	    else {
		if (key == 0xffffff)
		    v = white;
		else if (raw)
		    v = job->color ? 0xffu | r << 8 | g << 16 | b << 24 : r;
		else if (!job->color) {
		    lut_lookup(axis, lut, 1, (int)r, (int)g, (int)b, out);
		    v = (uint32_t)out[0];
		} else if (lut) {
		    lut_lookup(axis, lut, 3, (int)r, (int)g, (int)b, out);
		    v = 0xffu | (uint32_t)out[0] << 8 | (uint32_t)out[1] << 16 | (uint32_t)out[2] << 24;
		} else
		    v = 0xffu | r << 8 | g << 16 | b << 24;
		last_rgb = key;
		last_px = v;
	    }
	    if (v != white)
		blank = 0;
	    dst[x] = v;
	}
	for (; x < w; x++)
	    dst[x] = white;
    }

    if (blank && job->skip_blank) {
	free(img);
	return 1;
    }

    if (job->turn) {	/* rotate the printable area by 180 degrees */
	for (y = 0; y < (ch + 1) / 2; y++) {
	    uint32_t *a = img + (size_t)y * w, *b = img + (size_t)(ch - 1 - y) * w;
	    for (x = 0; x < cw; x++) {
		uint32_t *pa = a + x, *pb = b + (cw - 1 - x);
		if (pa < pb) {
		    uint32_t t = *pa;
		    *pa = *pb;
		    *pb = t;
		}
	    }
	}
    }

    /* page header record */
    ob_byte(o, 'C');
    ob_byte(o, 0x91); ob_byte(o, 0xa1); ob_byte(o, lef);
    ob_byte(o, 0x92); ob_byte(o, 0xa1);
    ob_byte(o, job->tray == TRAY_AUTO ? 0 : job->tray == TRAY_BYPASS ? 1 : 2);
    ob_byte(o, 0x93); ob_byte(o, 0xa1); ob_byte(o, 1);
    ob_byte(o, 0x94); ob_byte(o, 0xa1); ob_byte(o, code);
    ob_byte(o, 0x95); ob_byte(o, 0xc2); ob_le(o, 0, 2); ob_le(o, 0, 2);
    ob_byte(o, 0x96); ob_byte(o, 0xa1); ob_byte(o, 3);
    n = (res * 94 + 300) / 600;		/* 11.28 pt printable-area offset */
    ob_byte(o, 0x97); ob_byte(o, 0xc3); ob_le(o, (unsigned long)n, 2); ob_le(o, (unsigned long)n, 2);
    ob_byte(o, 0x98); ob_byte(o, 0xa1); ob_byte(o, 0);
    ob_byte(o, 0x99); ob_byte(o, 0xa4);
    pg->pageno_at = o->len;
    ob_le(o, 0, 4);
    ob_byte(o, 0x9a); ob_byte(o, 0xc4); ob_le(o, (unsigned long)w, 4); ob_le(o, (unsigned long)ch, 4);
    ob_byte(o, 0x9b); ob_byte(o, 0xa1); ob_byte(o, 0);
    ob_byte(o, 0x9c); ob_byte(o, 0xa1); ob_byte(o, 1);
    ob_byte(o, 0x9d); ob_byte(o, 0xa1); ob_byte(o, job->color ? 0x16 : 0x11);
    ob_byte(o, 0x9e); ob_byte(o, 0xa1); ob_byte(o, 0);
    ob_byte(o, 0x9f); ob_byte(o, 0xa1); ob_byte(o, 1);
    ob_byte(o, 0xa0); ob_byte(o, 0xa1); ob_byte(o, job->color ? 0x20 : 0x08);
    ob_byte(o, 0xa1); ob_byte(o, 0xa1); ob_byte(o, res == 300 ? 3 : 0);
    ob_byte(o, 0xa2); ob_byte(o, 0xc4); ob_le(o, (unsigned long)w, 4); ob_le(o, (unsigned long)ch, 4);
    ob_byte(o, 'Q');

    /* raster record: 24-byte image header + LRLE2 bitstream */
    {
	static const unsigned char head[24] = {
	    0x18, 0, 0, 0, 0, 1, 0, 0, 0x10, 0x32, 0x04, 0, 0xa1, 0x42, 0, 0,
	    0, 0, 0xff, 0, 0, 0, 0, 0
	};
	obuf_t img_data = { 0 };
	ob_put(&img_data, head, sizeof head);
	if (!job->color)
	    img_data.buf[4] = 0x08;
	encode_page(&img_data, img, w, ch, job->color);
	ob_byte(o, 'R');
	ob_byte(o, 0xa3); ob_byte(o, 0xa1); ob_byte(o, 0);
	ob_byte(o, 0xa4); ob_byte(o, 0xb1); ob_byte(o, 0xa4);
	ob_le(o, (unsigned long)img_data.len, 4);
	ob_put(o, img_data.buf, img_data.len);
	free(img_data.buf);
    }
    ob_byte(o, 'S');
    ob_byte(o, 'D');

    {
	double mw = (lef ? ph : pw) * 25.4 / 72, mh = (lef ? pw : ph) * 25.4 / 72;
	int intray = job->tray == TRAY_AUTO ? 0 : job->tray == TRAY_BYPASS ? 5 : 1;
	if (job->color)
	    snprintf(pg->info, sizeof pg->info, "SIZE[%.1fX%.1f]:MODE[COLOR]:COLORS[0]:INTRAY[%d]", mw, mh, intray);
	else
	    snprintf(pg->info, sizeof pg->info, "SIZE[%.1fX%.1f]:MODE[MONOCHROME]:INTRAY[%d]", mw, mh, intray);
    }
    free(img);
    return 0;
}

static void emit_page(page_t *pg, unsigned long pageno)
{
    unsigned char *p = pg->data.buf + pg->pageno_at;
    p[0] = pageno & 0xff;
    p[1] = (pageno >> 8) & 0xff;
    p[2] = (pageno >> 16) & 0xff;
    p[3] = (pageno >> 24) & 0xff;
    fwrite(pg->data.buf, 1, pg->data.len, stdout);
}

int main(int argc, char **argv)
{
    static const unsigned char rec_a[] = { 'A', 0x81, 0xa1, 0x00, 0x82, 0xa2, 0x01, 0x00, 0x83, 0xa2, 0x01, 0x00 };
    job_t job;
    axis_t axis;
    cups_raster_t *ras;
    cups_page_header2_t hdr;
    page_t *pages = NULL;
    char **infos = NULL;
    int fd = 0, npages = 0, nkept = 0, started = 0, c;
    unsigned long emitted = 0;
    int i;

    if (argc < 6 || argc > 7) {
	fputs("Usage: rastertodell1320c job user title copies options [file]\n", stderr);
	return 1;
    }
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
	perror("ERROR: Unable to open raster file");
	return 1;
    }

    get_job_options(&job, argc, argv);
    init_axis(&axis);
    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);

    while (cupsRasterReadHeader2(ras, &hdr)) {
	unsigned char *raster;
	size_t size = (size_t)hdr.cupsBytesPerLine * hdr.cupsHeight;
	page_t pg;
	unsigned y;
	int rc;

	memset(&pg, 0, sizeof pg);
	npages++;
	if (hdr.cupsBitsPerColor != 8 || (hdr.cupsBitsPerPixel != 24 && hdr.cupsBitsPerPixel != 8)) {
	    fprintf(stderr, "ERROR: Unsupported raster format (%u bits per colour, %u per pixel)\n",
		    hdr.cupsBitsPerColor, hdr.cupsBitsPerPixel);
	    return 1;
	}
	if (hdr.HWResolution[0] != hdr.HWResolution[1] ||
	    (hdr.HWResolution[0] != 600 && hdr.HWResolution[0] != 300)) {
	    fprintf(stderr, "ERROR: Unsupported resolution %ux%u\n", hdr.HWResolution[0], hdr.HWResolution[1]);
	    return 1;
	}
	if (!(raster = malloc(size))) {
	    fputs("ERROR: out of memory\n", stderr);
	    return 1;
	}
	for (y = 0; y < hdr.cupsHeight; y++)
	    if (cupsRasterReadPixels(ras, raster + (size_t)y * hdr.cupsBytesPerLine,
				     hdr.cupsBytesPerLine) < 1)
		break;

	fprintf(stderr, "INFO: Printing page %d\n", npages);
	rc = build_page(&pg, &job, &hdr, raster, &axis);
	free(raster);
	if (rc < 0)
	    return 1;
	if (rc > 0) {
	    fprintf(stderr, "INFO: Skipping blank page %d\n", npages);
	    continue;
	}

	if (!started) {
	    write_pjl_header(stdout, &job, argv[2], argv[3],
			     hdr.PageSize[0] > hdr.PageSize[1], (int)hdr.HWResolution[0]);
	    fwrite(rec_a, 1, sizeof rec_a, stdout);
	    started = 1;
	}

	infos = realloc(infos, sizeof *infos * (size_t)(nkept + 1));
	infos[nkept] = strdup(pg.info);
	if (job.collate && job.copies > 1) {
	    /* collated copies: keep the encoded page, send the set N times */
	    pages = realloc(pages, sizeof *pages * (size_t)(nkept + 1));
	    pages[nkept] = pg;
	} else {
	    emit_page(&pg, ++emitted);
	    free(pg.data.buf);
	    fprintf(stderr, "PAGE: %d %d\n", nkept + 1, job.copies);
	}
	nkept++;
	fflush(stdout);
    }
    cupsRasterClose(ras);
    if (fd)
	close(fd);

    if (!started) {
	fputs(npages ? "INFO: All pages were blank, nothing to print\n"
		     : "ERROR: No pages found\n", stderr);
	return npages ? 0 : 1;
    }

    if (job.collate && job.copies > 1) {
	for (c = 0; c < job.copies; c++)
	    for (i = 0; i < nkept; i++) {
		emit_page(&pages[i], ++emitted);
		fprintf(stderr, "PAGE: %lu 1\n", emitted);
	    }
	for (i = 0; i < nkept; i++)
	    free(pages[i].data.buf);
	free(pages);
    }

    fputc('B', stdout);
    printf("\033%%-12345X@PJL COMMENT FXJOBINFO VERSION=1.0.0\n");
    printf("@PJL COMMENT FXJOBINFO BEGIN\n");
    printf("@PJL COMMENT FXJOBINFO PDLTYPE=PDL[PCLXL]:VERSION[1.0.0]\n");
    printf("@PJL COMMENT FXJOBINFO JOBTYPE=PRINT\n");
    printf("@PJL COMMENT FXJOBINFO JOBCOPIES=1\n");
    printf("@PJL COMMENT FXJOBINFO PAGECOPIES=%d\n", job.collate ? 1 : job.copies);
    printf("@PJL COMMENT FXJOBINFO DUPLEXTYPE=SIMPLEX\n");
    printf("@PJL COMMENT FXJOBINFO NUP=1\n");
    printf("@PJL COMMENT FXJOBINFO PAGEORDER=1TON\n");
    printf("@PJL COMMENT FXJOBINFO TONERSAVE=%s\n", job.toner_saver ? "ON" : "OFF");
    printf("@PJL COMMENT FXJOBINFO PHYSICALPAGES=%lu\n", emitted);
    printf("@PJL COMMENT FXJOBINFO PAGEINFOBEGIN\n");
    for (c = 0; c < (job.collate && job.copies > 1 ? job.copies : 1); c++)
	for (i = 0; i < nkept; i++)
	    printf("@PJL COMMENT FXJOBINFO PAGEINFO=%s\n", infos[i]);
    printf("@PJL COMMENT FXJOBINFO PAGEINFOEND\n");
    printf("@PJL COMMENT FXJOBINFO END\n");
    printf("@PJL EOJ\n");
    fflush(stdout);
    for (i = 0; i < nkept; i++)
	free(infos[i]);
    free(infos);
    return 0;
}
