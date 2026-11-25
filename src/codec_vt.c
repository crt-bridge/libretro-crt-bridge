/* SPDX-License-Identifier: GPL-3.0-or-later */
/* codec_vt.c -- H.264 decoding on macOS through VideoToolbox.
 *
 * System frameworks only (VideoToolbox, CoreMedia, CoreVideo, CoreFoundation): nothing third-party
 * sits beside a core loaded by RetroArch. One access unit in, Annex B as the encoder wrote
 * it; one picture out, as XRGB (0x00RRGGBB, like the client canvas). Synchronous: the call returns
 * when the picture is converted, which is what the client needs (0.5 to 0.8 ms on an M5, measured).
 *
 * Included by gmclient.c on __APPLE__ (and by the decoder's own test), never linked on its own.
 * HEVC is not here yet: gmc_vt_open refuses it. */
#ifndef GMC_CODEC_VT_C
#define GMC_CODEC_VT_C

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

typedef struct gmc_vt gmc_vt;

struct gmc_vt {
    CMVideoFormatDescriptionRef fmt;
    VTDecompressionSessionRef   sess;
    uint8_t *sps, *pps;
    size_t   sps_n, pps_n;
    uint8_t *avcc;                 /* scratch: the length-prefixed access unit */
    size_t   avcc_cap;
    /* filled by the output callback */
    uint32_t *out;
    size_t    out_cap_px;
    unsigned  w, h;
    int       got, bad_format;
    uint32_t  pix_fmt;             /* the format VideoToolbox handed back, for the log */
    /* measures of the last call, in microseconds */
    double    decode_us, convert_us, open_us;   /* open_us: format description and session of the last (re)build */
    unsigned long long pictures, errors;
};

static double gmc_vt_now_us(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000.0; }

static inline uint8_t gmc_vt_clamp(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* YCbCr -> 0x00RRGGBB, fixed point. matrix601 = 1 for BT.601 (what an untagged stream means), else BT.709.
 * full = 1 for full range, else video range (16..235 / 16..240). */
static inline uint32_t gmc_vt_px(int y, int cb, int cr, int matrix601, int full)
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
    return ((uint32_t)gmc_vt_clamp(r) << 16) | ((uint32_t)gmc_vt_clamp(g) << 8) | (uint32_t)gmc_vt_clamp(b);
}

static void gmc_vt_output(void *refcon, void *src, OSStatus status, VTDecodeInfoFlags flags,
                          CVImageBufferRef img, CMTime pts, CMTime dur)
{
    gmc_vt *d = (gmc_vt *)refcon;
    (void)src; (void)flags; (void)pts; (void)dur;
    if (status != noErr || !img) return;
    const unsigned w = (unsigned)CVPixelBufferGetWidth(img), h = (unsigned)CVPixelBufferGetHeight(img);
    const OSType f = CVPixelBufferGetPixelFormatType(img);
    int matrix601 = 1;
    CFTypeRef m = CVBufferCopyAttachment(img, kCVImageBufferYCbCrMatrixKey, NULL);
    if (m) {
        if (CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_709_2)) matrix601 = 0;
        CFRelease(m);
    }
    d->pix_fmt = f;
    if ((size_t)w * h > d->out_cap_px) { d->bad_format = 1; return; }
    if (CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) { d->bad_format = 1; return; }
    const double t0 = gmc_vt_now_us();
    if (f == kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange || f == kCVPixelFormatType_444YpCbCr8BiPlanarFullRange) {
        const int full = (f == kCVPixelFormatType_444YpCbCr8BiPlanarFullRange);
        const uint8_t *yp = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(img, 0);
        const uint8_t *cp = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(img, 1);
        const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(img, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *yr = yp + y * ys, *cr = cp + y * cs;
            uint32_t *o = d->out + (size_t)y * w;
            for (unsigned x = 0; x < w; x++) o[x] = gmc_vt_px(yr[x], cr[2 * x], cr[2 * x + 1], matrix601, full);
        }
        d->got = 1;
    } else if (f == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || f == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        const int full = (f == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
        const uint8_t *yp = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(img, 0);
        const uint8_t *cp = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(img, 1);
        const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(img, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *yr = yp + y * ys, *cr = cp + (y / 2) * cs;
            uint32_t *o = d->out + (size_t)y * w;
            for (unsigned x = 0; x < w; x++) o[x] = gmc_vt_px(yr[x], cr[(x / 2) * 2], cr[(x / 2) * 2 + 1], matrix601, full);
        }
        d->got = 1;
    } else if (f == kCVPixelFormatType_32BGRA) {
        const uint8_t *p = (const uint8_t *)CVPixelBufferGetBaseAddress(img);
        const size_t s = CVPixelBufferGetBytesPerRow(img);
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *r = p + y * s; uint32_t *o = d->out + (size_t)y * w;
            for (unsigned x = 0; x < w; x++) o[x] = ((uint32_t)r[4 * x + 2] << 16) | ((uint32_t)r[4 * x + 1] << 8) | r[4 * x];
        }
        d->got = 1;
    } else d->bad_format = 1;
    d->convert_us = gmc_vt_now_us() - t0;
    d->w = w; d->h = h;
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
}

