/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gmclient.c -- see gmclient.h.
 *
 * Wire truths, established by reading this project's own protocol library,
 * the hardware receiver's parser, and a bench capture tool (none published
 * here):
 *   - a command header travels ALONE in its own datagram; the payload follows
 *     in chunks of at most MTU bytes WITHOUT an opcode prefix;
 *   - CMD_INIT 4 or 5 bytes; CMD_SWITCHRES 26; CMD_AUDIO 3;
 *     CMD_BLIT_VSYNC 0x06 in 7 (GroovyMAME) or 11 (libgm, lz4_size = 0 when raw);
 *     CMD_BLIT_FIELD_VSYNC 0x07 in 8 (raw), 9 (frame_dup, [8]==0x01),
 *     12 (LZ4), 13 (LZ4 delta -- refused);
 *   - bytes expected per blit = h_active x (interlace ? v_active/2 : v_active) x 3,
 *     for 0x06 as for 0x07;
 *   - RGB888 in R, G, B order; field 0 = even lines, field 1 = odd lines;
 *   - 0x06 under interlace = one field per send, parity = frame_id & 1;
 *     0x07 from the fork = two fields per frame, same frame_id.
 */
#include "gmclient.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "lz4.h"

/* CMD_INIT sound_rate code -> sample rate in Hz; 0 for "no audio" or an unknown code. */
static unsigned groovy_audio_rate_hz(uint8_t code)
{
    switch (code) {
        case 1u: return 22050u;
        case 2u: return 44100u;
        case 3u: return 48000u;
        default: return 0u;
    }
}

/* --- Audio resampling -----------------------------------------------------------
 * gmclient always delivers 48000 Hz to the shell; this linear resampler covers
 * the three other CMD_INIT rates. Ratio stays fixed at 1.0 in this file -- no
 * caller here ever adjusts it -- but the field and its use in groovy_resample
 * are kept unchanged so the algorithm matches its source exactly. */
struct groovy_resampler {
    unsigned src_hz;
    unsigned dst_hz;
    double   phase;        /* read position, in source samples */
    int16_t  prev[2];      /* last pair of the previous block */
    int      prev_valid;
    double   ratio;   /* output pairs per input pair, on top of dst/src; 1.0 = neutral */
};

static void groovy_resample_reset(struct groovy_resampler *r, unsigned src_hz, unsigned dst_hz)
{
    if (!r) return;
    r->src_hz     = src_hz;
    r->dst_hz     = dst_hz;
    r->phase      = 0.0;
    r->prev[0]    = 0;
    r->prev[1]    = 0;
    r->prev_valid = 0;
    r->ratio      = 1.0;
}

/* Upper bound on the number of pairs produced for `in_frames` input pairs.
 * The +2 covers the carried-over phase and rounding. */
static size_t groovy_resample_capacity(size_t in_frames, unsigned src_hz, unsigned dst_hz)
{
    if (src_hz == 0u || dst_hz == 0u)
        return in_frames;
    return (size_t)((double)in_frames * (double)dst_hz / (double)src_hz) + 2u;
}

/* Integer floor of a possibly-negative double -- (long) truncates toward
 * zero, which is wrong for negatives, and the carried-over phase is negative. */
static long groovy_resample_floor(double v)
{
    long i = (long)v;
    return ((double)i > v) ? (i - 1) : i;
}

/* Produces the resampled stereo pairs from `in` into `out`. Returns the
 * number of pairs written. `out_cap` is in pairs. A ratio of 1 (src == dst)
 * copies through without touching the phase. */
static size_t groovy_resample(struct groovy_resampler *r, const int16_t *in, size_t in_frames,
                              int16_t *out, size_t out_cap)
{
    if (!r || !in || !out || in_frames == 0u || out_cap == 0u)
        return 0u;
    if (r->src_hz == 0u || r->dst_hz == 0u)
        return 0u;

    if (r->src_hz == r->dst_hz && r->ratio == 1.0) {
        size_t n = (in_frames < out_cap) ? in_frames : out_cap;
        memcpy(out, in, n * 2u * sizeof(int16_t));
        return n;
    }

    /* The carried-over phase sits between -1 and 0 in steady state, and
     * index -1 designates `prev`. Nothing in the loop can push it lower,
     * but an explicit bound costs one comparison and permanently forbids
     * reading before the start of the block. */
    if (r->phase < -1.0)
        r->phase = -1.0;

    /* r_eff: the fractional ratio, folded back to 1.0 if not positive
     * (numeric guard -- step can never become zero, negative or infinite). */
    const double r_eff = (r->ratio > 0.0) ? r->ratio : 1.0;
    const double step  = ((double)r->src_hz / (double)r->dst_hz) / r_eff;
    const double limit = (double)in_frames - 1.0;
    size_t       n     = 0u;

    /* Only emit outputs whose right-hand neighbor is WITHIN this block;
     * the rest is carried over to the next call via the phase. */
    while (r->phase < limit && n < out_cap) {
        long   i    = groovy_resample_floor(r->phase);
        double frac = r->phase - (double)i;

        const int16_t *a;
        if (i < 0)
            a = r->prev_valid ? r->prev : in;   /* very first block: no history */
        else
            a = in + (size_t)i * 2u;
        const int16_t *b = in + (size_t)(i + 1) * 2u;

        for (unsigned c = 0u; c < 2u; ++c) {
            double v = (double)a[c] + ((double)b[c] - (double)a[c]) * frac;
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            out[n * 2u + c] = (int16_t)v;
        }
        ++n;
        r->phase += step;
    }

    /* Carry-over: the phase becomes relative to the start of the NEXT
     * block, and `prev` keeps the last pair of this one. */
    r->prev[0]    = in[(in_frames - 1u) * 2u];
    r->prev[1]    = in[(in_frames - 1u) * 2u + 1u];
    r->prev_valid = 1;
    r->phase     -= (double)in_frames;

    return n;
}

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <process.h>
typedef SOCKET           gmc_sock;
typedef HANDLE           gmc_thread;
#  define GMC_BAD_SOCK    INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <pthread.h>
#  include <time.h>
#  include <sys/select.h>
typedef int              gmc_sock;
typedef pthread_t        gmc_thread;
#  define GMC_BAD_SOCK    (-1)
#endif

/* gmc_mutex and MTX_*: shared with gmclient_input.c.
 * Included AFTER the platform block above -- <windows.h> must come after
 * <winsock2.h>, never before. */
#include "gmc_mtx.h"
/* The input channel: its socket is now monitored by this file's receiving
 * thread. Only service functions are called here; the channel's state stays
 * private to gmclient_input.c. */
#include "gmclient_input.h"

/* --- Protocol (mirrors the hardware receiver's own parser, not published here) --- */
#define CMD_CLOSE             0x01
#define CMD_INIT              0x02
#define CMD_SWITCHRES         0x03
#define CMD_AUDIO             0x04
#define CMD_GET_STATUS        0x05
#define CMD_BLIT_VSYNC        0x06
#define CMD_BLIT_FIELD_VSYNC  0x07
#define CMD_GET_VERSION       0x08

#define FLAG_VRAM_READY      0x01
#define FLAG_VRAM_END_FRAME  0x02
#define FLAG_VRAM_SYNCED     0x04
#define FLAG_VGA_F1          0x20
#define FLAG_AUDIO           0x40

#define GMC_DGRAM_MAX     65536u
/* Trace only: how long rx_loop waits for the successor of a datagram that ends a
 * video payload, so as to stamp it BEFORE the decode. 200 us: twice the 100 us
 * gap threshold used by a bench latency check, so any gap that check could pass
 * is stamped truly; a later successor is stamped after the decode, well above
 * the threshold. */
#define GMC_TRACE_PEEK_NS 200000ull
#define GMC_MAX_W         1024u
#define GMC_MAX_H         1024u
#define GMC_MAX_SAMPLES   400000u

/* --- Audio -------------------------------------------------------------------------
 * GMC_AUDIO_PEND_CAP bounds the payload of a CMD_AUDIO block: beyond it,
 * the block is rejected rather than silently truncated in a memcpy.
 * GMC_AUDIO_RING_CAP is the PCM ring's capacity in stereo PAIRS (~170 ms at
 * 48000 Hz); beyond it, the oldest pairs are dropped and counted.
 * GMC_AUDIO_SCRATCH_CAP bounds the INPUT samples converted to stereo before
 * resampling (the worst case is mono: GMC_AUDIO_PEND_CAP / 2 bytes/sample).
 * GMC_AUDIO_RESAMP_CAP bounds the resampler's OUTPUT (a generous upper bound:
 * max input 4096 pairs, max ratio 48000/22050 ~ 2.18). */
