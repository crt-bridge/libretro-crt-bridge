/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_gmclient_padding.c -- client-side proof of the per-session-mode
 * length table. Same pattern as test_gmclient_audio.c: #define
 * GMCLIENT_TEST then #include "gmclient.c", which skips the socket/thread
 * creation inside gmc_open() (GMCLIENT_TEST guards that block) and leaves
 * the static functions (is_command_header, zero_tail,
 * handle_datagram_inner) directly callable.
 *
 * Links ONLY against lz4.o -- never against gmclient.o (see client/Makefile):
 * gmclient.c is included as-is, not compiled separately, so linking
 * gmclient.o as well would produce duplicate symbols.
 *
 * ============================================================================
 * EQUIVALENCE TABLE -- input (bytes) | expected on the hardware receiver's
 * own protocol test (its padded-session case) | expected on the client
 * (this file). Reviewed line by line against the receiver's protocol
 * implementation. NONE of the cases diverge on the inputs that are actually
 * shared between the two parsers.
 *
 *   input (bytes)                                     receiver       client
 *   -----------------------------------------------  -------------  -------------
 *   { 0x01 }                              len=1        accept(clas)   accept(clas)
 *   { 0x01, 0,0,0,0,0,0,0 }                len=8       accept(pad)    accept(pad)
 *   { 0x01 } in padded mode                             reject         reject
 *   { 0x01,0,0,0,0,0,0,0 } in classic mode               reject         reject
 *   { 0x01,0,0,0,0,0,0,0x01 } (dirty tail) len=8         reject         reject
 *   { 0x05 } / padded / dirty tail                       -- same as CMD_CLOSE, same table --
 *   { 0x08 } / padded / dirty tail                       -- same as CMD_CLOSE, same table --
 *   { 0x04,0x10,0x00 } len=3                            accept(clas)   accept(clas)
 *   { 0x04,0x10,0x00,0,0,0,0,0 } len=8                  accept(pad)    accept(pad)
 *   { 0x04,0x10,0x00,0,0,0,0,0x01 } (dirty tail)         reject         reject
 *   { 0x02,0,3,2 } len=4                    (both)       accept         accept
 *   { 0x02,0,3,2,0 } len=5                  (both)       accept         accept
 *   { 0x02,0,3,2,0,0,0,0 } len=8            (both)       accept         accept
 *   { 0x02,0,3,2,0,0,0,0x01 } (dirty tail)               reject         reject
 *   { 0x03,...} len=26                      (both)       accept         accept
 *   { 0x06,...} len=7                       (both)       accept         accept
 *   { 0x06,...} len=11                      (both)       accept         accept
 *   { 0x07,...} len=8                       (both)       accept         accept
 *   parse_cmd_init 3..9 (equivalent: is_command_header on 0x02, 3..9)
 *     len=3   reject         reject
 *     len=4   accept         accept
 *     len=5   accept         accept
 *     len=6   reject         reject
 *     len=7   reject         reject
 *     len=8 (nulled tail)     accept         accept
 *     len=8 (dirty tail)      reject         reject
 *     len=9   reject         reject
 *   audio escape in a padded session (end of block = audio, mid-block = header)
 *     -- same behaviour, checked via handle_datagram (side effect on
 *        cont_escape) on the client side (the receiver calls its own audio
 *        escape function directly; the client has no separate function,
 *        the same rule is INLINED in handle_datagram_inner -- see case 20h
 *        below)
 *
 * BOTH SUITES COUNTED: the receiver's own protocol test's padded-session
 * case returns 46 PASS, 0 FAIL on the inputs shared between the two
 * parsers. client/test_gmclient_padding.c (this file) returns 57 PASS, 0
 * FAIL -- 46 identical inputs + 11 documented additions (7 extra lengths
 * for 0x07, mode-independent for that opcode; 4 cases close to 20i/20j,
 * client-only, see below). NONE of the 46 shared inputs diverge. Proof that
 * the test can actually fail (project rule): the `if (len == 8u && ...)`
 * line in the CMD_INIT block of is_command_header (gmclient.c) was
 * temporarily broken into `if (padded && len == 8u && ...)`, which made
 * exactly two cases fail (header_init_8_both_modes_classic,
 * init_len8_ok_nulled_tail), exit code 1 -- then restored with `git checkout
 * -- client/gmclient.c`, never committed. Green was re-checked after the
 * restore.
 *
 * TWO CASES ARE CLIENT-ONLY, ABSENT FROM THE RECEIVER -- THIS IS NOT A
 * DIVERGENCE TO FIX: the receiver's own header check does not verify ANY
 * field bound on CMD_INIT (neither compression nor the rate code) -- only
 * its init-parsing routine checks compression <= 1, never the rate code.
 * The client, on the other hand, checks b[1] <= 1 AND b[2] <= 3, from a fix
 * that predates this test (93% of audio blocks lost, measured -- see the
 * comment on is_command_header in gmclient.c): this is a CLIENT-ONLY
 * invariant, stricter than the receiver, documented and intentional. This
 * test EXTENDS it to the 8-byte form without touching the 4/5-byte form.
 * Cases 20i/20j below EXERCISE this bound, CLIENT SIDE ONLY:
 *   { 0x02, 2, 3, 2, 0,0,0,0 }   b[1]=2 (compression out of bounds) -> reject (client)
 *                                 -- the receiver WOULD ACCEPT this same datagram
 *                                    (no bound on b[1] in its own header check)
 *   { 0x02, 0, 9, 2, 0,0,0,0 }   b[2]=9 (rate code out of bounds) -> reject (client)
 *                                 -- same remark: the receiver WOULD ACCEPT it
 * Documented here so a future reader does not "fix" either the client (by
 * removing the bound) or the receiver (by adding it without a deliberate
 * decision), believing they are closing a divergence.
 *
 * COPIES OF THIS RULE THAT LIVE OUTSIDE THE PROTOCOL PARSERS, AND WILL
 * THEREFORE DIVERGE -- a known fact, not an oversight: an emitter-side
 * relay script, the receiver's own mock-emitter tool, and a separate
 * measurement tool. Do NOT "fix" them in the belief that you are doing the
 * right thing. The measurement tool in particular is not a parser: its
 * divergence is intentional, it is what lets it count lengths without
 * judging them.
 * ============================================================================
 */
