/* SPDX-License-Identifier: GPL-3.0-or-later */
/* loss_hint.h -- pure hysteresis detector over the continuation-escape counter
 * (gmc_cont_escape). The core speaks, it never gates anything -- this header
 * owns the decision alone, no I/O, no RetroArch dependency, testable on the
 * host.
 *
 * Calibration (bench measurements, 2026-09-07):
 *   - wired: 0 loss, so 0 escapes per window, never fires;
 *   - a Wi-Fi laptop follower: ~35 continuation escapes per 10 s window
 *     (5.9% of audio blocks broken);
 *   - a bench link-loss injection tool: ~one escape per audio block, i.e.
 *     continuation_escape == audio_blocks in that scenario (~600 per
 *     10 s window at 60 blocks/s).
 * Thresholds are provisional: confirmed at the bench against a real link by
 * reading last_window_escapes.
 */
#ifndef GMC_LOSS_HINT_H
#define GMC_LOSS_HINT_H

#include <stdint.h>

#define GMC_LOSS_HINT_ENTER 10u  /* escapes/window to declare the link lossy (>= 1/s) */
#define GMC_LOSS_HINT_EXIT  2u   /* escapes/window to declare it recovered */

struct gmc_loss_hint {
    uint64_t last_escapes;  /* escapes_total as of the previous window */
    uint64_t last_window;   /* escapes counted in the most recent window */
    uint64_t triggered;     /* number of ENTER edges over the session */
    int      armed;         /* 0 until the first call has set a reference point */
    int      active;        /* 1 while the link is currently judged lossy */
};

/* Called once per rate-judge window (GMC_RATE_WINDOW_NS) with the cumulative
 * escapes_total (gmc_cont_escape(client)). Returns 1 on the rising edge
 * (just became active, i.e. this is a new episode), 0 otherwise.
 *
 * First call arms the detector: it has no prior window to diff against, so
 * it only records a reference point and never fires. A counter that goes
 * backwards (a fresh client session against the same struct) is treated as a
 * window of zero escapes, never as a negative delta. */
static inline int gmc_loss_hint_window(struct gmc_loss_hint *h, uint64_t escapes_total)
{
    uint64_t d;

    if (!h->armed) {
        h->armed = 1;
        h->last_escapes = escapes_total;
        h->last_window = 0;
        return 0;
    }

    d = (escapes_total >= h->last_escapes) ? (escapes_total - h->last_escapes) : 0;
    h->last_escapes = escapes_total;
    h->last_window = d;

    if (!h->active && d >= GMC_LOSS_HINT_ENTER) {
        h->active = 1;
        h->triggered++;
        return 1;
    }
    if (h->active && d <= GMC_LOSS_HINT_EXIT)
        h->active = 0;

    return 0;
}

/* Which remedy the message names. audio=off only helps when
 * the emitter announced sound to this machine in its CMD_INIT; otherwise that advice
 * is already applied, and repeating it would send the peer down a false trail. */
enum gmc_loss_advice {
    GMC_LOSS_ADVICE_AUDIO_OFF = 0,   /* sound announced: removing it may shorten bursts */
    GMC_LOSS_ADVICE_LINK      = 1    /* no sound: the link itself is the problem */
};

static inline enum gmc_loss_advice gmc_loss_hint_advice(int audio_announced)
{
    return audio_announced ? GMC_LOSS_ADVICE_AUDIO_OFF : GMC_LOSS_ADVICE_LINK;
}

#endif /* GMC_LOSS_HINT_H */