#define GMC_AUDIO_PEND_CAP     8192u
#define GMC_AUDIO_RING_CAP     8192u
#define GMC_AUDIO_SCRATCH_CAP  4096u
#define GMC_AUDIO_RESAMP_CAP   10240u
#define GMC_AUDIO_PREROLL      1600u   /* two ticks at 48000 Hz / 60 Hz */

/* --- Clock --------------------------------------------------------------------- */
uint64_t gmc_now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* --- Samples for percentiles ---------------------------------------------------- */
typedef struct { double *v; size_t n, cap; } samples_t;

static void samples_push(samples_t *s, double x)
{
    if (s->n >= GMC_MAX_SAMPLES) return;
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        double *nv = (double *)realloc(s->v, nc * sizeof(double));
        if (!nv) return;
        s->v = nv; s->cap = nc;
    }
    s->v[s->n++] = x;
}
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}
static double pct(const samples_t *s, double q)   /* s sorted */
{
    if (s->n == 0) return 0.0;
    size_t i = (size_t)(q * (double)(s->n - 1) + 0.5);
    if (i >= s->n) i = s->n - 1;
    return s->v[i];
}
static void samples_json(FILE *out, const char *name, samples_t *s, double scale)
{
    if (s->n > 1) qsort(s->v, s->n, sizeof(double), cmp_double);
    fprintf(out, "\"%s\":{\"n\":%zu,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"min\":%.3f}",
            name, s->n, pct(s, 0.50) * scale, pct(s, 0.95) * scale, pct(s, 0.99) * scale,
            (s->n ? s->v[s->n - 1] : 0.0) * scale, (s->n ? s->v[0] : 0.0) * scale);
}

/* --- Internal structures --------------------------------------------------------- */
typedef struct {
    gmc_frame f;
    int       in_ring;     /* 1 = in the ready queue */
    int       held;        /* 1 = acquired by the shell */
} slot_t;

enum { PEND_NONE = 0, PEND_VIDEO, PEND_AUDIO };

struct gmc {
    gmc_config cfg;
    gmc_sock   sock;
    struct sockaddr_in last_src;
    int        have_src;
    volatile int stop;
    gmc_thread thread;
    gmc_mutex  mtx;

    /* session */
    int      session_open, compressed, audio_announced;
    int      padded;   /* set by the length of the received CMD_INIT */
    uint8_t  audio_rate_code, audio_channels;

    /* audio: PCM ring and wire -> fixed 48000 Hz resampling. Allocated by
     * audio_alloc(), including under GMCLIENT_TEST -- no network dependency here. */
    struct groovy_resampler ar;
    unsigned  audio_src_hz;              /* 0 = no audio announced */
    uint8_t  *apend_buf;                 /* CMD_AUDIO payload in progress, bounded to GMC_AUDIO_PEND_CAP */
    int16_t  *astereo;                   /* interleaved stereo samples, before resampling */
    int16_t  *aresamp;                   /* resampler output, before push into the ring */
    int16_t  *apcm;                      /* ring, interleaved stereo pairs */
    unsigned  acap;                      /* capacity in pairs */
    unsigned  ahead, acount;             /* read index and occupancy, in pairs */
    int       aprerolled;
    uint64_t  audio_frames_in, audio_frames_out, audio_underruns, audio_overruns;

    /* geometry */
    gmc_geom geom;          /* current (full height) */
    int      have_geom;
    uint32_t geom_seen_by_shell;
    uint32_t blit_w, blit_lines_full, blit_lines_field;

    /* persistent canvas (last frame, full height, XRGB) */
    uint32_t *canvas; size_t canvas_px;
    uint8_t  *scratch; size_t scratch_cap;     /* LZ4 output (clear) */
    uint8_t  *pend_buf; size_t pend_cap;       /* payload currently being received */

    /* reception in progress */
    int      pend_kind;
    size_t   pend_expected, pend_got;
    uint32_t pend_frame_id; uint16_t pend_vsync; uint8_t pend_field, pend_opcode;
    uint32_t pend_lz4; int pend_is_lz4, pend_is_delta;
    uint64_t pend_t_first;

    /* 480i pairing (0x07, two fields sharing the same frame_id) */
    int      pair_active; uint32_t pair_frame_id; uint8_t pair_fields; uint16_t pair_vsync;
    uint64_t pair_t_last_byte;

    /* 480i half-height (0x06, one field per send): bit0 = even field already written
     * into the canvas since the last geometry change, bit1 = odd field. Reset to 0 by
     * apply_switchres. Avoids a false field_swapped on the very first frame emitted
     * after a CMD_SWITCHRES: the canvas is calloc'd to zero and only one half has
     * received a real write, so the other half's marker is still black -- not a
     * recombination defect, just a canvas not yet warm. */
    uint8_t  half_seen;

    /* ring */
    slot_t  *slots; unsigned cap;
    unsigned *ready; unsigned ready_head, ready_count;   /* circular queue of indices */
    int      prerolled;
    uint64_t seq;

    /* status */
    uint32_t ack_frame;
    uint64_t last_nogeom_status_ns;   /* last send of the no-geometry knock, 0 at open */

    /* counters */
    uint64_t datagrams, bytes, inits, switchres, blit06, blit07, dups, deltas,
             frames_emitted, pairs_incomplete, lz4_fail, cont_escape, stray_payload,
             unknown_opcode, audio_blocks, audio_bytes, status_sent, get_status,
             closes, overlong, blit_empty, overflow_drops, skips, underruns, prerolls,
             repeats, presents, pattern_ok, pattern_mismatch, field_swapped, geom_changes,
             half_fields, status_nogeom;
    /* The same escapes, split by the kind of block that was cut (pend_kind at
     * the time of the escape); cont_escape stays their sum. */
    uint64_t cont_escape_audio, cont_escape_video;
    unsigned *depth_hist;   /* cap+1 */
    samples_t lat_last_byte, lat_ready, decode_us, present_interval, ring_wait;
    /* Instrumentation from a debugging session: contention on c->mtx and the size of
     * audio pulls. Measurement only -- neither changes behavior. */
    samples_t emit_lock_us;      /* time HELD under c->mtx by emit_frame */
    samples_t acquire_wait_us;   /* time WAITED to take c->mtx in gmc_acquire */
    samples_t audio_pull;        /* frames returned by gmc_audio_read, non-empty pull */
    uint64_t  last_present_ns;
    FILE     *evlog;
    /* Input channel, monitored by the receiving thread. Set once in gmc_open,
     * before the thread exists, and never modified afterward. */
    struct gmc_input *input;
    /* Per-datagram trace (NULL = off, the default) and the receive buffer's actual
     * size, read back via getsockopt after gmc_open's setsockopt -- macOS often
     * refuses 8 MB and keeps its own default. */
    FILE     *trace; unsigned trace_lines;
    int       rcvbuf_asked, rcvbuf_got;
    char      geom_log[1024]; size_t geom_log_len;
};

/* --- utilities --------------------------------------------------------------------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/*
 * zero_tail -- exact mirror of the hardware receiver's own check (not
 * published here): are the bytes [from, to) of `b` all zero? Used by the
 * padded 8-byte forms that require a zero-filled tail.
 *
 * Bounds are always LITERAL CONSTANTS at the call site, never a value taken
 * from the wire: called only AFTER an exact length check (`len == 8`) has
 * already verified that [from, to) lies inside the buffer.
 */
static int zero_tail(const uint8_t *b, size_t from, size_t to)
{
    size_t i;
    for (i = from; i < to; i++) {
        if (b[i] != 0u) return 0;
    }
    return 1;
}

