/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gmclient_input.h -- Groovy input channel (UDP, port = video port + 1), CLIENT
 * side. Sends back the RetroPad state that RetroArch has already
 * resolved, to the emitter, in the bit-exact format already proven on the
 * wire.
 *
 * Deliberately a separate file from gmclient.c/gmclient.h: distinct socket,
 * INVERSE flow direction (the video channel RECEIVES, this one SENDS), no
 * relation to the video buffer policy -- the same "one responsibility, one
 * file" separation that the hardware receiver's own sources already practice
 * between its networking, input and display handling.
 *
 * Button masks and wire structures: a CONSCIOUS, verbatim copy of the
 * hardware receiver's own input header -- same field layout, same size guard
 * placed IMMEDIATELY after pack(pop), in the same gesture as the structures
 * -- this repo requires that discipline after finding two duplicated struct
 * definitions without a matching guard. Do NOT invent a bit 14/15: neither
 * upstream source defines them.
 */
#ifndef GMCLIENT_INPUT_H
#define GMCLIENT_INPUT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Button masks -- bit-for-bit identical to the hardware receiver's own input header.
 * Bits 14 and 15: defined by NEITHER upstream source -- do not invent them.
 * ------------------------------------------------------------------------- */
#define GROOVY_JOY_RIGHT (1u<<0)
#define GROOVY_JOY_LEFT  (1u<<1)
#define GROOVY_JOY_DOWN  (1u<<2)
#define GROOVY_JOY_UP    (1u<<3)
#define GROOVY_JOY_B1    (1u<<4)
#define GROOVY_JOY_B2    (1u<<5)
#define GROOVY_JOY_B3    (1u<<6)
#define GROOVY_JOY_B4    (1u<<7)
#define GROOVY_JOY_B5    (1u<<8)
#define GROOVY_JOY_B6    (1u<<9)
#define GROOVY_JOY_B7    (1u<<10)
#define GROOVY_JOY_B8    (1u<<11)
#define GROOVY_JOY_B9    (1u<<12)
#define GROOVY_JOY_B10   (1u<<13)

/* ---------------------------------------------------------------------------
 * Wire structures, packed to 1 byte -- field names and order EXACTLY match
 * the hardware receiver's own input header. The 9-byte message (dig_inputs) is a DISTINCT legitimate
 * case from the 17-byte message (inputs), not a truncation: the encoder can
 * legitimately emit 9 bytes when no active analog axis needs the 8 bytes of
 * axes.
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)

struct groovy_joy_dig_inputs {
    uint32_t frame;
    uint8_t  order;
    uint16_t joy1;
    uint16_t joy2;
};

struct groovy_joy_inputs {
    uint32_t frame;
    uint8_t  order;
    uint16_t joy1;
    uint16_t joy2;
    int8_t   joy1_lx, joy1_ly, joy1_rx, joy1_ry;
    int8_t   joy2_lx, joy2_ly, joy2_rx, joy2_ry;
};

#pragma pack(pop)

_Static_assert(sizeof(struct groovy_joy_dig_inputs) == 9,
    "upstream layout (hardware receiver's own input header) -- do not drift without rechecking");
_Static_assert(sizeof(struct groovy_joy_inputs) == 17,
    "upstream layout (hardware receiver's own input header) -- do not drift without rechecking");

/* ---------------------------------------------------------------------------
 * Channel API -- handshake, address learning, encoding, sending.
 * Opaque type, same pattern as gmc_t in gmclient.h.
 * ------------------------------------------------------------------------- */
typedef struct gmc_input gmc_input_t;

/* Binds INADDR_ANY:<video_port + 1>. NULL if the bind fails (not fatal for
 * the caller: the client must keep displaying without a gamepad). NEVER
 * hardcode this port here: it is already the VIDEO channel's test port in
 * all of this repo's tooling. */
gmc_input_t *gmc_input_open(unsigned video_port);

/* Drains the socket without blocking, at most 64 datagrams per call (same
 * hard bound as gm_poll_inputs on the emitter side: a flooding peer must not
 * lengthen the tick). Learns (or relearns) the emitter's address from the
 * first datagram of EXACTLY 1 byte in length; any other length is rejected
 * and counted (strict routing, never a tolerant parser). Returns 1 if a peer
 * is known after the call, 0 otherwise. Call once per tick. */
int gmc_input_poll(gmc_input_t *in);

/* Encodes and sends player 1's state to the EXACT address learned at
 * handshake time -- never a recomputed port. joy2 is always 0 (shared
 * player 1). axes == NULL -> 9-byte form; otherwise 17-byte form. Returns
 * the number of bytes sent, 0 if no peer is known yet (counted in
 * no_peer). */
