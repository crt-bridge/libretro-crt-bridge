/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm_shard.h -- the codec session's own transport: a bitstream cut into shards
 * with a 6-byte header, protected by Reed-Solomon parity, put back together by
 * frame_id.
 *
 * A codec frame is not recovered by the next one: it depends on it. So, in a
 * codec session (CMD_INIT compression 0x10 = H.264, 0x11 = HEVC, padded session
 * only), the image payload leaves the Groovy "arrival order" wire: each datagram
 * carries which frame and which shard it is, and the parity lets a receiver put
 * a frame back together without a lost shard. The commands stay Groovy.
 *
 * Datagram = 6-byte header + `sz` bytes of shard, never under 256 bytes in all
 * (the padded-session rule: a datagram under 256 bytes dies on Wi-Fi).
 *
 *   byte 0..1  frame_id, u16 LE
 *   byte 2     index of this shard, 0 .. k + m - 1 (data first, then parity)
 *   byte 3     k, the number of data shards   (1 .. 254)
 *   byte 4     m, the number of parity shards (0 .. 254, k + m <= 255)
 *   byte 5     flags: bit 0 = the frame starts a decodable sequence (IDR)
 *
 * The data shards hold  u32 LE length || u32 LE Groovy frame_id || u16 LE vsync || bitstream ||
 * zero padding, so that the length and the identity of the picture come out of the recovered data
 * itself (under the parity) and depend on no other datagram: the CMD_BLIT_VSYNC header sent before
 * the shards is a single small datagram, and the Wi-Fi loses those too. The byte 0..1 `frame_id` of
 * the datagram header is the stream's own consecutive counter, not the Groovy one.
 *
 * Pure of the network: both ends are plain buffers, the tests drive them without
 * a socket. Public library: no allocation hidden in the send path other than the
 * working buffers named below. */
#ifndef GM_SHARD_H
#define GM_SHARD_H

#include <stddef.h>
#include <stdint.h>

#define GM_SHARD_HDR        6u
#define GM_SHARD_MIN_DGRAM  256u     /* never a datagram under this (padded session) */
#define GM_SHARD_MAX        255u     /* k + m */
#define GM_SHARD_F_KEY      0x01u

/* How a payload of `len` bytes (the length prefix is added here) is cut for a
 * datagram size `dgram_max` (the session MTU, <= 1472) and a parity percentage:
 * k data shards of `sz` bytes each, m parity shards. Returns 0, or -1 when it
 * does not fit in 255 shards. */
int gm_shard_plan(size_t len, unsigned dgram_max, unsigned parity_pct,
                  unsigned *k, unsigned *m, size_t *sz);

/* Called once per datagram, in order: the data shards, then the parity ones.
 * Return non-zero to stop (the send failed). */
typedef int (*gm_shard_emit_fn)(void *user, const uint8_t *dgram, size_t len);

/* Cut, protect and emit one frame. `seq` numbers the datagrams' frame (consecutive); `gid` and `vsync` are the
 * Groovy frame_id and vsync of the picture, handed back by the receiver. Returns 0, or -1 (bad argument, no
 * memory, the emitter refused a datagram). gm_shard_send is the same with gid = vsync = 0. */
int gm_shard_send_ex(const uint8_t *bitstream, size_t len, uint16_t seq, uint32_t gid, uint16_t vsync, uint8_t flags,
                     unsigned dgram_max, unsigned parity_pct, gm_shard_emit_fn emit, void *user);
int gm_shard_send(const uint8_t *bitstream, size_t len, uint16_t frame_id, uint8_t flags,
                  unsigned dgram_max, unsigned parity_pct,
                  gm_shard_emit_fn emit, void *user);

/* --- reception --- */
typedef struct gm_shard_rx gm_shard_rx;

typedef struct {
    uint16_t       frame_id;    /* the stream's consecutive counter */
    uint32_t       gid;         /* the Groovy frame_id of the picture */
    uint16_t       vsync;
    uint8_t        flags;
    const uint8_t *data;        /* the bitstream, valid until the next push */
    size_t         len;
    unsigned       recovered;   /* data shards rebuilt from parity for this frame */
    unsigned       gap;         /* frames skipped since the last one delivered (0 = consecutive) */
} gm_shard_frame;

typedef struct {
    uint64_t frames;            /* delivered */
    uint64_t recovered_frames;  /* delivered after at least one shard was rebuilt */
    uint64_t lost_frames;       /* given up on: evicted before they could be rebuilt */
    uint64_t stale_frames;      /* completed too late: older than one already delivered */
    uint64_t shards;            /* accepted */
    uint64_t duplicate_shards;
    uint64_t rejected;          /* malformed datagram */
} gm_shard_stats;

gm_shard_rx *gm_shard_rx_new(void);
void         gm_shard_rx_free(gm_shard_rx *r);
void         gm_shard_rx_reset(gm_shard_rx *r);
const gm_shard_stats *gm_shard_rx_stats(const gm_shard_rx *r);

/* Feeds one datagram. Returns 1 when a frame is complete (*out is filled), 0 when
 * nothing is ready yet, -1 when the datagram is not a valid shard. */
int gm_shard_rx_push(gm_shard_rx *r, const uint8_t *dgram, size_t len, gm_shard_frame *out);

#endif /* GM_SHARD_H */