/*
 * is_command_header -- mirror of the hardware receiver's own header check
 * (not published here); any divergence between the two is a defect. LENGTH
 * TABLE BY SESSION MODE -- shared authority between this file and that
 * parser, the two MUST NOT diverge:
 *
 *   opcode                     classic      padded      zero tail required (8-byte form)
 *   0x01 CMD_CLOSE              len==1       len==8      b[1..7]
 *   0x02 CMD_INIT                4, 5 or 8    4, 5 or 8   b[4..7] -- DOES NOT DEPEND ON THE MODE
 *   0x03 CMD_SWITCHRES           len==26      len==26     --
 *   0x04 CMD_AUDIO               len==3       len==8      b[3..7]
 *   0x05 CMD_GET_STATUS          len==1       len==8      b[1..7]
 *   0x06 CMD_BLIT_VSYNC          7 or 11      same        --
 *   0x07 CMD_BLIT_FIELD_VSYNC    8/9/12/13    same        --
 *   0x08 CMD_GET_VERSION         len==1       len==8      b[1..7]
 *
 * ACCEPTED EXCEPTION, NOT A PENDING WORKAROUND: CMD_INIT is recognized at 4,
 * 5 AND 8 bytes IN BOTH MODES, bytes 4 to 7 required zero for the 8-byte
 * form. Rationale: the INIT chooses the mode, it cannot depend on the table
 * it installs.
 *
 * This exception matters MORE here than on the receiver side: this client is
 * the ONLY one of the parsers sharing this table that ALSO filters the main
 * path through is_command_header (see the `else if
 * (!is_command_header(...))` call in handle_datagram_inner -- the receiver's
 * own main path is filtered only through net.c, never through
 * proto_is_command_header alone). An unrecognized CMD_INIT here is therefore
 * not merely swallowed as a continuation: it is counted as stray_payload and
 * LOST -- the padded session would NEVER open.
 *
 * CMD_INIT's field bounds (b[1] <= 1, b[2] <= 3) remain, for the same reason
 * as before this exception: a PCM tail that resembles a CMD_INIT caused 93%
 * of blocks to be lost (see further below). They extend to the 8-byte form
 * -- an 8-byte PCM tail aligned on these bounds is additionally required to
 * have a zero tail.
 */
static int is_command_header(const uint8_t *b, size_t len, int compressed, int padded)
{
    if (len < 1) return 0;

    /* (a) what does not depend on ANY session mode: 0x03, 0x06, 0x07. */
    const int blit_field = (b[0] == CMD_BLIT_FIELD_VSYNC) &&
        (len == 8u || (len == 9u && b[8] == 0x01u) || (compressed && (len == 12u || len == 13u)));
    if ((b[0] == CMD_SWITCHRES  && len == 26u) ||
        (b[0] == CMD_BLIT_VSYNC && (len == 7u || len == 11u)) ||
        blit_field) {
        return 1;
    }

    /* (b) CMD_INIT: recognized at 4, 5 and 8-with-zero-tail, IN BOTH MODES --
     * it negotiates the mode, it cannot depend on it. The field bounds,
     * mirroring parse_cmd_init on the receiver side and this file's own
     * historical fix (93% of blocks lost, measured) on the rate-code bound,
     * remain and also apply to the 8-byte form. A real INIT always carries
     * compression 0/1 and a rate code 0..3 (gm_send_init). */
    if (b[0] == CMD_INIT) {
        if ((len == 4u || len == 5u) && b[1] <= 1u && b[2] <= 3u) return 1;
        if (len == 8u && b[1] <= 1u && b[2] <= 3u && zero_tail(b, 4u, 8u)) return 1;
        return 0;
    }

    /* (c) the four short commands: the TABLE depends on the mode. The
     * classic length is not accepted in a padded session, and vice versa --
     * one session, one mode, one table. */
    if (padded) {
        if (b[0] == CMD_CLOSE       && len == 8u && zero_tail(b, 1u, 8u)) return 1;
        if (b[0] == CMD_GET_STATUS  && len == 8u && zero_tail(b, 1u, 8u)) return 1;
        if (b[0] == CMD_GET_VERSION && len == 8u && zero_tail(b, 1u, 8u)) return 1;
        if (b[0] == CMD_AUDIO       && len == 8u && zero_tail(b, 3u, 8u)) return 1;
        return 0;
    } else {
        if (b[0] == CMD_CLOSE       && len == 1u) return 1;
        if (b[0] == CMD_GET_STATUS  && len == 1u) return 1;
        if (b[0] == CMD_GET_VERSION && len == 1u) return 1;
        if (b[0] == CMD_AUDIO       && len == 3u) return 1;
        return 0;
    }
}

static void send_status(gmc_t *c, uint32_t frame_echo, uint16_t vcount_echo)
{
    if (!c->cfg.send_status || !c->have_src) return;
    uint8_t pkt[13];
    c->ack_frame++;
    uint8_t flags = FLAG_VRAM_READY | FLAG_VRAM_END_FRAME;
    if (c->have_geom) flags |= FLAG_VRAM_SYNCED;
    if (c->audio_announced) flags |= FLAG_AUDIO;
    if (c->have_geom && c->geom.interlaced && (c->ack_frame & 1u)) flags |= FLAG_VGA_F1;
    uint16_t vcount = 0;
    memcpy(&pkt[0], &frame_echo, 4); memcpy(&pkt[4], &vcount_echo, 2);
    memcpy(&pkt[6], &c->ack_frame, 4); memcpy(&pkt[10], &vcount, 2);
    pkt[12] = flags;
    sendto(c->sock, (const char *)pkt, sizeof pkt, 0, (struct sockaddr *)&c->last_src, sizeof c->last_src);
    c->status_sent++;
}

/* Decoding gmsource's binary test strip: 32 bits, 4 px per bit, on one line. */
static uint32_t decode_strip(const uint32_t *row, uint32_t w)
{
    if (w < 128) return 0xFFFFFFFFu;
    uint32_t id = 0;
    for (unsigned bit = 0; bit < 32; bit++) {
        const uint32_t px = row[bit * 4 + 1];
        const unsigned lum = ((px >> 16) & 0xFF) + ((px >> 8) & 0xFF) + (px & 0xFF);
        id = (id << 1) | (lum > 384 ? 1u : 0u);
    }
    return id;
}

static void verify_pattern(gmc_t *c, const gmc_frame *f)
{
    if (!c->cfg.verify_pattern || f->width < 144 || f->height < 4 || f->dup) return;
    const uint32_t id0 = decode_strip(f->xrgb, f->width);
    const uint32_t id1 = decode_strip(f->xrgb + f->width, f->width);
    int ok;
    if (f->interlaced && f->half_mode) {
        /* one field per send: the two lines come from adjacent frames */
        const uint32_t d = (id0 > id1) ? id0 - id1 : id1 - id0;
        ok = (d <= 1u) && (id0 == f->frame_id || id1 == f->frame_id);
    } else if (f->interlaced) {
        ok = (id0 == f->frame_id) && (id1 == f->frame_id);
    } else {
        ok = (id0 == f->frame_id);
    }
    if (ok) c->pattern_ok++; else c->pattern_mismatch++;
    /* field marker: line 2 red (even), line 3 blue (odd). In half-height mode
     * (one field per send), judge it only once both parities have been seen at
     * least once since the last geometry change (c->half_seen == 3): before that,
     * the still-zeroed half of the canvas would fail the marker without a real
     * defect. For a forced-incomplete 0x07 pair (half-height disabled), the same
     * guard applies via fields_present == 3: a forced incomplete pair
     * (pairs_incomplete, only one field written) leaves the other half of the
     * canvas holding content from a DIFFERENT frame -- the marker is not
     * meaningful there. */
    if (f->interlaced && f->fields_present == 3u && (!f->half_mode || c->half_seen == 3u)) {
        const uint32_t r = f->xrgb[2 * f->width + 4], b = f->xrgb[3 * f->width + 4];
        const int red_ok  = ((r >> 16) & 0xFF) > 128 && (r & 0xFF) < 64;
        const int blue_ok = (b & 0xFF) > 128 && ((b >> 16) & 0xFF) < 64;
        if (!(red_ok && blue_ok)) c->field_swapped++;
    }
}

/* --- ring (called under the mutex) ------------------------------------------------ */
static unsigned ring_pop_oldest(gmc_t *c)
{
    unsigned idx = c->ready[c->ready_head];
    c->ready_head = (c->ready_head + 1) % c->cap;
    c->ready_count--;
    c->slots[idx].in_ring = 0;
    return idx;
}
static void ring_push(gmc_t *c, unsigned idx)
{
    c->ready[(c->ready_head + c->ready_count) % c->cap] = idx;
    c->ready_count++;
    c->slots[idx].in_ring = 1;
}
static int slot_take_free(gmc_t *c)
{
    for (unsigned i = 0; i < c->cap; i++)
        if (!c->slots[i].in_ring && !c->slots[i].held) return (int)i;
    return -1;
}

/* Copies the canvas into a slot and pushes it into the ring. */
/* EMIT_HELD: records time held under c->mtx, on EVERY exit from emit_frame.
 * The recording itself happens under the lock, so it is not counted in the
 * value it publishes -- the real duration is marginally higher. */