size_t gmc_input_send(gmc_input_t *in, uint32_t frame, uint16_t joy1,
                      const int8_t axes[8]);

/* RetroPad -> Groovy bit table, a PURE function testable without libretro:
 * `get` is an accessor returning nonzero if button `id` (a
 * RETRO_DEVICE_ID_JOYPAD_* constant) is held. L3 (14) and R3 (15) always
 * return 0 -- no upstream source assigns them a bit, do not invent one. */
uint16_t gmc_input_joy_from_retropad(int (*get)(unsigned id));

/* Same table, from the mask of a SINGLE call to RETRO_DEVICE_ID_JOYPAD_MASK
 * (bit N = the RETRO_DEVICE_ID_JOYPAD_N constant -- libretro.h,
 * RETRO_ENVIRONMENT_GET_INPUT_BITMASKS). A PURE function, testable without
 * libretro: bit N of `mask` maps to the same local identifier as `get(N)` in
 * gmc_input_joy_from_retropad above -- same mapping, same absent bits for
 * L3/R3. Replaces, when the frontend accepts
 * RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, the fourteen individual calls to
 * input_state_cb with a single one. */
uint16_t gmc_input_joy_from_mask(uint16_t mask);

/* ===========================================================================
 * The client's receiving thread owns the input socket. retro_run makes NO
 * system call anymore.
 *
 * Why. Measured: gmc_input_poll called from retro_run accounted for 84 to
 * 96% of retro_run_work_us's p99 -- not because of its own cost (p50 = 10 us,
 * a single non-blocking system call per frame) but because a system call is
 * a PREEMPTION POINT: macOS can deschedule RetroArch's main thread there,
 * and the queue reached 1.5 ms. The fix is not to make it faster, it is to
 * move it off the thread that must hold 60 Hz.
 *
 * Split: the receiving thread receives (service_rx) and sends (flush);
 * retro_run only DEPOSITS the gamepad state (post) and asks whether a peer
 * is known (have_peer). Those two only take a lock.
 *
 * All functions below are safe across the two threads.
 * ========================================================================= */

/* Adds the input socket to a read set for select(). `readfds` is an
 * `fd_set *` -- passed as void* so that this header stays free of any
 * platform dependency. Returns the value to compare against select's nfds
 * (ignored on Windows), or -1 if there is no socket. */
int gmc_input_fdset_add(gmc_input_t *in, void *readfds);

/* True if the input socket is ready for reading in `readfds`. */
int gmc_input_fdset_isset(gmc_input_t *in, void *readfds);

/* Drains the input socket and learns (or relearns) the peer's address. Same
 * body and same bounds as gmc_input_poll -- at most GMC_INPUT_MAX_POLL
 * datagrams, handshake on the EXACT length of 1 byte. CALLED BY THE
 * RECEIVING THREAD ONLY. */
void gmc_input_service_rx(gmc_input_t *in);

/* Is a peer known? No system call: a lock and a read. This is what
 * retro_run checks before making the frontend's eighteen callbacks. */
int gmc_input_have_peer(const gmc_input_t *in);

/* Deposits the gamepad state into the mailbox. No system call. Overwrites
 * whatever was there: for a STATE (not an event), the latest known value is
 * the right one. Counts no_peer if no peer is known, exactly as
 * gmc_input_send used to from retro_run. axes == NULL -> 9-byte form.
 * CALLED BY retro_run ONLY. */
void gmc_input_post(gmc_input_t *in, uint32_t frame, uint16_t joy1,
                    const int8_t axes[8]);

/* Sends the mailbox if it has been deposited since the last send. Returns
 * the number of bytes sent, 0 if nothing was pending or no peer is known.
 * The receiving thread calls it on every loop iteration -- so on every
 * received video frame AND on select's timeout when nothing arrives --
 * which gives the mister driver the steady stream it expects, paced by
 * retro_run's deposits rather than by the loop itself. CALLED BY THE
 * RECEIVING THREAD ONLY. */
size_t gmc_input_flush(gmc_input_t *in);


/* Counters for the session JSON report. */
void gmc_input_stats(const gmc_input_t *in, unsigned *hello_seen,
                     unsigned long long *sent, unsigned long long *no_peer,
                     unsigned long long *rejected);

void gmc_input_close(gmc_input_t *in);

#ifdef __cplusplus
}
#endif
#endif /* GMCLIENT_INPUT_H */
