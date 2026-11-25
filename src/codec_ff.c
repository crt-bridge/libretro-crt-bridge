/* SPDX-License-Identifier: GPL-3.0-or-later */
/* codec_ff.c -- H.264 decoding through a libavcodec loaded at run time (Windows and Linux).
 *
 * Nothing of FFmpeg is linked or included: no header, no -lavcodec. The library is opened by name
 * when the first decoder is asked for, the dozen functions used are looked up one by one, and the few
 * prototypes below are written by hand. A machine without FFmpeg simply reports "not available"
 * (gmc_ff_available() = 0) and the client keeps its other paths. The client therefore ships without
 * any third-party binary beside it and without FFmpeg's licence in its own.
 *
 * Search order: the directory in GMC_FFMPEG_DIR (made absolute; on Windows its dependencies are
 * looked up in that directory and System32 only, never PATH or the current directory), then the
 * system. Only the majors listed in gmc_ff_ok[] are accepted: that
 * list is the set that was actually tested, and a major that is not in it is refused even if the file
 * loads. Today: libavcodec 61 with libavutil 59 (FFmpeg 7.1), and libavcodec 63 with libavutil 61.
 *
 * Same shape as codec_vt.c: gmc_ff_open(codec) / gmc_ff_decode(...) / gmc_ff_close(). One Annex B
 * access unit in, one picture out as XRGB (0x00RRGGBB), with the colour rules of gmc_vt_px: BT.601 and
 * limited range unless the stream says otherwise. Synchronous, one thread, no output delay.
 *
 * Included by gmclient.c under -DGMC_HAVE_FF (and by its own test), never linked on its own. */
#ifndef GMC_CODEC_FF_C
#define GMC_CODEC_FF_C

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>

#ifdef _WIN32
#  include <windows.h>
#  ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#    define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#  endif
#  ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#    define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#  endif
#else
#  include <dlfcn.h>
#endif

#ifndef GMC_FF_MAX_W
#define GMC_FF_MAX_W 2560u   /* the widest and tallest picture the client holds (gmclient.c: GMC_MAX_W / GMC_MAX_H) */
#define GMC_FF_MAX_H 1024u
#endif
#define GMC_FF_PAD       64  /* AV_INPUT_BUFFER_PADDING_SIZE of FFmpeg 7 */
#define GMC_FF_EAGAIN    (-EAGAIN)   /* AVERROR(EAGAIN): the platform's own EAGAIN (11 on Linux and Windows, 35 on macOS) */
#define GMC_FF_OPT_CHILDREN 1

typedef struct gmc_ff gmc_ff;

/* The front of an AVFrame (FFmpeg 7.x), and only the front: data, linesize, width, height, format.
 * The frame is allocated and freed by FFmpeg; we never take its size. */
typedef struct {
    uint8_t *data[8];
    int      linesize[8];
    uint8_t **extended_data;
    int      width, height;
    int      nb_samples;
    int      format;
} gmc_ff_frame;
#if UINTPTR_MAX > 0xffffffffu
_Static_assert(offsetof(gmc_ff_frame, width) == 104 && offsetof(gmc_ff_frame, format) == 116,
               "AVFrame prefix layout (FFmpeg 7.x, 64-bit)");
#endif

/* The libraries accepted: libavcodec major, libavutil major. 61/59 = FFmpeg 7.1 (Windows DLLs, Debian 13);
 * 63/61 = FFmpeg 8 (Homebrew). Each pair listed here was played against the reference hashes. */
static const struct { int codec, util; } gmc_ff_ok[] = { { 63, 61 }, { 61, 59 } };