#define EMIT_HELD() samples_push(&c->emit_lock_us, (double)(gmc_now_ns() - t_held0) / 1e3)
static void emit_frame(gmc_t *c, uint32_t frame_id, uint16_t vsync, uint8_t fields, uint8_t dup,
                       uint8_t half_mode, uint64_t t_last_byte, uint64_t t_decode_start)
{
    const size_t px = (size_t)c->geom.width * c->geom.height;
    MTX_LOCK(&c->mtx);
    const uint64_t t_held0 = gmc_now_ns();
    int idx = slot_take_free(c);
    if (idx < 0) {
        if (c->ready_count == 0) { EMIT_HELD(); MTX_UNLOCK(&c->mtx); return; }   /* everything is held by the shell */
        idx = (int)ring_pop_oldest(c);
        c->overflow_drops++;
    }
    slot_t *s = &c->slots[idx];
    gmc_frame *f = &s->f;
    if (f->xrgb == NULL || f->width * f->height < px) {
        free(f->xrgb);
        f->xrgb = (uint32_t *)malloc(px * sizeof(uint32_t));
        if (!f->xrgb) { EMIT_HELD(); MTX_UNLOCK(&c->mtx); return; }
    }
    memcpy(f->xrgb, c->canvas, px * sizeof(uint32_t));
    f->width = c->geom.width; f->height = c->geom.height; f->interlaced = c->geom.interlaced;
    f->fields_present = fields; f->dup = dup; f->half_mode = half_mode;
    f->frame_id = frame_id; f->vsync = vsync; f->seq = ++c->seq;
    f->t_last_byte_ns = t_last_byte; f->t_ready_ns = gmc_now_ns(); f->generation = c->geom.generation;
    ring_push(c, (unsigned)idx);
    c->frames_emitted++;
    if (dup) c->dups++;
    if (t_decode_start) samples_push(&c->decode_us, (double)(f->t_ready_ns - t_decode_start) / 1e3);
    if (c->depth_hist) c->depth_hist[c->ready_count <= c->cap ? c->ready_count : c->cap]++;
    verify_pattern(c, f);
    if (c->evlog)
        fprintf(c->evlog, "{\"ev\":\"emit\",\"seq\":%llu,\"frame_id\":%u,\"fields\":%u,\"dup\":%u,\"t_last_byte\":%llu,\"t_ready\":%llu,\"depth\":%u}\n",
                (unsigned long long)f->seq, frame_id, fields, dup,
                (unsigned long long)t_last_byte, (unsigned long long)f->t_ready_ns, c->ready_count);
    EMIT_HELD();
    MTX_UNLOCK(&c->mtx);
    send_status(c, frame_id, vsync);
}

/* --- geometry ----------------------------------------------------------------------- */
static void apply_switchres(gmc_t *c, const uint8_t *b)
{
    double pclock; memcpy(&pclock, b + 1, 8);
    const uint16_t h_active = rd16(b + 9),  h_total = rd16(b + 15);
    const uint16_t v_active = rd16(b + 17), v_total = rd16(b + 23);
    const uint8_t  interlace = b[25];
    if (h_active == 0 || v_active == 0 || h_active > GMC_MAX_W || v_active > GMC_MAX_H) { c->unknown_opcode++; return; }

    const uint32_t full_h = interlace ? (uint32_t)(v_active & ~1u) : v_active;
    const int changed = !c->have_geom || c->geom.width != h_active || c->geom.height != full_h || c->geom.interlaced != (interlace ? 1 : 0);
    c->switchres++;
    if (!changed) return;

    MTX_LOCK(&c->mtx);
    c->geom.width = h_active; c->geom.height = full_h; c->geom.interlaced = interlace ? 1 : 0;
    c->geom.refresh_hz = (h_total && v_total) ? (pclock * 1e6 / ((double)h_total * v_total)) * (interlace ? 2.0 : 1.0) : 0.0;
    c->geom.generation++;
    c->have_geom = 1;
    c->blit_w = h_active;
    c->blit_lines_full  = full_h;
    c->blit_lines_field = full_h / 2;
    const size_t px = (size_t)h_active * full_h;
    if (px > c->canvas_px) {
        free(c->canvas);
        c->canvas = (uint32_t *)calloc(px, sizeof(uint32_t));
        c->canvas_px = c->canvas ? px : 0;
    } else if (c->canvas) {
        memset(c->canvas, 0, px * sizeof(uint32_t));
    }
    const size_t need = px * 3u;
    if (need > c->scratch_cap) { free(c->scratch); c->scratch = (uint8_t *)malloc(need); c->scratch_cap = c->scratch ? need : 0; }
    if (need > c->pend_cap)    { free(c->pend_buf); c->pend_buf = (uint8_t *)malloc(need); c->pend_cap = c->pend_buf ? need : 0; }
    c->pair_active = 0; c->pair_fields = 0;
    c->half_seen = 0;
    c->geom_changes++;
    if (c->geom_log_len < sizeof(c->geom_log) - 96)
        c->geom_log_len += (size_t)snprintf(c->geom_log + c->geom_log_len, sizeof(c->geom_log) - c->geom_log_len,
            "%s{\"w\":%u,\"h\":%u,\"i\":%u,\"hz\":%.3f}", c->geom_log_len ? "," : "", h_active, full_h, interlace ? 1 : 0, c->geom.refresh_hz);
    MTX_UNLOCK(&c->mtx);
    if (c->evlog) fprintf(c->evlog, "{\"ev\":\"switchres\",\"w\":%u,\"h\":%u,\"interlace\":%u,\"hz\":%.3f}\n", h_active, full_h, interlace, c->geom.refresh_hz);
}

/* --- RGB888 -> canvas conversion ----------------------------------------------------- */
static void rows_to_canvas(gmc_t *c, const uint8_t *rgb, uint32_t lines, uint32_t first_row, uint32_t row_step)
{
    const uint32_t w = c->geom.width;
    for (uint32_t r = 0; r < lines; r++) {
        const uint32_t dst_row = first_row + r * row_step;
        if (dst_row >= c->geom.height) break;
        uint32_t *dst = c->canvas + (size_t)dst_row * w;
        const uint8_t *src = rgb + (size_t)r * w * 3u;
        for (uint32_t x = 0; x < w; x++, src += 3)
            dst[x] = ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | (uint32_t)src[2];
    }
}

/* --- end of a video payload ----------------------------------------------------------- */
static void finish_video(gmc_t *c, uint64_t t_last_byte)
{
    const uint64_t t0 = gmc_now_ns();
    const uint8_t *src = c->pend_buf;
    const size_t clear = (size_t)c->blit_w * (c->geom.interlaced ? c->blit_lines_field : c->blit_lines_full) * 3u;

    if (c->pend_is_delta) { c->deltas++; return; }               /* refused: GroovyMAME does not emit it, neither do we */
    if (c->pend_is_lz4) {
        const int n = LZ4_decompress_safe((const char *)c->pend_buf, (char *)c->scratch, (int)c->pend_got, (int)c->scratch_cap);
        if (n < 0 || (size_t)n != clear) {
            c->lz4_fail++;
            if (c->evlog) fprintf(c->evlog, "{\"ev\":\"lz4_fail\",\"frame_id\":%u,\"got\":%d,\"clear\":%zu}\n", c->pend_frame_id, n, clear);
            return;
        }
        src = c->scratch;
    } else if (c->pend_got != clear) {
        c->lz4_fail++;  /* wrong-sized raw payload: counted with decode failures */
        return;
    }

    if (!c->geom.interlaced) {
        rows_to_canvas(c, src, c->blit_lines_full, 0, 1);
        emit_frame(c, c->pend_frame_id, c->pend_vsync, 3, 0, 0, t_last_byte, t0);
        return;
    }

    /* 480i */
    if (c->pend_opcode == CMD_BLIT_VSYNC) {
        /* one field per send (GroovyMAME, gm_send_blit_half): parity = frame_id & 1 */
        const uint8_t field = (uint8_t)(c->pend_frame_id & 1u);
        rows_to_canvas(c, src, c->blit_lines_field, field, 2);
        c->half_fields++;
        c->half_seen |= (uint8_t)(1u << field);
        emit_frame(c, c->pend_frame_id, c->pend_vsync, 3, 0, 1, t_last_byte, t0);
        return;
    }
    /* 0x07: two fields per frame, same frame_id */
    if (c->pair_active && c->pair_frame_id != c->pend_frame_id) {
        /* the previous pair was not completed: emit it as-is */
        c->pairs_incomplete++;
        emit_frame(c, c->pair_frame_id, c->pair_vsync, c->pair_fields, 0, 0, c->pair_t_last_byte, 0);
        c->pair_active = 0; c->pair_fields = 0;
    }
    const uint8_t field = c->pend_field & 1u;
    rows_to_canvas(c, src, c->blit_lines_field, field, 2);
    c->pair_active = 1; c->pair_frame_id = c->pend_frame_id; c->pair_vsync = c->pend_vsync;
    c->pair_fields |= (uint8_t)(1u << field); c->pair_t_last_byte = t_last_byte;
    if (c->pair_fields == 3) {
        emit_frame(c, c->pair_frame_id, c->pair_vsync, 3, 0, 0, t_last_byte, t0);
        c->pair_active = 0; c->pair_fields = 0;
    }
}

