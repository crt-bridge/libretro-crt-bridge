/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_loss_hint.c -- proof of the pure hysteresis detector gmc_loss_hint_window.
 * Standalone: includes core/loss_hint.h only, no gmclient dependency, no
 * network. Every case is checked by hand (not assert()) so a failure names
 * the case on stderr and returns non-zero, instead of aborting silently on
 * whichever case happens to fail first.
 */
#include "core/loss_hint.h"

#include <stdio.h>

#define CHECK(cond, case_name)                                              \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "test_loss_hint: FAIL -- %s (%s)\n",            \
                    case_name, #cond);                                      \
            return 1;                                                       \
        }                                                                   \
    } while (0)

int main(void)
{
    struct gmc_loss_hint h;
    int r;

    h.last_escapes = 0;
    h.last_window = 0;
    h.triggered = 0;
    h.armed = 0;
    h.active = 0;

    /* Case 1: first call (unarmed) with 500 -> 0, becomes the reference, no
     * trigger. */
    r = gmc_loss_hint_window(&h, 500);
    CHECK(r == 0, "case 1: first call arms, no trigger");
    CHECK(h.armed == 1, "case 1: armed after first call");
    CHECK(h.triggered == 0, "case 1: triggered still 0");

    /* Case 2: 500 again (delta 0) -> 0. */
    r = gmc_loss_hint_window(&h, 500);
    CHECK(r == 0, "case 2: delta 0 -> no trigger");
    CHECK(h.active == 0, "case 2: not active");

    /* Case 3: 509 (delta 9, under ENTER=10) -> 0. */
    r = gmc_loss_hint_window(&h, 509);
    CHECK(r == 0, "case 3: delta 9 under ENTER -> no trigger");
    CHECK(h.active == 0, "case 3: not active");
    CHECK(h.last_window == 9, "case 3: last_window == 9");

    /* Case 4: 519 (delta 10, == ENTER) -> 1, rising edge, active, triggered == 1. */
    r = gmc_loss_hint_window(&h, 519);
    CHECK(r == 1, "case 4: delta 10 -> rising edge");
    CHECK(h.active == 1, "case 4: active == 1");
    CHECK(h.triggered == 1, "case 4: triggered == 1");

    /* Case 5: 600 (delta 81, already active) -> 0, triggered stays 1. */
    r = gmc_loss_hint_window(&h, 600);
    CHECK(r == 0, "case 5: already active -> no repeat trigger");
    CHECK(h.active == 1, "case 5: still active");
    CHECK(h.triggered == 1, "case 5: triggered unchanged");

    /* Case 6: 603 (delta 3, above EXIT=2) -> 0, stays active. */
    r = gmc_loss_hint_window(&h, 603);
    CHECK(r == 0, "case 6: delta 3 above EXIT -> stays active");
    CHECK(h.active == 1, "case 6: still active");

    /* Case 7: 605 (delta 2, == EXIT) -> 0, active becomes 0. */
    r = gmc_loss_hint_window(&h, 605);
    CHECK(r == 0, "case 7: delta 2 == EXIT -> no trigger");
    CHECK(h.active == 0, "case 7: active == 0");

    /* Case 8: 700 (delta 95) -> 1, triggered == 2, new episode. */
    r = gmc_loss_hint_window(&h, 700);
    CHECK(r == 1, "case 8: delta 95 -> second rising edge");
    CHECK(h.triggered == 2, "case 8: triggered == 2");

    /* Case 9: counter goes backwards (a fresh client session, 700 -> 4):
     * treated as a delta of 0, never a negative delta, no trigger. */
    r = gmc_loss_hint_window(&h, 4);
    CHECK(r == 0, "case 9: counter reset -> delta treated as 0, no trigger");
    CHECK(h.last_window == 0, "case 9: last_window == 0 on reset");

    /* Case 10: last_window always holds the most recent window's delta --
     * already checked at cases 3 and 9 above; one more direct check here.
     * last_escapes == 4 after case 9 (active == 0), so 10 is a delta of 6,
     * safely under ENTER == 10. */
    r = gmc_loss_hint_window(&h, 10);
    CHECK(r == 0, "case 10: delta 6 under ENTER");
    CHECK(h.last_window == 6, "case 10: last_window == 6");

    /* Case 11: which remedy the message names. The
     * audio=off advice only when the emitter announced sound to this machine;
     * without sound that advice is already applied, so the link is named. */
    CHECK(gmc_loss_hint_advice(1) == GMC_LOSS_ADVICE_AUDIO_OFF, "case 11: sound announced -> audio=off");
    CHECK(gmc_loss_hint_advice(0) == GMC_LOSS_ADVICE_LINK, "case 11: no sound -> the link itself");

    printf("test_loss_hint: OK\n");
    return 0;
}