void gmc_vt_close(gmc_vt *d)
{
    if (!d) return;
    if (d->sess) { VTDecompressionSessionInvalidate(d->sess); CFRelease(d->sess); }
    if (d->fmt) CFRelease(d->fmt);
    free(d->sps); free(d->pps); free(d->avcc);
    free(d);
}

/* codec: 0x10 H.264. HEVC is refused for now. */
gmc_vt *gmc_vt_open(int codec)
{
    if (codec != 0x10) return NULL;
    return (gmc_vt *)calloc(1u, sizeof(gmc_vt));
}

static int gmc_vt_same(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    return a && an == bn && memcmp(a, b, an) == 0;
}

/* (Re)builds the format description and the session when the parameter sets are new or changed. */
static int gmc_vt_prepare(gmc_vt *d, const uint8_t *sps, size_t sps_n, const uint8_t *pps, size_t pps_n)
{
    if (d->sess && gmc_vt_same(d->sps, d->sps_n, sps, sps_n) && gmc_vt_same(d->pps, d->pps_n, pps, pps_n)) return 0;
    const double t_open = gmc_vt_now_us();
    if (d->sess) { VTDecompressionSessionInvalidate(d->sess); CFRelease(d->sess); d->sess = NULL; }
    if (d->fmt) { CFRelease(d->fmt); d->fmt = NULL; }
    free(d->sps); free(d->pps);
    d->sps = (uint8_t *)malloc(sps_n); d->pps = (uint8_t *)malloc(pps_n);
    if (!d->sps || !d->pps) return -1;
    memcpy(d->sps, sps, sps_n); memcpy(d->pps, pps, pps_n);
    d->sps_n = sps_n; d->pps_n = pps_n;

    const uint8_t *ps[2] = { d->sps, d->pps };
    const size_t sz[2] = { sps_n, pps_n };
    if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, ps, sz, 4, &d->fmt) != noErr) return -1;

    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
    VTDecompressionOutputCallbackRecord cb = { gmc_vt_output, d };
    const OSStatus st = VTDecompressionSessionCreate(kCFAllocatorDefault, d->fmt, spec, NULL, &cb, &d->sess);
    CFRelease(spec);
    if (st != noErr) return -1;
    VTSessionSetProperty(d->sess, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
    d->open_us = gmc_vt_now_us() - t_open;
    return 0;
}

/* One Annex B access unit in. Returns 1 when a picture came out (*w, *h set, d->out filled), 0 when
 * the unit holds no picture (parameter sets alone) or cannot be decoded yet (no parameter sets seen),
 * -1 on a decoder error. `xrgb` must hold cap_px pixels. */