/* --- end of an audio payload ------------------------------------------------------------- */
static void finish_audio(gmc_t *c)
{
    if (c->audio_src_hz == 0u) return;                 /* no audio announced: nothing to play */
    const size_t n_bytes = c->pend_got & ~(size_t)1u;  /* ignore a residual odd byte */
    if (n_bytes == 0u) return;

    size_t in_frames;
    if (c->audio_channels == 1u) {
        /* mono -> stereo: duplicate each sample */
        in_frames = n_bytes / 2u;
        if (in_frames > GMC_AUDIO_SCRATCH_CAP) in_frames = GMC_AUDIO_SCRATCH_CAP;
        for (size_t i = 0; i < in_frames; i++) {
            const int16_t s = (int16_t)rd16(c->apend_buf + i * 2u);
            c->astereo[i * 2u] = s; c->astereo[i * 2u + 1u] = s;
        }
    } else {
        /* any channel count other than 1 is treated as stereo */
        in_frames = n_bytes / 4u;
        if (in_frames > GMC_AUDIO_SCRATCH_CAP) in_frames = GMC_AUDIO_SCRATCH_CAP;
        for (size_t i = 0; i < in_frames; i++) {
            c->astereo[i * 2u]      = (int16_t)rd16(c->apend_buf + i * 4u);
            c->astereo[i * 2u + 1u] = (int16_t)rd16(c->apend_buf + i * 4u + 2u);
        }
    }
    if (in_frames == 0u) return;

    size_t out_cap = groovy_resample_capacity(in_frames, c->audio_src_hz, 48000u);
    if (out_cap > GMC_AUDIO_RESAMP_CAP) out_cap = GMC_AUDIO_RESAMP_CAP;
    const size_t got = groovy_resample(&c->ar, c->astereo, in_frames, c->aresamp, out_cap);
    if (got == 0u) return;

    MTX_LOCK(&c->mtx);
    c->audio_frames_in += got;
    for (size_t i = 0; i < got; i++) {
        if (c->acount >= c->acap) {
            /* ring full: drop the oldest pair (same policy as video) */
            c->ahead = (c->ahead + 1u) % c->acap;
            c->acount--;
            c->audio_overruns++;
        }
        const unsigned w = (c->ahead + c->acount) % c->acap;
        c->apcm[w * 2u]      = c->aresamp[i * 2u];
        c->apcm[w * 2u + 1u] = c->aresamp[i * 2u + 1u];
        c->acount++;
    }
    MTX_UNLOCK(&c->mtx);
}

/* --- datagram handling --------------------------------------------------------------- */
static void start_video(gmc_t *c, uint8_t opcode, uint32_t frame_id, uint16_t vsync, uint8_t field, uint32_t lz4_size, int is_delta)
{
    if (!c->have_geom || !c->pend_buf) { c->stray_payload++; return; }
    const size_t clear = (size_t)c->blit_w * (c->geom.interlaced ? c->blit_lines_field : c->blit_lines_full) * 3u;
    const size_t expected = lz4_size ? lz4_size : clear;
    if (expected == 0) { c->blit_empty++; return; }
    if (expected > c->pend_cap) { c->unknown_opcode++; return; }
    c->pend_kind = PEND_VIDEO; c->pend_expected = expected; c->pend_got = 0;
    c->pend_frame_id = frame_id; c->pend_vsync = vsync; c->pend_field = field; c->pend_opcode = opcode;
    c->pend_lz4 = lz4_size; c->pend_is_lz4 = lz4_size != 0; c->pend_is_delta = is_delta;
    c->pend_t_first = gmc_now_ns();
}

static void handle_header(gmc_t *c, const uint8_t *b, size_t len)
{
    switch (b[0]) {
    case CMD_INIT:
        c->inits++;
        c->session_open = 1;
        c->padded = (len == 8u);   /* set by the received length */
        c->compressed = (b[1] == 1);
        c->audio_rate_code = b[2]; c->audio_channels = b[3];
        c->audio_announced = (b[2] != 0 && b[3] != 0);
        {
            const unsigned hz = groovy_audio_rate_hz(c->audio_rate_code);
            if (hz != c->audio_src_hz) {
                MTX_LOCK(&c->mtx);
                c->audio_src_hz = hz;
                groovy_resample_reset(&c->ar, hz, 48000u);
                /* a rate change empties the ring: mixing two rates would produce
                 * an audible pitch drift */
                c->ahead = c->acount = 0u;
                c->aprerolled = 0;
                MTX_UNLOCK(&c->mtx);
            }
        }
        if (c->evlog) fprintf(c->evlog, "{\"ev\":\"init\",\"len\":%zu,\"compression\":%u,\"rate\":%u,\"channels\":%u,\"padded\":%d}\n", len, b[1], b[2], b[3], c->padded);
        break;
    case CMD_SWITCHRES:
        apply_switchres(c, b);
        break;
    case CMD_BLIT_VSYNC: {
        c->blit06++;
        const uint32_t frame_id = rd32(b + 1); const uint16_t vsync = rd16(b + 5);
        const uint32_t lz4 = (len == 11u) ? rd32(b + 7) : 0u;
        start_video(c, CMD_BLIT_VSYNC, frame_id, vsync, 0, lz4, 0);
        break; }
    case CMD_BLIT_FIELD_VSYNC: {
        const uint32_t frame_id = rd32(b + 1); const uint8_t field = b[5]; const uint16_t vsync = rd16(b + 6);
        if (len == 9u) {   /* frame_dup: repeat the canvas as-is */
            if (c->have_geom) emit_frame(c, frame_id, vsync, 3, 1, 0, gmc_now_ns(), 0);
            break;
        }
        c->blit07++;
        const uint32_t lz4 = (len >= 12u) ? rd32(b + 8) : 0u;
        start_video(c, CMD_BLIT_FIELD_VSYNC, frame_id, vsync, field, lz4, len == 13u);
        break; }
    case CMD_AUDIO: {
        /* The size offset (b[1..2]) does not move between the two forms --
         * padding only adds zeros at the tail. */
        const uint16_t size = rd16(b + 1);
        c->audio_blocks++;
        if (size) {
            /* rejected and counted beyond capacity, never silently truncated
             * in a memcpy -- same discipline as start_video. */
            if (size > GMC_AUDIO_PEND_CAP) { c->unknown_opcode++; break; }
            c->pend_kind = PEND_AUDIO; c->pend_expected = size; c->pend_got = 0;
        }
        break; }
    case CMD_GET_STATUS:
        c->get_status++;
        send_status(c, c->pair_active ? c->pair_frame_id : c->pend_frame_id, c->pend_vsync);
        break;
    case CMD_CLOSE:
        c->closes++; c->session_open = 0;
        c->padded = 0;   /* a new session does not inherit the previous one's mode */
        if (c->evlog) fprintf(c->evlog, "{\"ev\":\"close\"}\n");
        break;
    case CMD_GET_VERSION:
        break;
    default:
        c->unknown_opcode++;
    }
}

/* Datagram outcome, for the diagnostic trace:
 *   H = header handled while no block was in progress
 *   P = payload chunk, block still incomplete
 *   C = payload chunk that COMPLETES the block (video or audio)
 *   E = header arrived while a block was in progress: escape, block dropped, header handled
 *   S = stray payload (no block in progress, and this is not a header) */