static struct {
    void *hcodec, *hutil;
    unsigned (*avcodec_version)(void);
    unsigned (*avutil_version)(void);
    const void *(*find_decoder_by_name)(const char *);
    void *(*alloc_context3)(const void *);
    void (*free_context)(void **);
    int  (*open2)(void *, const void *, void **);
    int  (*send_packet)(void *, const void *);
    int  (*receive_frame)(void *, void *);
    void *(*packet_alloc)(void);
    void (*packet_free)(void **);
    int  (*packet_from_data)(void *, uint8_t *, int);
    void *(*av_malloc)(size_t);
    void (*av_free)(void *);
    void *(*frame_alloc)(void);
    void (*frame_free)(void **);
    void (*frame_unref)(void *);
    void (*log_set_level)(int);
    int  (*opt_set)(void *, const char *, const char *, int);
    int  (*opt_get_int)(void *, const char *, int, int64_t *);
    int  (*get_pix_fmt)(const char *);
    int  fmt_444, fmt_420, fmt_j444, fmt_j420;   /* AVPixelFormat values, looked up by name */
} gmc_ff_lib;

static int  gmc_ff_state;          /* 0 = not probed, 1 = loaded, -1 = failed (cached) */
static char gmc_ff_err[192];

/* Why the last probe failed ("" when it did not). For the log and for the tests. */
const char *gmc_ff_error(void) { return gmc_ff_err; }