#define GMCLIENT_TEST
#include "gmclient.c"

#include <stdio.h>

static int g_fail = 0;

#define EXPECT(cond, label) do { \
    if (cond) printf("PASS: %s\n", label); \
    else { printf("FAIL: %s\n", label); g_fail++; } \
} while (0)

int main(void)
{
    /* ---- 20a : CMD_CLOSE (0x01) ---- */
    {
        uint8_t classic[1]    = { CMD_CLOSE };
        uint8_t padded[8]     = { CMD_CLOSE, 0,0,0,0,0,0,0 };
        uint8_t dirty_tail[8] = { CMD_CLOSE, 0,0,0,0,0,0,0x01u };

        EXPECT( is_command_header(classic, 1, 0, 0), "header_close_1_classic");
        EXPECT( is_command_header(padded, 8, 0, 1), "header_close_8_padded");
        EXPECT(!is_command_header(classic, 1, 0, 1), "header_close_1_rejected_in_padded");
        EXPECT(!is_command_header(padded, 8, 0, 0), "header_close_8_rejected_in_classic");
        EXPECT(!is_command_header(dirty_tail, 8, 0, 1), "header_close_8_dirty_tail_rejected");
    }

    /* ---- 20b : CMD_GET_STATUS (0x05) ---- */
    {
        uint8_t classic[1]    = { CMD_GET_STATUS };
        uint8_t padded[8]     = { CMD_GET_STATUS, 0,0,0,0,0,0,0 };
        uint8_t dirty_tail[8] = { CMD_GET_STATUS, 0,0,0,0,0,0x01u,0 };

        EXPECT( is_command_header(classic, 1, 0, 0), "header_get_status_1_classic");
        EXPECT( is_command_header(padded, 8, 0, 1), "header_get_status_8_padded");
        EXPECT(!is_command_header(classic, 1, 0, 1), "header_get_status_1_rejected_in_padded");
        EXPECT(!is_command_header(padded, 8, 0, 0), "header_get_status_8_rejected_in_classic");
        EXPECT(!is_command_header(dirty_tail, 8, 0, 1), "header_get_status_8_dirty_tail_rejected");
    }

    /* ---- 20c : CMD_GET_VERSION (0x08) ---- */
    {
        uint8_t classic[1]    = { CMD_GET_VERSION };
        uint8_t padded[8]     = { CMD_GET_VERSION, 0,0,0,0,0,0,0 };
        uint8_t dirty_tail[8] = { CMD_GET_VERSION, 0x01u,0,0,0,0,0,0 };

        EXPECT( is_command_header(classic, 1, 0, 0), "header_get_version_1_classic");
        EXPECT( is_command_header(padded, 8, 0, 1), "header_get_version_8_padded");
        EXPECT(!is_command_header(classic, 1, 0, 1), "header_get_version_1_rejected_in_padded");
        EXPECT(!is_command_header(padded, 8, 0, 0), "header_get_version_8_rejected_in_classic");
        EXPECT(!is_command_header(dirty_tail, 8, 0, 1), "header_get_version_8_dirty_tail_rejected");
    }

    /* ---- 20d : CMD_AUDIO (0x04) -- bytes 1-2 carry the size: only bytes
     * 3 to 7 of the padded form are required to be zero. ---- */
    {
        uint8_t classic[3]    = { CMD_AUDIO, 0x10, 0x00 };
        uint8_t padded[8]     = { CMD_AUDIO, 0x10, 0x00, 0,0,0,0,0 };
        uint8_t dirty_tail[8] = { CMD_AUDIO, 0x10, 0x00, 0,0,0,0,0x01u };

        EXPECT( is_command_header(classic, 3, 0, 0), "header_audio_3_classic");
        EXPECT( is_command_header(padded, 8, 0, 1), "header_audio_8_padded");
        EXPECT(!is_command_header(classic, 3, 0, 1), "header_audio_3_rejected_in_padded");
        EXPECT(!is_command_header(padded, 8, 0, 0), "header_audio_8_rejected_in_classic");
        EXPECT(!is_command_header(dirty_tail, 8, 0, 1), "header_audio_8_dirty_tail_rejected");
    }

    /* ---- 20e : CMD_INIT (0x02) -- accepted at length 4, 5 and 8 (nulled
     * tail) IN BOTH MODES. This is NOT a table indexed by mode. Fields
     * b[1]=0 (compression), b[2]=3 (rate), b[3]=2 (channels) -- always
     * within the client's bounds, to isolate what 20e tests (length and
     * tail) from what 20i/20j test (the bounds). ---- */
    {
        uint8_t len4[4]      = { CMD_INIT, 0, 3, 2 };
        uint8_t len5[5]      = { CMD_INIT, 0, 3, 2, 0 };
        uint8_t len8[8]      = { CMD_INIT, 0, 3, 2, 0,0,0,0 };
        uint8_t len8_sale[8] = { CMD_INIT, 0, 3, 2, 0,0,0,0x01u };

        EXPECT( is_command_header(len4, 4, 0, 0), "header_init_4_both_modes_classic");
        EXPECT( is_command_header(len4, 4, 0, 1), "header_init_4_both_modes_padded");
        EXPECT( is_command_header(len5, 5, 0, 0), "header_init_5_both_modes_classic");
        EXPECT( is_command_header(len5, 5, 0, 1), "header_init_5_both_modes_padded");
        EXPECT( is_command_header(len8, 8, 0, 0), "header_init_8_both_modes_classic");
        EXPECT( is_command_header(len8, 8, 0, 1), "header_init_8_both_modes_padded");
        EXPECT(!is_command_header(len8_sale, 8, 0, 0), "header_init_8_dirty_tail_rejected_classic");
        EXPECT(!is_command_header(len8_sale, 8, 0, 1), "header_init_8_dirty_tail_rejected_padded");
    }

    /* ---- 20f : opcodes whose length does NOT depend on the session mode
     * -- identical in both session states. 0x07 is exercised at its four
     * lengths (8/9/12/13) -- the receiver covers these elsewhere, outside
     * the "per mode" context, since these lengths don't depend on the
     * mode; grouped here so this file is a complete test PER OPCODE. ---- */
    {
        uint8_t sw26[26]   = { CMD_SWITCHRES };
        uint8_t bv7[7]     = { CMD_BLIT_VSYNC };
        uint8_t bv11[11]   = { CMD_BLIT_VSYNC };
        uint8_t bfv8[8]    = { CMD_BLIT_FIELD_VSYNC };
        uint8_t bfv9[9]    = { CMD_BLIT_FIELD_VSYNC, 0,0,0,0,0,0,0, 0x01u };  /* frame_dup */
        uint8_t bfv12[12]  = { CMD_BLIT_FIELD_VSYNC };
        uint8_t bfv13[13]  = { CMD_BLIT_FIELD_VSYNC };

        EXPECT(is_command_header(sw26, 26, 0, 0), "header_switchres_26_classic");
        EXPECT(is_command_header(sw26, 26, 0, 1), "header_switchres_26_padded");
        EXPECT(is_command_header(bv7, 7, 0, 0), "header_blit_7_classic");
        EXPECT(is_command_header(bv7, 7, 0, 1), "header_blit_7_padded");
        EXPECT(is_command_header(bv11, 11, 0, 0), "header_blit_11_classic");
        EXPECT(is_command_header(bv11, 11, 0, 1), "header_blit_11_padded");
        EXPECT(is_command_header(bfv8, 8, 0, 0), "header_blit_field_8_classic");
        EXPECT(is_command_header(bfv8, 8, 0, 1), "header_blit_field_8_padded");
        EXPECT(is_command_header(bfv9, 9, 0, 0), "header_blit_field_9_dup_classic");
        EXPECT(is_command_header(bfv9, 9, 1, 1), "header_blit_field_9_dup_padded");
        EXPECT(!is_command_header(bfv12, 12, 0, 0), "header_blit_field_12_rejected_without_compression");
        EXPECT( is_command_header(bfv12, 12, 1, 0), "header_blit_field_12_compressed_classic");
        EXPECT( is_command_header(bfv12, 12, 1, 1), "header_blit_field_12_compressed_padded");
        EXPECT( is_command_header(bfv13, 13, 1, 0), "header_blit_field_13_compressed_classic");
        EXPECT( is_command_header(bfv13, 13, 1, 1), "header_blit_field_13_compressed_padded");
    }

    /* ---- 20g : client-side equivalent of "parse_cmd_init on 3..9".
     * The client has no separate parse_cmd_init: is_command_header CARRIES
     * the same length/tail rule, on the same bytes. ---- */
    {
        uint8_t len3[3]      = { CMD_INIT, 0, 0 };
        uint8_t len4[4]      = { CMD_INIT, 0, 3, 2 };
        uint8_t len5[5]      = { CMD_INIT, 0, 3, 2, 0 };
        uint8_t len6[6]      = { CMD_INIT, 0, 3, 2, 0, 0 };
        uint8_t len7[7]      = { CMD_INIT, 0, 3, 2, 0, 0, 0 };
        uint8_t len8[8]      = { CMD_INIT, 0, 3, 2, 0, 0, 0, 0 };
        uint8_t len8_sale[8] = { CMD_INIT, 0, 3, 2, 0, 0, 0, 0x01u };
        uint8_t len9[9]      = { CMD_INIT, 0, 3, 2, 0, 0, 0, 0, 0 };

        EXPECT(!is_command_header(len3, 3, 0, 0), "init_len3_too_short");
        EXPECT( is_command_header(len4, 4, 0, 0), "init_len4_ok");
        EXPECT( is_command_header(len5, 5, 0, 0), "init_len5_ok");
        EXPECT(!is_command_header(len6, 6, 0, 0), "init_len6_too_short");
        EXPECT(!is_command_header(len7, 7, 0, 0), "init_len7_too_short");
        EXPECT( is_command_header(len8, 8, 0, 0), "init_len8_ok_nulled_tail");
        EXPECT(!is_command_header(len8_sale, 8, 0, 0), "init_len8_dirty_tail_rejected");
        EXPECT(!is_command_header(len9, 9, 0, 0), "init_len9_too_short");
    }

    /* ---- 20h : audio escape in a padded session -- an 8-byte header that
     * ENDS the block is audio; the same header that does not end it is a
     * header. The client has no separate escape function: the same rule
     * is INLINED in handle_datagram_inner ("is_payload"). Exercised here
     * through the real PUBLIC path (handle_datagram, the same function
     * rx_loop calls), observing its side effect (cont_escape) -- same
     * pattern as the escape case in test_gmclient_audio.c. ---- */
    {
        gmc_config cfg; gmc_config_defaults(&cfg);
        gmc_t *c = gmc_open(&cfg);
        uint8_t pad_close8[8] = { CMD_CLOSE, 0,0,0,0,0,0,0 };

        c->padded = 1;
        c->pend_kind = PEND_AUDIO; c->pend_expected = 100u; c->pend_got = 92u; /* 8 left, exact */
        unsigned long long esc_before = c->cont_escape;
        handle_datagram(c, pad_close8, 8, 0);
        EXPECT(c->cont_escape == esc_before, "escape_padded_end_of_block_is_audio");

        c->pend_kind = PEND_AUDIO; c->pend_expected = 100u; c->pend_got = 50u; /* 50 left, not 8 */
        esc_before = c->cont_escape;
        handle_datagram(c, pad_close8, 8, 0);
        EXPECT(c->cont_escape == esc_before + 1u, "escape_padded_mid_block_is_header");

        gmc_close(c);
    }

    /* ---- 20i / 20j : CMD_INIT field bounds, CLIENT-ONLY (see the
     * equivalence table at the top of the file -- the receiver does not
     * check either of these two bounds in its own header check). The
     * bound survives padding: it applies to the 8-byte form too. ---- */
    {
        uint8_t compression_hors_bornes[8] = { CMD_INIT, 2, 3, 2, 0,0,0,0 };
        uint8_t debit_hors_bornes[8]       = { CMD_INIT, 0, 9, 2, 0,0,0,0 };

        EXPECT(!is_command_header(compression_hors_bornes, 8, 0, 0),
               "init_8_compression_out_of_bounds_rejected_classic_CLIENT_ONLY");
        EXPECT(!is_command_header(compression_hors_bornes, 8, 0, 1),
               "init_8_compression_out_of_bounds_rejected_padded_CLIENT_ONLY");
        EXPECT(!is_command_header(debit_hors_bornes, 8, 0, 0),
               "init_8_rate_out_of_bounds_rejected_classic_CLIENT_ONLY");
        EXPECT(!is_command_header(debit_hors_bornes, 8, 0, 1),
               "init_8_rate_out_of_bounds_rejected_padded_CLIENT_ONLY");
    }

    printf("%s (test_gmclient_padding)\n", g_fail ? "SOME CASES FAILED" : "ALL CASES PASSED");
    return g_fail ? 1 : 0;
}