static char handle_datagram_inner(gmc_t *c, const uint8_t *b, size_t len, uint64_t t_arrival)
{
    c->datagrams++; c->bytes += len;
    char outcome = 'H';
    if (c->pend_kind != PEND_NONE) {
        /* Mirrors the hardware receiver's own audio escape check (not published
         * here): a datagram that LOOKS
         * like a header but exactly COMPLETES the audio block at the byte level
         * is payload, not a header. gm_send_audio emits raw PCM with no opcode
         * prefix, in MTU-sized chunks: its short tail frequently aliases a
         * 4-byte CMD_INIT (0x02...). Without this rule the block would be
         * dropped AND audio_src_hz / compressed corrupted by the fake header
         * (93% of blocks lost, measured). Applied to the audio path ONLY -- video
         * (lz4) keeps its escape logic unchanged. MTU chunking guarantees that
         * the LAST chunk of a block is always exactly the remainder, never a
         * real mid-block header. (Classic session: it is the only short chunk.
         * Padded session: the last TWO chunks may be short -- 1472 + 738 +
         * 738 -- and both are >= 256 bytes, far above any header length.) */
        int is_payload = !is_command_header(b, len, c->compressed, c->padded)
            || (c->pend_kind == PEND_AUDIO
                && len == (size_t)(c->pend_expected - c->pend_got));
        if (is_payload) {
            /* payload */
            size_t take = len;
            const size_t remaining = c->pend_expected - c->pend_got;
            if (take > remaining) { c->overlong++; take = remaining; }
            if (c->pend_kind == PEND_VIDEO) {
                memcpy(c->pend_buf + c->pend_got, b, take);
            } else if (c->pend_kind == PEND_AUDIO) {
                memcpy(c->apend_buf + c->pend_got, b, take);
                c->audio_bytes += take;
            }
            c->pend_got += take;
            if (c->pend_got >= c->pend_expected) {
                const int kind = c->pend_kind;
                c->pend_kind = PEND_NONE;
                if (kind == PEND_VIDEO) finish_video(c, t_arrival);
                else if (kind == PEND_AUDIO) finish_audio(c);
                return 'C';
            }
            return 'P';
        }
        /* a header arrives while a payload was in flight: datagram(s) lost */
        c->cont_escape++;
        /* Split by the kind of block that was cut -- a lost video chunk is
         * not a reason to advise audio=off. */
        if (c->pend_kind == PEND_AUDIO) c->cont_escape_audio++;
        else                            c->cont_escape_video++;
        if (c->evlog) fprintf(c->evlog, "{\"ev\":\"continuation_escape\",\"opcode\":%u,\"got\":%zu,\"expected\":%zu}\n", b[0], c->pend_got, c->pend_expected);
        c->pend_kind = PEND_NONE;
        outcome = 'E';
    } else if (!is_command_header(b, len, c->compressed, c->padded)) {
        c->stray_payload++;
        return 'S';
    }
    handle_header(c, b, len);
    return outcome;
}

/* Wrapper around handle_datagram_inner: without a trace, strictly the same work.
 * With a trace (cfg.trace_log_path), one CSV line per datagram:
 *   t_ns,len,b0,out,bk,bg,be,ba,ak,ag,ae,aa
 * t_ns = gmc_now_ns() on receipt; b0 = first byte; out = outcome (H/P/C/E/S);
 * (the successor of a datagram ending a video payload is stamped BEFORE that
 * payload is decoded, within GMC_TRACE_PEEK_NS -- see rx_loop);
 * bk/bg/be = pend_kind (0 none, 1 video, 2 audio) / pend_got / pend_expected BEFORE;
 * ak/ag/ae = the same AFTER; ba/aa = count of CMD_AUDIO headers seen before/after,
 * which serves as an identifier for the audio block in progress or the last one
 * seen (a stray audio chunk thus attaches to the block that precedes it).
 * Periodic fflush: macOS closing via AppleEvent goes through exit(), which
 * flushes streams, but a SIGTERM would not. */
static void handle_datagram(gmc_t *c, const uint8_t *b, size_t len, uint64_t t_arrival)
{
    if (!c->trace) { (void)handle_datagram_inner(c, b, len, t_arrival); return; }
    const int bk = c->pend_kind; const size_t bg = c->pend_got, be = c->pend_expected;
    const unsigned long long ba = (unsigned long long)c->audio_blocks;
    const char out = handle_datagram_inner(c, b, len, t_arrival);
    fprintf(c->trace, "%llu,%zu,%u,%c,%d,%zu,%zu,%llu,%d,%zu,%zu,%llu\n",
            (unsigned long long)t_arrival, len, (unsigned)(len ? b[0] : 0u), out,
            bk, bg, be, ba, c->pend_kind, c->pend_got, c->pend_expected,
            (unsigned long long)c->audio_blocks);
    if (++c->trace_lines % 512u == 0u) fflush(c->trace);
}

/* Knocks on the door as long as the emitter is known but geometry is not. This
 * is the status -- frame_echo = 0 and a growing own counter -- that
 * groovy_reannounce_should_fire reads on the emitter side (unknown address;
 * counter that goes backward after a restart). It stops as soon as have_geom
 * becomes 1.
 * Only caller: rx_loop -- not compiled under GMCLIENT_TEST (the receiving
 * thread does not exist under test), hence the same guard here to avoid an
 * unused-function warning. */
#ifndef GMCLIENT_TEST
static void maybe_status_nogeom(gmc_t *c, uint64_t now_ns)
{
    if (!c->have_src || c->have_geom || !c->cfg.send_status) return;
    if (now_ns - c->last_nogeom_status_ns < 250000000ULL) return;
    send_status(c, 0, 0);
    c->status_nogeom++;
    c->last_nogeom_status_ns = now_ns;
}
#endif /* GMCLIENT_TEST */

/* --- receiving thread ------------------------------------------------------------------ */
/* Guarded under GMCLIENT_TEST (client/test_gmclient_audio.c): the receiving thread
 * is never started under test (gmc_open skips thread/socket creation there), so
 * this body would stay unreferenced -- same pattern as the hardware receiver's own
 * input handling (guarded
 * by INPUT_TEST) for its evdev/socket thread. */
#ifndef GMCLIENT_TEST
static void rx_loop(gmc_t *c)
{
    uint8_t *buf = (uint8_t *)malloc(GMC_DGRAM_MAX);
    if (!buf) return;
    /* Second buffer for the successor read ahead of a video decode -- trace only. */
    uint8_t *buf2 = c->trace ? (uint8_t *)malloc(GMC_DGRAM_MAX) : NULL;
    while (!c->stop) {
        maybe_status_nogeom(c, gmc_now_ns());
        fd_set rf; FD_ZERO(&rf); FD_SET(c->sock, &rf);
        int nfds = (int)c->sock;
        /* This thread ALSO owns the input channel's socket. retro_run makes no
         * system call anymore -- it deposits the gamepad state, we send it here. */
        const int ifd = c->input ? gmc_input_fdset_add(c->input, &rf) : -1;
        if (ifd > nfds) nfds = ifd;
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 2000;
        const int r = select(nfds + 1, &rf, NULL, NULL, &tv);
        if (r > 0) {
            if (c->input && gmc_input_fdset_isset(c->input, &rf))
                gmc_input_service_rx(c->input);   /* peer handshake, and draining */
            if (FD_ISSET(c->sock, &rf)) {
                for (;;) {
                    struct sockaddr_in src; socklen_t sl = sizeof src;
                    const int n = recvfrom(c->sock, (char *)buf, GMC_DGRAM_MAX, 0, (struct sockaddr *)&src, &sl);
                    if (n <= 0) break;
                    const uint64_t t = gmc_now_ns();
                    c->last_src = src; c->have_src = 1;

                    /* UNDER TRACE ONLY (see GMC_TRACE_PEEK_NS, the bench latency check). The datagram that ends a
                     * video payload makes finish_video decompress and copy the image
                     * BEFORE the next recvfrom: the successor's t_ns then carried that
                     * decode time (p50 ~150 us measured), a floor that blinded the trace
                     * to any gap shorter than it. So when `buf` is about to end a video
                     * payload, read and TIMESTAMP the successor first -- a bounded busy
                     * wait -- then handle both, in arrival order. Same datagrams, same
                     * order, same trace lines: what is counted does not change, only
                     * WHEN the successor is stamped. A successor later than the bound is
                     * stamped after the decode, as before. Without a trace, `buf2` is
                     * NULL and this block is dead. */
                    int n2 = 0; uint64_t t2 = 0; struct sockaddr_in src2;
                    if (buf2 && c->pend_kind == PEND_VIDEO
                        && (size_t)n >= c->pend_expected - c->pend_got) {
                        do {
                            socklen_t sl2 = sizeof src2;
                            n2 = recvfrom(c->sock, (char *)buf2, GMC_DGRAM_MAX, 0, (struct sockaddr *)&src2, &sl2);
                            t2 = gmc_now_ns();
                        } while (n2 <= 0 && t2 - t < GMC_TRACE_PEEK_NS);
                    }

                    handle_datagram(c, buf, (size_t)n, t);
                    if (n2 > 0) {
                        c->last_src = src2;
                        handle_datagram(c, buf2, (size_t)n2, t2);
                    }
                }
            }
        }
        /* Outside the `if`: so on EVERY received video frame AND on select's timeout
         * when nothing arrives. The real pace is not this loop's but retro_run's
         * deposits -- gmc_input_flush does nothing until something has been
         * deposited -- which gives the mister driver the steady one-frame-per-image
         * stream it expects. */
        if (c->input) (void)gmc_input_flush(c->input);
    }
    free(buf); free(buf2);
}
#ifdef _WIN32
static unsigned __stdcall rx_thread(void *arg) { rx_loop((gmc_t *)arg); return 0; }
#else
static void *rx_thread(void *arg) { rx_loop((gmc_t *)arg); return NULL; }
#endif
#endif /* GMCLIENT_TEST */

