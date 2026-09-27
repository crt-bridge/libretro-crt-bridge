/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gmclient.h -- Groovy v2 receiving library for a "buffered" follower.
 *
 * Common code shared by two shells: a standalone C + SDL2 application, and a
 * libretro core loaded by RetroArch.
 *
 * Model: a receiving thread (UDP socket, parser, LZ4, 480i field
 * recombination, RGB888 -> XRGB8888 conversion) pushes complete frames into
 * a ring; the shell pulls at ITS OWN rate via gmc_acquire(). "Buffered"
 * policy:
 *   - pre-roll: nothing is rendered until target_depth + 1 frames are
 *     present (at open, and after every starvation);
 *   - empty ring at tick -> 0 (the shell repeats the previous frame);
 *   - ring beyond target_depth + 1 -> the oldest frames are skipped.
 *
 * Portable: Winsock (_WIN32) or POSIX. No dependency beyond lz4.
 */
#ifndef GMCLIENT_H
#define GMCLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gmc gmc_t;

typedef struct {
    uint32_t  width;            /* pixels                                        */
    uint32_t  height;           /* lines of the FULL-height frame (480i: 2x field) */
    uint8_t   interlaced;       /* regime announced by CMD_SWITCHRES              */
    uint8_t   fields_present;   /* bit0 even field, bit1 odd field; 3 = complete; 240p: 3 */
    uint8_t   dup;              /* 1 = produced by a frame_dup (frame repeated by the emitter) */
    uint8_t   half_mode;        /* 1 = one field per send (0x06 half-height, GroovyMAME form) */
    uint32_t  frame_id;         /* wire frame_id                                  */
    uint16_t  vsync;
    uint64_t  seq;              /* client-side serial number, monotonic           */
    uint64_t  t_last_byte_ns;   /* arrival of the last payload byte (before decoding) */
    uint64_t  t_ready_ns;       /* frame decoded, converted, pushed into the ring */
    uint32_t  generation;       /* geometry number (changes on every distinct CMD_SWITCHRES) */
    uint32_t *xrgb;             /* width*height pixels 0x00RRGGBB, pitch = width*4 */
} gmc_frame;

typedef struct {
    uint32_t width, height;     /* full height */
    uint8_t  interlaced;
    double   refresh_hz;        /* derived from the modeline (pclock / (htotal*vtotal)), x2 if interlaced */
    uint32_t generation;
} gmc_geom;

typedef struct {
    unsigned    port;             /* 32100 by default                              */
    unsigned    target_depth;     /* 0..4 frames kept in reserve                   */
    unsigned    ring_capacity;    /* >= target_depth + 2; 8 by default             */
    int         send_status;      /* 1 = reply to 13-byte status polls (default 1) */
    int         verify_pattern;   /* 1 = verify gmsource's binary test pattern     */
    const char *event_log_path;   /* NULL = no detailed per-frame log              */
    const char *trace_log_path;   /* NULL = no per-datagram trace (one CSV line per
                                     received datagram -- monotonic time, length,
                                     first byte, outcome, cursor state before/after,
                                     audio block id) */
    /* The input channel, if present, is MONITORED BY THIS CLIENT'S RECEIVING
     * THREAD -- its socket enters the same select(), its handshake is received
     * there, and it is the one that flushes the mailbox that retro_run deposits
     * into. Passed through CONFIGURATION rather than a setter added after the
     * fact: gmc_open starts the thread immediately, so any later wiring would
     * race it. Deliberately an incomplete type -- this header must not depend
     * on gmclient_input.h. */
    struct gmc_input *input_chan; /* NULL = no input channel (default)             */
} gmc_config;

/* Sensible defaults in *cfg. */
void      gmc_config_defaults(gmc_config *cfg);

/* Opens the socket, starts the thread. NULL on failure (errno/WSAGetLastError). */
gmc_t    *gmc_open(const gmc_config *cfg);

/* 1 = *out points to a new frame (render, then gmc_release);
 * 0 = nothing new this tick (repeat the previous frame, call gmc_note_repeat). */
int       gmc_acquire(gmc_t *c, gmc_frame **out);
void      gmc_release(gmc_t *c, gmc_frame *f);

/* 1 if geometry changed since the last call (out is filled either way). */
int       gmc_geometry(gmc_t *c, gmc_geom *out);

/* Real-time read of the continuation-escape counter: a block whose continuation
 * was cut by a new header, i.e. a lost chunk. Same lock as gmc_stats_json. */
uint64_t  gmc_cont_escape(gmc_t *c);

/* 1 if the last CMD_INIT announced sound (non-zero rate and channels), 0 otherwise.
 * Same lock as gmc_cont_escape: the loss message only advises audio=off when
 * there is sound to remove. */
int       gmc_audio_announced(gmc_t *c);

/* Presentation telemetry, supplied by the shell. */
void      gmc_note_present(gmc_t *c, const gmc_frame *f, uint64_t t_present_ns);
void      gmc_note_repeat(gmc_t *c, uint64_t t_present_ns);

/* Audio: resamples to a fixed 48000 Hz and delivers the stereo PCM
 * accumulated from CMD_AUDIO. Returns the number of pairs written into *pcm
 * (0..max_frames), 0 if no audio is announced or the ring is dry (pre-roll
 * or starvation). */
size_t    gmc_audio_read(gmc_t *c, int16_t *pcm, size_t max_frames);

/* JSON metrics (one object). */
void      gmc_stats_json(gmc_t *c, FILE *out);

uint64_t  gmc_now_ns(void);
void      gmc_close(gmc_t *c);

#ifdef __cplusplus
}
#endif
#endif /* GMCLIENT_H */