int gmc_vt_decode(gmc_vt *d, const uint8_t *au, size_t len, uint32_t *xrgb, size_t cap_px, unsigned *w, unsigned *h)
{
    size_t i = 0, vcl_bytes = 0, nvcl = 0;
    struct { size_t off, n; } nal[64];
    size_t nn = 0;
    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_n = 0, pps_n = 0, o = 0;

    if (!d || !au || !xrgb) return -1;
    /* split the unit into NAL units */
    while (i + 3 <= len && nn < 64) {
        size_t s;
        while (i + 3 <= len && !(au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)) i++;
        if (i + 3 > len) break;
        s = i + 3;
        i = s;
        while (i + 3 <= len && !(au[i] == 0 && au[i + 1] == 0 && (au[i + 2] == 1 || (au[i + 2] == 0 && i + 3 < len && au[i + 3] == 1)))) i++;
        if (i + 3 > len) i = len;
        {
            size_t e = i;
            while (e > s && au[e - 1] == 0) e--;      /* trailing zero bytes belong to the next start code */
            nal[nn].off = s; nal[nn].n = e - s; nn++;
        }
    }
    for (i = 0; i < nn; i++) {
        const unsigned t = au[nal[i].off] & 0x1Fu;
        if (t == 7u) { sps = au + nal[i].off; sps_n = nal[i].n; }
        else if (t == 8u) { pps = au + nal[i].off; pps_n = nal[i].n; }
        else if (t == 1u || t == 5u) { vcl_bytes += 4u + nal[i].n; nvcl++; }
    }
    if (sps && pps && gmc_vt_prepare(d, sps, sps_n, pps, pps_n) != 0) { d->errors++; return -1; }
    if (nvcl == 0u) return 0;                       /* parameter sets or SEI alone */
    if (!d->sess) return 0;                         /* no keyframe seen yet: nothing to decode with */

    if (vcl_bytes > d->avcc_cap) {
        uint8_t *g = (uint8_t *)realloc(d->avcc, vcl_bytes);
        if (!g) return -1;
        d->avcc = g; d->avcc_cap = vcl_bytes;
    }
    for (i = 0; i < nn; i++) {
        const unsigned t = au[nal[i].off] & 0x1Fu;
        if (t != 1u && t != 5u) continue;
        d->avcc[o] = (uint8_t)(nal[i].n >> 24); d->avcc[o + 1] = (uint8_t)(nal[i].n >> 16);
        d->avcc[o + 2] = (uint8_t)(nal[i].n >> 8); d->avcc[o + 3] = (uint8_t)nal[i].n;
        memcpy(d->avcc + o + 4, au + nal[i].off, nal[i].n);
        o += 4u + nal[i].n;
    }

    d->out = xrgb; d->out_cap_px = cap_px; d->got = 0; d->bad_format = 0;
    {
        CMBlockBufferRef bb = NULL;
        CMSampleBufferRef sb = NULL;
        const size_t sample_size = o;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, d->avcc, o, kCFAllocatorNull, NULL, 0, o, 0, &bb);
        if (st == noErr)
            st = CMSampleBufferCreateReady(kCFAllocatorDefault, bb, d->fmt, 1, 0, NULL, 1, &sample_size, &sb);
        if (st != noErr) { if (bb) CFRelease(bb); d->errors++; return -1; }
        {
            VTDecodeInfoFlags fl = 0;
            const double t0 = gmc_vt_now_us();
            st = VTDecompressionSessionDecodeFrame(d->sess, sb, 0, NULL, &fl);
            if (st == noErr) VTDecompressionSessionWaitForAsynchronousFrames(d->sess);
            d->decode_us = gmc_vt_now_us() - t0 - d->convert_us;   /* the conversion happened inside the call */
        }
        CFRelease(sb); CFRelease(bb);
        if (st != noErr) { d->errors++; return -1; }
    }
    if (d->bad_format || !d->got) { d->errors++; return d->bad_format ? -1 : 0; }
    d->pictures++;
    if (w) *w = d->w;
    if (h) *h = d->h;
    return 1;
}

#endif /* GMC_CODEC_VT_C */