/* --- audio: allocation/release, called by gmc_open/gmc_close, including
 * under test -- no network dependency here. ---------------------------------------------- */
static void audio_alloc(gmc_t *c)
{
    c->acap = GMC_AUDIO_RING_CAP;
    c->apcm = (int16_t *)calloc((size_t)c->acap * 2u, sizeof(int16_t));
    c->apend_buf = (uint8_t *)malloc(GMC_AUDIO_PEND_CAP);
    c->astereo = (int16_t *)calloc((size_t)GMC_AUDIO_SCRATCH_CAP * 2u, sizeof(int16_t));
    c->aresamp = (int16_t *)calloc((size_t)GMC_AUDIO_RESAMP_CAP * 2u, sizeof(int16_t));
}
static void audio_free(gmc_t *c)
{
    free(c->apcm); free(c->apend_buf); free(c->astereo); free(c->aresamp);
}

/* --- API -------------------------------------------------------------------------- */
void gmc_config_defaults(gmc_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->port = 32100; cfg->target_depth = 1; cfg->ring_capacity = 8; cfg->send_status = 1;
}

gmc_t *gmc_open(const gmc_config *cfg)
{
#ifndef GMCLIENT_TEST
#ifdef _WIN32
    WSADATA wsa; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
#endif
#endif
    gmc_t *c = (gmc_t *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->cfg = *cfg;
    c->input = cfg->input_chan;   /* set BEFORE the thread starts */
    if (c->cfg.ring_capacity < c->cfg.target_depth + 2) c->cfg.ring_capacity = c->cfg.target_depth + 2;
    c->cap = c->cfg.ring_capacity;
    c->slots = (slot_t *)calloc(c->cap, sizeof(slot_t));
    c->ready = (unsigned *)calloc(c->cap, sizeof(unsigned));
    c->depth_hist = (unsigned *)calloc(c->cap + 1, sizeof(unsigned));
    MTX_INIT(&c->mtx);
    audio_alloc(c);

#ifndef GMCLIENT_TEST
    c->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (c->sock == GMC_BAD_SOCK) { gmc_close(c); return NULL; }
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(c->sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof rcvbuf);
    /* What the kernel actually granted. macOS caps this via kern.ipc.maxsockbuf and
     * can refuse the full request (keeping its own default, ~786 KB); Linux doubles
     * the requested value up to rmem_max; Windows grants it. */
    c->rcvbuf_asked = rcvbuf; c->rcvbuf_got = -1;
    { int got = 0; socklen_t ol = (socklen_t)sizeof got;
      if (getsockopt(c->sock, SOL_SOCKET, SO_RCVBUF, (char *)&got, &ol) == 0) c->rcvbuf_got = got; }
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)c->cfg.port);
    if (bind(c->sock, (struct sockaddr *)&a, sizeof a) != 0) { gmc_close(c); return NULL; }
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(c->sock, FIONBIO, &nb);
#else
    fcntl(c->sock, F_SETFL, fcntl(c->sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (c->cfg.event_log_path) c->evlog = fopen(c->cfg.event_log_path, "w");
    if (c->evlog) fprintf(c->evlog, "{\"ev\":\"rcvbuf\",\"asked\":%d,\"got\":%d}\n", c->rcvbuf_asked, c->rcvbuf_got);
    if (c->cfg.trace_log_path) {
        c->trace = fopen(c->cfg.trace_log_path, "w");
        if (c->trace) {
            setvbuf(c->trace, NULL, _IOFBF, 1u << 20);
            fprintf(c->trace, "# gmclient trace v1 port=%u rcvbuf_asked=%d rcvbuf_got=%d\n",
                    c->cfg.port, c->rcvbuf_asked, c->rcvbuf_got);
            fprintf(c->trace, "t_ns,len,b0,out,bk,bg,be,ba,ak,ag,ae,aa\n");
        }
    }

#ifdef _WIN32
    c->thread = (HANDLE)_beginthreadex(NULL, 0, rx_thread, c, 0, NULL);
    if (!c->thread) { gmc_close(c); return NULL; }
#else
    if (pthread_create(&c->thread, NULL, rx_thread, c) != 0) { gmc_close(c); return NULL; }
#endif
#else
    c->sock = GMC_BAD_SOCK;   /* under test: no real socket or thread */
#endif
    return c;
}

int gmc_acquire(gmc_t *c, gmc_frame **out)
{
    *out = NULL;
    const uint64_t t_wait0 = gmc_now_ns();
    MTX_LOCK(&c->mtx);
    samples_push(&c->acquire_wait_us, (double)(gmc_now_ns() - t_wait0) / 1e3);
    const unsigned target = c->cfg.target_depth;
    if (!c->prerolled) {
        if (c->ready_count < target + 1) { MTX_UNLOCK(&c->mtx); return 0; }
        c->prerolled = 1; c->prerolls++;
    }
    if (c->ready_count == 0) {
        c->underruns++; c->prerolled = 0;      /* re-preroll: rebuilding the reserve */
        MTX_UNLOCK(&c->mtx); return 0;
    }
    while (c->ready_count > target + 1) {     /* too far ahead: skip the oldest */
        (void)ring_pop_oldest(c);
        c->skips++;
    }
    const unsigned idx = ring_pop_oldest(c);
    c->slots[idx].held = 1;
    *out = &c->slots[idx].f;
    samples_push(&c->ring_wait, (double)(gmc_now_ns() - (*out)->t_ready_ns) / 1e6);
    MTX_UNLOCK(&c->mtx);
    return 1;
}

void gmc_release(gmc_t *c, gmc_frame *f)
{
    if (!f) return;
    MTX_LOCK(&c->mtx);
    for (unsigned i = 0; i < c->cap; i++) if (&c->slots[i].f == f) { c->slots[i].held = 0; break; }
    MTX_UNLOCK(&c->mtx);
}

int gmc_geometry(gmc_t *c, gmc_geom *out)
{
    MTX_LOCK(&c->mtx);
    *out = c->geom;
    const int changed = c->have_geom && c->geom.generation != c->geom_seen_by_shell;
    c->geom_seen_by_shell = c->geom.generation;
    MTX_UNLOCK(&c->mtx);
    return changed;
}

uint64_t gmc_cont_escape(gmc_t *c)
{
    uint64_t v;
    if (!c) return 0;
    MTX_LOCK(&c->mtx);
    v = c->cont_escape;
    MTX_UNLOCK(&c->mtx);
    return v;
}

int gmc_audio_announced(gmc_t *c)
{
    int v;
    if (!c) return 0;
    MTX_LOCK(&c->mtx);
    v = c->audio_announced;
    MTX_UNLOCK(&c->mtx);
    return v;
}

void gmc_note_present(gmc_t *c, const gmc_frame *f, uint64_t t_present_ns)
{
    MTX_LOCK(&c->mtx);
    c->presents++;
    samples_push(&c->lat_last_byte, (double)(t_present_ns - f->t_last_byte_ns) / 1e6);
    samples_push(&c->lat_ready,     (double)(t_present_ns - f->t_ready_ns) / 1e6);
    if (c->last_present_ns) samples_push(&c->present_interval, (double)(t_present_ns - c->last_present_ns) / 1e6);
    c->last_present_ns = t_present_ns;
    if (c->evlog) fprintf(c->evlog, "{\"ev\":\"present\",\"seq\":%llu,\"t\":%llu,\"lat_ms\":%.3f}\n",
                          (unsigned long long)f->seq, (unsigned long long)t_present_ns, (double)(t_present_ns - f->t_last_byte_ns) / 1e6);
    MTX_UNLOCK(&c->mtx);
}

void gmc_note_repeat(gmc_t *c, uint64_t t_present_ns)
{
    MTX_LOCK(&c->mtx);
    c->repeats++;
    if (c->last_present_ns) samples_push(&c->present_interval, (double)(t_present_ns - c->last_present_ns) / 1e6);
    c->last_present_ns = t_present_ns;
    MTX_UNLOCK(&c->mtx);
}

size_t gmc_audio_read(gmc_t *c, int16_t *pcm, size_t max_frames)
{
    if (!c || !pcm || max_frames == 0u) return 0u;
    MTX_LOCK(&c->mtx);
    if (c->audio_src_hz == 0u) { MTX_UNLOCK(&c->mtx); return 0u; }
    if (!c->aprerolled) {
        if (c->acount < GMC_AUDIO_PREROLL) { MTX_UNLOCK(&c->mtx); return 0u; }
        c->aprerolled = 1;
    }
    if (c->acount == 0u) {
        c->audio_underruns++;
        c->aprerolled = 0;   /* re-preroll: rebuilding the reserve */
        MTX_UNLOCK(&c->mtx);
        return 0u;
    }
    size_t n = ((size_t)c->acount < max_frames) ? (size_t)c->acount : max_frames;
    for (size_t i = 0; i < n; i++) {
        const unsigned r = (c->ahead + (unsigned)i) % c->acap;
        pcm[i * 2u]      = c->apcm[r * 2u];
        pcm[i * 2u + 1u] = c->apcm[r * 2u + 1u];
    }
    c->ahead = (c->ahead + (unsigned)n) % c->acap;
    c->acount -= (unsigned)n;
    c->audio_frames_out += n;
    samples_push(&c->audio_pull, (double)n);
    MTX_UNLOCK(&c->mtx);
    return n;
}

void gmc_stats_json(gmc_t *c, FILE *out)
{
    MTX_LOCK(&c->mtx);
    fprintf(out, "{\"gmclient\":{");
    fprintf(out, "\"config\":{\"port\":%u,\"target_depth\":%u,\"ring_capacity\":%u,\"send_status\":%d,\"verify_pattern\":%d},",
            c->cfg.port, c->cfg.target_depth, c->cap, c->cfg.send_status, c->cfg.verify_pattern);
    fprintf(out, "\"session\":{\"compressed\":%d,\"audio_announced\":%d,\"padded\":%d,\"geometries\":[%s]},", c->compressed, c->audio_announced, c->padded, c->geom_log);
    fprintf(out, "\"wire\":{\"datagrams\":%llu,\"bytes\":%llu,\"inits\":%llu,\"switchres\":%llu,\"blit_0x06\":%llu,\"blit_0x07_fields\":%llu,"
                 "\"frame_dup\":%llu,\"delta_refused\":%llu,\"lz4_or_size_fail\":%llu,\"continuation_escape\":%llu,"
                 "\"continuation_escape_audio\":%llu,\"continuation_escape_video\":%llu,\"stray_payload\":%llu,"
                 "\"overlong\":%llu,\"unknown_or_rejected\":%llu,\"blit_empty\":%llu,\"audio_blocks\":%llu,\"audio_bytes\":%llu,"
                 "\"get_status\":%llu,\"closes\":%llu,\"status_sent\":%llu,\"status_nogeom\":%llu,\"half_fields\":%llu,\"pairs_incomplete\":%llu,\"geom_changes\":%llu},",
            (unsigned long long)c->datagrams, (unsigned long long)c->bytes, (unsigned long long)c->inits, (unsigned long long)c->switchres,
            (unsigned long long)c->blit06, (unsigned long long)c->blit07, (unsigned long long)c->dups, (unsigned long long)c->deltas,
            (unsigned long long)c->lz4_fail, (unsigned long long)c->cont_escape,
            (unsigned long long)c->cont_escape_audio, (unsigned long long)c->cont_escape_video,
            (unsigned long long)c->stray_payload,
            (unsigned long long)c->overlong, (unsigned long long)c->unknown_opcode, (unsigned long long)c->blit_empty,
            (unsigned long long)c->audio_blocks, (unsigned long long)c->audio_bytes, (unsigned long long)c->get_status,
            (unsigned long long)c->closes, (unsigned long long)c->status_sent, (unsigned long long)c->status_nogeom, (unsigned long long)c->half_fields,
            (unsigned long long)c->pairs_incomplete, (unsigned long long)c->geom_changes);
    fprintf(out, "\"ring\":{\"frames_emitted\":%llu,\"presents\":%llu,\"repeats\":%llu,\"skips\":%llu,\"underruns\":%llu,\"prerolls\":%llu,\"overflow_drops\":%llu,\"depth_hist\":[",
            (unsigned long long)c->frames_emitted, (unsigned long long)c->presents, (unsigned long long)c->repeats,
            (unsigned long long)c->skips, (unsigned long long)c->underruns, (unsigned long long)c->prerolls, (unsigned long long)c->overflow_drops);
    for (unsigned i = 0; i <= c->cap; i++) fprintf(out, "%s%u", i ? "," : "", c->depth_hist[i]);
    fprintf(out, "]},");
    fprintf(out, "\"audio\":{\"src_hz\":%u,\"channels\":%u,\"frames_in\":%llu,\"frames_out\":%llu,"
                 "\"underruns\":%llu,\"overruns\":%llu,\"blocks\":%llu,\"bytes\":%llu},",
            c->audio_src_hz, (unsigned)c->audio_channels,
            (unsigned long long)c->audio_frames_in, (unsigned long long)c->audio_frames_out,
            (unsigned long long)c->audio_underruns, (unsigned long long)c->audio_overruns,
            (unsigned long long)c->audio_blocks, (unsigned long long)c->audio_bytes);
    fprintf(out, "\"pattern\":{\"ok\":%llu,\"mismatch\":%llu,\"field_swapped\":%llu},",
            (unsigned long long)c->pattern_ok, (unsigned long long)c->pattern_mismatch, (unsigned long long)c->field_swapped);
    fprintf(out, "\"latency_ms\":{");
    samples_json(out, "last_byte_to_present", &c->lat_last_byte, 1.0); fputc(',', out);
    samples_json(out, "ready_to_present", &c->lat_ready, 1.0); fputc(',', out);
    samples_json(out, "ready_to_acquire", &c->ring_wait, 1.0); fputc(',', out);
    samples_json(out, "present_interval", &c->present_interval, 1.0); fputc(',', out);
    samples_json(out, "decode_us", &c->decode_us, 1.0);
    fputc('}', out); fputc(',', out);
    fprintf(out, "\"contention\":{");
    samples_json(out, "emit_lock_us", &c->emit_lock_us, 1.0); fputc(',', out);
    samples_json(out, "acquire_wait_us", &c->acquire_wait_us, 1.0); fputc(',', out);
    samples_json(out, "audio_pull_frames", &c->audio_pull, 1.0);
    fprintf(out, "}}}");
    MTX_UNLOCK(&c->mtx);
}

void gmc_close(gmc_t *c)
{
    if (!c) return;
    c->stop = 1;
#ifndef GMCLIENT_TEST
#ifdef _WIN32
    if (c->thread) { WaitForSingleObject(c->thread, 2000); CloseHandle(c->thread); }
    if (c->sock != GMC_BAD_SOCK && c->sock != 0) closesocket(c->sock);
    WSACleanup();
#else
    if (c->thread) pthread_join(c->thread, NULL);
    if (c->sock >= 0) close(c->sock);
#endif
#endif
    if (c->evlog) fclose(c->evlog);
    if (c->trace) fclose(c->trace);
    if (c->slots) for (unsigned i = 0; i < c->cap; i++) free(c->slots[i].f.xrgb);
    free(c->slots); free(c->ready); free(c->depth_hist);
    free(c->canvas); free(c->scratch); free(c->pend_buf);
    free(c->lat_last_byte.v); free(c->lat_ready.v); free(c->decode_us.v); free(c->present_interval.v); free(c->ring_wait.v);
    free(c->emit_lock_us.v); free(c->acquire_wait_us.v); free(c->audio_pull.v);
    audio_free(c);
    MTX_DESTROY(&c->mtx);
    free(c);
}