static void gmc_ff_fail(const char *what, const char *detail)
{
    snprintf(gmc_ff_err, sizeof gmc_ff_err, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
}

#ifdef _WIN32
static void *gmc_ff_dlopen(const wchar_t *dir, const char *name)
{
    wchar_t wname[96], joined[1024], full[1024];
    size_t i;
    for (i = 0; name[i] && i < 95; i++) wname[i] = (wchar_t)(unsigned char)name[i];
    wname[i] = 0;
    if (!dir) return (void *)LoadLibraryExW(wname, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);   /* System32 only */
    if (_snwprintf(joined, 1024, L"%ls\\%ls", dir, wname) < 0) return NULL;
    if (!GetFullPathNameW(joined, 1024, full, NULL)) return NULL;                          /* absolute, always */
    /* its dependencies resolve beside it, then in System32 -- never PATH or the current directory */
    return (void *)LoadLibraryExW(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}
static void *gmc_ff_dlsym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
static void  gmc_ff_dlclose(void *h) { if (h) FreeLibrary((HMODULE)h); }
#define GMC_FF_NAME_CODEC "avcodec-%d.dll"
#define GMC_FF_NAME_UTIL  "avutil-%d.dll"
#else
static void *gmc_ff_dlopen(const char *dir, const char *name)
{
    char path[1024], real[4096];
    if (!dir) return dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return NULL;
    if (!realpath(path, real)) return NULL;                                                  /* absolute, always */
    return dlopen(real, RTLD_NOW | RTLD_LOCAL);
}
static void *gmc_ff_dlsym(void *h, const char *n) { return dlsym(h, n); }
static void  gmc_ff_dlclose(void *h) { if (h) dlclose(h); }
#  ifdef __APPLE__
#    define GMC_FF_NAME_CODEC "libavcodec.%d.dylib"
#    define GMC_FF_NAME_UTIL  "libavutil.%d.dylib"
#  else
#    define GMC_FF_NAME_CODEC "libavcodec.so.%d"
#    define GMC_FF_NAME_UTIL  "libavutil.so.%d"
#  endif
#endif

#define GMC_FF_SYM(h, field, sym) do { \
        *(void **)&gmc_ff_lib.field = gmc_ff_dlsym((h), sym); \
        if (!gmc_ff_lib.field) { gmc_ff_fail("missing symbol", sym); return -1; } \
    } while (0)

/* The two libraries are open: check their majors, then look every function up. 0 on success. */
static int gmc_ff_bind(int codec_major, int util_major)
{
    void *hc = gmc_ff_lib.hcodec, *hu = gmc_ff_lib.hutil;
    char detail[64];
    GMC_FF_SYM(hc, avcodec_version, "avcodec_version");
    GMC_FF_SYM(hu, avutil_version, "avutil_version");
    if ((int)(gmc_ff_lib.avcodec_version() >> 16) != codec_major || (int)(gmc_ff_lib.avutil_version() >> 16) != util_major) {
        snprintf(detail, sizeof detail, "avcodec %u, avutil %u", gmc_ff_lib.avcodec_version() >> 16, gmc_ff_lib.avutil_version() >> 16);
        gmc_ff_fail("major version not in the accepted list", detail);
        return -1;
    }
    GMC_FF_SYM(hc, find_decoder_by_name, "avcodec_find_decoder_by_name");
    GMC_FF_SYM(hc, alloc_context3, "avcodec_alloc_context3");
    GMC_FF_SYM(hc, free_context, "avcodec_free_context");
    GMC_FF_SYM(hc, open2, "avcodec_open2");
    GMC_FF_SYM(hc, send_packet, "avcodec_send_packet");
    GMC_FF_SYM(hc, receive_frame, "avcodec_receive_frame");
    GMC_FF_SYM(hc, packet_alloc, "av_packet_alloc");
    GMC_FF_SYM(hc, packet_free, "av_packet_free");
    GMC_FF_SYM(hc, packet_from_data, "av_packet_from_data");
    GMC_FF_SYM(hu, av_malloc, "av_malloc");
    GMC_FF_SYM(hu, av_free, "av_free");
    GMC_FF_SYM(hu, frame_alloc, "av_frame_alloc");
    GMC_FF_SYM(hu, frame_free, "av_frame_free");
    GMC_FF_SYM(hu, frame_unref, "av_frame_unref");
    GMC_FF_SYM(hu, log_set_level, "av_log_set_level");
    GMC_FF_SYM(hu, opt_set, "av_opt_set");
    GMC_FF_SYM(hu, opt_get_int, "av_opt_get_int");
    GMC_FF_SYM(hu, get_pix_fmt, "av_get_pix_fmt");
    gmc_ff_lib.log_set_level(-8);   /* AV_LOG_QUIET: a damaged unit is reported by the return value, not by a line per slice on stderr */
    gmc_ff_lib.fmt_444 = gmc_ff_lib.get_pix_fmt("yuv444p");
    gmc_ff_lib.fmt_420 = gmc_ff_lib.get_pix_fmt("yuv420p");
    gmc_ff_lib.fmt_j444 = gmc_ff_lib.get_pix_fmt("yuvj444p");
    gmc_ff_lib.fmt_j420 = gmc_ff_lib.get_pix_fmt("yuvj420p");
    if (gmc_ff_lib.fmt_444 < 0 || gmc_ff_lib.fmt_420 < 0 || gmc_ff_lib.fmt_j444 < 0 || gmc_ff_lib.fmt_j420 < 0) {
        gmc_ff_fail("pixel format unknown to the library", "");
        return -1;
    }
    return 0;
}

/* Tries one source (a directory, or NULL for the system) with every listed major. */
static int gmc_ff_try_source(const void *dir)
{
    size_t k;
    for (k = 0; k < sizeof gmc_ff_ok / sizeof gmc_ff_ok[0]; k++) {
        char nc[64], nu[64];
        snprintf(nc, sizeof nc, GMC_FF_NAME_CODEC, gmc_ff_ok[k].codec);
        snprintf(nu, sizeof nu, GMC_FF_NAME_UTIL, gmc_ff_ok[k].util);
        memset(&gmc_ff_lib, 0, sizeof gmc_ff_lib);
        gmc_ff_lib.hutil = gmc_ff_dlopen(dir, nu);        /* libavutil first: libavcodec needs it */
        if (!gmc_ff_lib.hutil) { gmc_ff_fail("library not found", nu); continue; }
        gmc_ff_lib.hcodec = gmc_ff_dlopen(dir, nc);
        if (!gmc_ff_lib.hcodec) { gmc_ff_fail("library not found", nc); gmc_ff_dlclose(gmc_ff_lib.hutil); continue; }
        if (gmc_ff_bind(gmc_ff_ok[k].codec, gmc_ff_ok[k].util) == 0) return 1;
        gmc_ff_dlclose(gmc_ff_lib.hcodec); gmc_ff_dlclose(gmc_ff_lib.hutil);
    }
    memset(&gmc_ff_lib, 0, sizeof gmc_ff_lib);
    return 0;
}

/* Probes once and caches the answer. Called from the shell thread (gmc_config_defaults) before any
 * receive thread exists. */
static int gmc_ff_load(void)
{
    if (gmc_ff_state) return gmc_ff_state > 0;
    gmc_ff_err[0] = 0;
#ifdef _WIN32
    {
        const wchar_t *dir = _wgetenv(L"GMC_FFMPEG_DIR");
        if (dir && *dir && gmc_ff_try_source(dir)) { gmc_ff_state = 1; gmc_ff_err[0] = 0; return 1; }
    }
#else
    {
        const char *dir = getenv("GMC_FFMPEG_DIR");
        if (dir && *dir && gmc_ff_try_source(dir)) { gmc_ff_state = 1; gmc_ff_err[0] = 0; return 1; }
    }
#endif
    {
        char keep[sizeof gmc_ff_err];
        memcpy(keep, gmc_ff_err, sizeof keep);       /* "" unless GMC_FFMPEG_DIR was set and failed */
        if (gmc_ff_try_source(NULL)) { gmc_ff_state = 1; gmc_ff_err[0] = 0; return 1; }
#ifdef __APPLE__
        {   /* dlopen's default path misses Homebrew: Apple silicon, then Intel */
            static const char *const brew[2] = { "/opt/homebrew/lib", "/usr/local/lib" };
            unsigned b;
            for (b = 0; b < 2; b++)
                if (gmc_ff_try_source(brew[b])) { gmc_ff_state = 1; gmc_ff_err[0] = 0; return 1; }
        }
#endif
        if (keep[0]) memcpy(gmc_ff_err, keep, sizeof keep);   /* the explicit directory's reason is the useful one */
    }
    gmc_ff_state = -1;
    return 0;
}

/* 1 when libavcodec could be loaded and bound (probed once, then cached). */
int gmc_ff_available(void) { return gmc_ff_load(); }

#ifdef GMC_FF_TEST
/* Test only: forget the probe and unload, so one process can try several environments. */
void gmc_ff_unload(void)
{
    gmc_ff_dlclose(gmc_ff_lib.hcodec); gmc_ff_dlclose(gmc_ff_lib.hutil);
    memset(&gmc_ff_lib, 0, sizeof gmc_ff_lib);
    gmc_ff_state = 0; gmc_ff_err[0] = 0;
}
/* Test only: sees every decoded picture's planes before the colour conversion. */
static void (*gmc_ff_tap)(const uint8_t *const data[3], const int linesize[3], unsigned w, unsigned h, int c420);
#endif

/* --- Colour ---------------------------------------------------------------------- */
static inline uint8_t gmc_ff_clamp(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* YCbCr -> 0x00RRGGBB, fixed point. matrix601 = 1 for BT.601 (what an untagged stream means), else BT.709.
 * full = 1 for full range, else video range (16..235 / 16..240). */
static inline uint32_t gmc_ff_px(int y, int cb, int cr, int matrix601, int full)
{
    int r, g, b;
    cb -= 128; cr -= 128;
    if (matrix601) {
        if (full) { r = y + ((359 * cr + 128) >> 8); g = y - ((88 * cb + 183 * cr + 128) >> 8); b = y + ((454 * cb + 128) >> 8); }
        else { const int yy = 298 * (y - 16);
               r = (yy + 409 * cr + 128) >> 8; g = (yy - 100 * cb - 208 * cr + 128) >> 8; b = (yy + 516 * cb + 128) >> 8; }
    } else {
        if (full) { r = y + ((403 * cr + 128) >> 8); g = y - ((48 * cb + 120 * cr + 128) >> 8); b = y + ((475 * cb + 128) >> 8); }
        else { const int yy = 298 * (y - 16);
               r = (yy + 459 * cr + 128) >> 8; g = (yy - 55 * cb - 136 * cr + 128) >> 8; b = (yy + 541 * cb + 128) >> 8; }
    }
    return ((uint32_t)gmc_ff_clamp(r) << 16) | ((uint32_t)gmc_ff_clamp(g) << 8) | (uint32_t)gmc_ff_clamp(b);
}

/* Three planes (Y, U, V) to XRGB. 4:4:4 reads chroma at (x, y), 4:2:0 at (x/2, y/2). */
static void gmc_ff_convert(const uint8_t *yp, ptrdiff_t ys, const uint8_t *up, ptrdiff_t us, const uint8_t *vp, ptrdiff_t vs,
                           unsigned w, unsigned h, int c420, int matrix601, int full, uint32_t *out)
{
    unsigned x, y;
    for (y = 0; y < h; y++) {
        const uint8_t *yr = yp + (ptrdiff_t)y * ys;
        const uint8_t *ur = up + (ptrdiff_t)(c420 ? y / 2 : y) * us;
        const uint8_t *vr = vp + (ptrdiff_t)(c420 ? y / 2 : y) * vs;
        uint32_t *o = out + (size_t)y * w;
        if (c420) for (x = 0; x < w; x++) o[x] = gmc_ff_px(yr[x], ur[x / 2], vr[x / 2], matrix601, full);
        else      for (x = 0; x < w; x++) o[x] = gmc_ff_px(yr[x], ur[x], vr[x], matrix601, full);
    }
}

/* --- Decoder --------------------------------------------------------------------- */
struct gmc_ff {
    void *ctx;            /* AVCodecContext * */
    void *frame;          /* AVFrame *, reused */
    int   ps_seen;        /* an access unit carried both parameter sets */
    unsigned long long pictures, errors;
};

void gmc_ff_close(gmc_ff *d)
{
    if (!d) return;
    if (d->frame) gmc_ff_lib.frame_free(&d->frame);
    if (d->ctx) gmc_ff_lib.free_context(&d->ctx);
    free(d);
}

/* codec: 0x10 H.264. HEVC is refused for now (no tested stream). NULL when the library is absent. */
gmc_ff *gmc_ff_open(int codec)
{
    gmc_ff *d;
    const void *dec;
    if (codec != 0x10) return NULL;
    if (!gmc_ff_load()) return NULL;
    d = (gmc_ff *)calloc(1u, sizeof *d);
    if (!d) return NULL;
    dec = gmc_ff_lib.find_decoder_by_name("h264");
    if (!dec) { free(d); return NULL; }
    d->ctx = gmc_ff_lib.alloc_context3(dec);
    d->frame = gmc_ff_lib.frame_alloc();
    if (!d->ctx || !d->frame
        || gmc_ff_lib.opt_set(d->ctx, "threads", "1", GMC_FF_OPT_CHILDREN) < 0      /* one thread: no output delay */
        || gmc_ff_lib.opt_set(d->ctx, "flags", "+low_delay", GMC_FF_OPT_CHILDREN) < 0
        || gmc_ff_lib.opt_set(d->ctx, "err_detect", "+explode", GMC_FF_OPT_CHILDREN) < 0 /* a damaged unit fails (-1) instead of showing a concealed picture */
        || gmc_ff_lib.open2(d->ctx, dec, NULL) < 0) {
        gmc_ff_close(d);
        return NULL;
    }
    return d;
}

/* Turns the decoded picture into XRGB. 1 = done (*w, *h set), -1 = a picture this client does not take. */
static int gmc_ff_take(gmc_ff *d, const gmc_ff_frame *f, uint32_t *xrgb, size_t cap_px, unsigned *w, unsigned *h)
{
    int c420, full = 0, matrix601 = 1;
    int64_t v = 0;
    unsigned cw;
    if (f->width <= 0 || f->height <= 0 || (unsigned)f->width > GMC_FF_MAX_W || (unsigned)f->height > GMC_FF_MAX_H) return -1;
    if ((size_t)f->width * (size_t)f->height > cap_px) return -1;
    if (f->format == gmc_ff_lib.fmt_444) c420 = 0;
    else if (f->format == gmc_ff_lib.fmt_420) c420 = 1;
    else if (f->format == gmc_ff_lib.fmt_j444) { c420 = 0; full = 1; }
    else if (f->format == gmc_ff_lib.fmt_j420) { c420 = 1; full = 1; }
    else return -1;
    cw = c420 ? ((unsigned)f->width + 1u) / 2u : (unsigned)f->width;
    if (!f->data[0] || !f->data[1] || !f->data[2]) return -1;
    if (f->linesize[0] < f->width || f->linesize[1] < (int)cw || f->linesize[2] < (int)cw) return -1;
    /* What the stream says about itself, read from the context: AVCOL_RANGE_JPEG = 2, AVCOL_SPC_BT709 = 1. */
    if (gmc_ff_lib.opt_get_int(d->ctx, "color_range", 0, &v) >= 0 && v == 2) full = 1;
    if (gmc_ff_lib.opt_get_int(d->ctx, "colorspace", 0, &v) >= 0 && v == 1) matrix601 = 0;
#ifdef GMC_FF_TEST
    if (gmc_ff_tap) {
        const uint8_t *const pl[3] = { f->data[0], f->data[1], f->data[2] };
        const int ls[3] = { f->linesize[0], f->linesize[1], f->linesize[2] };
        gmc_ff_tap(pl, ls, (unsigned)f->width, (unsigned)f->height, c420);
    }
#endif
    gmc_ff_convert(f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2], f->linesize[2],
                   (unsigned)f->width, (unsigned)f->height, c420, matrix601, full, xrgb);
    *w = (unsigned)f->width; *h = (unsigned)f->height;
    return 1;
}

/* One Annex B access unit in. Returns 1 when a picture came out (*w, *h set, xrgb filled), 0 when the
 * unit holds no picture (parameter sets alone) or cannot be decoded yet (no parameter sets seen),
 * -1 on a decoder error or a picture that is out of bounds. `xrgb` must hold cap_px pixels. */
int gmc_ff_decode(gmc_ff *d, const uint8_t *au, size_t len, uint32_t *xrgb, size_t cap_px, unsigned *w, unsigned *h)
{
    int has_sps = 0, has_pps = 0, got = 0, bad = 0, r;
    size_t i;
    uint8_t *buf;
    void *pkt;

    if (!d || !au || !xrgb) return -1;
    if (len == 0) return 0;
    if (len > (size_t)INT_MAX - GMC_FF_PAD) return -1;
    for (i = 0; i + 3 < len; i++) {                      /* the NAL types present: 00 00 01 then the header byte */
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            const unsigned t = au[i + 3] & 0x1Fu;
            if (t == 7u) has_sps = 1; else if (t == 8u) has_pps = 1;
            i += 2;
        }
    }
    if (has_sps && has_pps) d->ps_seen = 1;
    if (!d->ps_seen) return 0;                           /* no keyframe seen yet: nothing to decode with */

    buf = (uint8_t *)gmc_ff_lib.av_malloc(len + GMC_FF_PAD);
    if (!buf) return -1;
    memcpy(buf, au, len);
    memset(buf + len, 0, GMC_FF_PAD);
    pkt = gmc_ff_lib.packet_alloc();
    if (!pkt) { gmc_ff_lib.av_free(buf); return -1; }
    if (gmc_ff_lib.packet_from_data(pkt, buf, (int)len) < 0) {   /* on success the packet owns buf */
        gmc_ff_lib.av_free(buf); gmc_ff_lib.packet_free(&pkt);
        d->errors++;
        return -1;
    }
    r = gmc_ff_lib.send_packet(d->ctx, pkt);
    gmc_ff_lib.packet_free(&pkt);
    if (r < 0) { d->errors++; return -1; }

    for (;;) {                                           /* normally one picture, never more than a few */
        r = gmc_ff_lib.receive_frame(d->ctx, d->frame);
        if (r == GMC_FF_EAGAIN) break;
        if (r < 0) { d->errors++; return -1; }
        if (gmc_ff_take(d, (const gmc_ff_frame *)d->frame, xrgb, cap_px, w, h) == 1) got = 1; else bad = 1;
        gmc_ff_lib.frame_unref(d->frame);
    }
    if (bad && !got) { d->errors++; return -1; }
    if (!got) return 0;
    d->pictures++;
    return 1;
}

#endif /* GMC_CODEC_FF_C */
