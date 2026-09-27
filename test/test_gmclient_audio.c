/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_gmclient_audio.c -- proof of the resampler and the client-side PCM
 * ring. Same pattern as the hardware receiver's own input test:
 * #define GMCLIENT_TEST then #include "gmclient.c", which skips the
 * socket/thread creation inside gmc_open() (GMCLIENT_TEST guards that
 * block, no network dependency here) and leaves the static functions
 * (finish_audio, gmc_audio_read, the PCM ring) directly callable on a real
 * gmc_t, single-threaded.
 *
 * Links ONLY against lz4.o -- never against gmclient.o (see client/Makefile):
 * gmclient.c is included as-is, not compiled separately, so linking
 * gmclient.o as well would produce duplicate symbols.
 */
#define GMCLIENT_TEST
#include "gmclient.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deterministic pattern, never silence: silence would stay identical even
 * after a buggy resampling that only duplicated zeros. */
static void fill_pattern(int16_t *buf, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        buf[i * 2u]      = (int16_t)((i * 37u) & 0x7FFFu);
        buf[i * 2u + 1u] = (int16_t)(~(i * 53u) & 0x7FFFu);
    }
}

/* --- Case 1: identity (src == dst == 48000) ------------------------------------ */
static void test_identity(void)
{
    struct groovy_resampler r;
    int16_t in[2 * 800], out[2 * 800];
    fill_pattern(in, 800);
    groovy_resample_reset(&r, 48000u, 48000u);
    size_t got = groovy_resample(&r, in, 800, out, 800);
    assert(got == 800u);
    assert(memcmp(in, out, sizeof in) == 0);
    printf("Case 1 PASS: identity 48000->48000, 800 pairs, byte for byte\n");
}

/* --- Case 2: 44100 -> 48000 ----------------------------------------------------- */
static void test_44100(void)
{
    struct groovy_resampler r;
    int16_t in[2 * 4410];
    fill_pattern(in, 4410);
    groovy_resample_reset(&r, 44100u, 48000u);
    size_t cap = groovy_resample_capacity(4410, 44100u, 48000u);
    int16_t *out = (int16_t *)malloc(cap * 2u * sizeof(int16_t));
    assert(out != NULL);
    size_t got = groovy_resample(&r, in, 4410, out, cap);
    assert(got >= 4798u && got <= 4802u);
    free(out);
    printf("Case 2 PASS: 44100->48000, 4410 input pairs, %zu output (4800 +/- 2)\n", got);
}

/* --- Case 3: 22050 -> 48000 ----------------------------------------------------- */
static void test_22050(void)
{
    struct groovy_resampler r;
    int16_t in[2 * 2205];
    fill_pattern(in, 2205);
    groovy_resample_reset(&r, 22050u, 48000u);
    size_t cap = groovy_resample_capacity(2205, 22050u, 48000u);
    int16_t *out = (int16_t *)malloc(cap * 2u * sizeof(int16_t));
    assert(out != NULL);
    size_t got = groovy_resample(&r, in, 2205, out, cap);
    assert(got >= 4798u && got <= 4802u);
    free(out);
    printf("Case 3 PASS: 22050->48000, 2205 input pairs, %zu output (4800 +/- 2)\n", got);
}

/* --- Case 4: phase carry across blocks ---------------------------------------- */
static void test_phase_carry(void)
{
    int16_t in[2 * 4410];
    fill_pattern(in, 4410);

    struct groovy_resampler r1;
    groovy_resample_reset(&r1, 44100u, 48000u);
    size_t cap1 = groovy_resample_capacity(4410, 44100u, 48000u);
    int16_t *out1 = (int16_t *)malloc(cap1 * 2u * sizeof(int16_t));
    assert(out1 != NULL);
    size_t got1 = groovy_resample(&r1, in, 4410, out1, cap1);
    free(out1);

    struct groovy_resampler r2;
    groovy_resample_reset(&r2, 44100u, 48000u);
    size_t cap_block = groovy_resample_capacity(441, 44100u, 48000u);
    int16_t *outblk = (int16_t *)malloc(cap_block * 2u * sizeof(int16_t));
    assert(outblk != NULL);
    size_t got2 = 0u;
    for (int b = 0; b < 10; b++) {
        size_t g = groovy_resample(&r2, in + (size_t)b * 441u * 2u, 441, outblk, cap_block);
        got2 += g;
    }
    free(outblk);

    long diff = (long)got1 - (long)got2;
    if (diff < 0) diff = -diff;
    assert(diff <= 2);
    printf("Case 4 PASS: phase carry, single block %zu vs ten blocks %zu (diff %ld <= 2)\n",
           got1, got2, diff);
}

/* --- Case 5: mono -> stereo, through the real CMD_INIT/CMD_AUDIO path ---------- */
static void test_mono_to_stereo(void)
{
    gmc_config cfg; gmc_config_defaults(&cfg);
    gmc_t *c = gmc_open(&cfg);
    assert(c != NULL);

    /* CMD_INIT: compression 0, rate code 3 (48000 Hz), 1 channel (mono) */
    uint8_t init_b[4] = { CMD_INIT, 0, 3, 1 };
    handle_header(c, init_b, 4);
    assert(c->audio_src_hz == 48000u);
    assert(c->audio_channels == 1u);

    /* CMD_AUDIO: block of 800 mono samples (1600 bytes), non-zero pattern */
    const unsigned nsamp = 800u;
    const uint16_t nbytes = (uint16_t)(nsamp * 2u);
    uint8_t hdr[3] = { CMD_AUDIO, (uint8_t)(nbytes & 0xFFu), (uint8_t)((nbytes >> 8) & 0xFFu) };
    uint8_t payload[1600];
    for (unsigned i = 0; i < nsamp; i++) {
        const int16_t s = (int16_t)((i * 91u) & 0x7FFFu);
        payload[i * 2u]      = (uint8_t)(s & 0xFFu);
        payload[i * 2u + 1u] = (uint8_t)((s >> 8) & 0xFFu);
    }

    /* first block: 800 pairs are not enough for the pre-roll (1600 are needed) */
    handle_datagram(c, hdr, 3, 0);
    assert(c->pend_kind == PEND_AUDIO);
    handle_datagram(c, payload, sizeof payload, 0);
    assert(c->pend_kind == PEND_NONE);

    int16_t pcm[2 * 1600];
    size_t got = gmc_audio_read(c, pcm, 1600);
    assert(got == 0u);

    /* a second, identical block completes the pre-roll (800 + 800 = 1600) */
    handle_datagram(c, hdr, 3, 0);
    handle_datagram(c, payload, sizeof payload, 0);
    got = gmc_audio_read(c, pcm, 1600);
    assert(got > 0u);
    for (size_t i = 0; i < got; i++)
        assert(pcm[i * 2u] == pcm[i * 2u + 1u]);   /* both channels equal (mono duplication) */

    gmc_close(c);
    printf("Case 5 PASS: mono->stereo, %zu pairs read, left/right channels equal\n", got);
}

/* --- Case 6: bounded ring overflow ------------------------ */
static void test_ring_overflow(void)
{
    gmc_config cfg; gmc_config_defaults(&cfg);
    gmc_t *c = gmc_open(&cfg);
    assert(c != NULL);

    /* CMD_INIT: 48000 Hz, stereo -- 1:1 ratio, exact count with no rounding */
    uint8_t init_b[4] = { CMD_INIT, 0, 3, 2 };
    handle_header(c, init_b, 4);
    assert(c->audio_src_hz == 48000u);

    const size_t frames_per_block = 2000u;   /* <= GMC_AUDIO_SCRATCH_CAP, <= GMC_AUDIO_PEND_CAP/4 */
    const int nblocks = 10;                  /* 10 * 2000 = 20000 pairs pushed */
    size_t total_pushed = 0u;
    for (int b = 0; b < nblocks; b++) {
        for (size_t i = 0; i < frames_per_block; i++) {
            const int16_t l = (int16_t)((i * 7u) & 0x7FFFu);
            const int16_t r = (int16_t)(~(i * 11u) & 0x7FFFu);
            c->apend_buf[i * 4u]      = (uint8_t)(l & 0xFFu);
            c->apend_buf[i * 4u + 1u] = (uint8_t)((l >> 8) & 0xFFu);
            c->apend_buf[i * 4u + 2u] = (uint8_t)(r & 0xFFu);
            c->apend_buf[i * 4u + 3u] = (uint8_t)((r >> 8) & 0xFFu);
        }
        c->pend_got = frames_per_block * 4u;
        finish_audio(c);
        total_pushed += frames_per_block;
        assert(c->acount <= c->acap);   /* never past capacity, at every step */
    }
    assert(total_pushed == 20000u);
    assert(c->acount == c->acap);
    assert(c->audio_overruns == (uint64_t)(total_pushed - c->acap));

    printf("Case 6 PASS: %zu pairs pushed into a ring of %u, overruns=%llu, occupancy=%u\n",
           total_pushed, c->acap, (unsigned long long)c->audio_overruns, c->acount);
    gmc_close(c);
}

/* --- Case 7: starvation, after a complete pre-roll ----------------------------- */
static void test_starvation(void)
{
    gmc_config cfg; gmc_config_defaults(&cfg);
    gmc_t *c = gmc_open(&cfg);
    assert(c != NULL);

    uint8_t init_b[4] = { CMD_INIT, 0, 3, 2 };
    handle_header(c, init_b, 4);

    /* push exactly the pre-roll threshold (1600 pairs), 1:1 ratio */
    for (size_t i = 0; i < 1600u; i++) {
        c->apend_buf[i * 4u]      = 0;
        c->apend_buf[i * 4u + 1u] = 0;
        c->apend_buf[i * 4u + 2u] = 0;
        c->apend_buf[i * 4u + 3u] = 0;
    }
    c->pend_got = 1600u * 4u;
    finish_audio(c);
    assert(c->acount == 1600u);

    int16_t pcm[2 * 1600];
    size_t got = gmc_audio_read(c, pcm, 1600);
    assert(got == 1600u);          /* past the pre-roll, the ring empties completely */
    assert(c->acount == 0u);

    /* the ring is now dry: return 0, write nothing, count one starvation */
    memset(pcm, 0x5A, sizeof pcm);
    got = gmc_audio_read(c, pcm, 8);
    assert(got == 0u);
    for (size_t i = 0; i < 16; i++) assert(((uint8_t *)pcm)[i] == 0x5Au);
    assert(c->audio_underruns == 1u);

    gmc_close(c);
    printf("Case 7 PASS: starvation after pre-roll, pcm untouched, audio_underruns=1\n");
}

/* --- Case 8: a PCM tail that aliases a CMD_INIT does not break the block -------- */
/* Reproduces a real defect: gm_send_audio sends raw PCM with no opcode
 * prefix, in MTU-sized chunks. A 4-byte tail chunk starting with 0x02
 * satisfies is_command_header (CMD_INIT, len 4). Before the fix,
 * handle_datagram dropped it (continuation_escape) AND treated it as a
 * fake CMD_INIT, resetting audio_src_hz to 0 (unknown rate code). Here: a
 * 24-byte block = 20 (normal payload) + 4 (tail { 0x02, 0, 0, 0 }, exactly
 * the remainder). The rule mirrors the hardware receiver's own audio
 * escape check and must keep it as payload: complete block, audio_src_hz
 * intact, no escape, a single INIT. */
static void test_queue_alias_init(void)
{
    gmc_config cfg; gmc_config_defaults(&cfg);
    gmc_t *c = gmc_open(&cfg);
    assert(c != NULL);

    /* Real CMD_INIT: compression 0, 44100 Hz (code 2), stereo (2 channels) */
    uint8_t init_b[4] = { CMD_INIT, 0, 2, 2 };
    handle_header(c, init_b, 4);
    assert(c->audio_src_hz == 44100u);
    assert(c->compressed == 0);
    const unsigned long long inits_before = c->inits;
    const unsigned long long escapes_before = c->cont_escape;

    /* CMD_AUDIO announcing 24 bytes, then 20 bytes of payload + 4 of tail */
    uint8_t hdr[3] = { CMD_AUDIO, 24u, 0u };
    handle_datagram(c, hdr, 3, 0);
    assert(c->pend_kind == PEND_AUDIO);
    assert(c->pend_expected == 24u);

    uint8_t chunk1[20];
    for (unsigned i = 0; i < sizeof chunk1; i++) chunk1[i] = (uint8_t)(0xA0u + i);  /* never an opcode */
    handle_datagram(c, chunk1, sizeof chunk1, 0);
    assert(c->pend_got == 20u);

    /* the 4-byte tail aliases a CMD_INIT (len 4), but == the exact remainder (4) */
    uint8_t tail[4] = { CMD_INIT, 0u, 0u, 0u };
    handle_datagram(c, tail, sizeof tail, 0);

    /* the block must have completed cleanly, with no fake INIT and no escape */
    assert(c->pend_kind == PEND_NONE);
    assert(c->audio_src_hz == 44100u);     /* NOT corrupted to 0 by a fake CMD_INIT */
    assert(c->compressed == 0);            /* NOT overwritten by the tail { 0x02, 0, ... } */
    assert(c->inits == inits_before);       /* no INIT manufactured out of the PCM */
    assert(c->cont_escape == escapes_before); /* no audio block dropped */

    gmc_close(c);
    printf("Case 8 PASS: PCM tail aliasing CMD_INIT kept as payload, audio_src_hz intact\n");
}

/* --- Case 9: a CMD_INIT with implausible fields (born from PCM) is rejected ------ */
/* A second trigger for the same defect, seen on a real Wi-Fi link: when a
 * chunk from the MIDDLE of an audio block is lost, the tail no longer has
 * the expected size, so the len==remainder rule (case 8) does not catch
 * it, and a tail { 0x02, ... } becomes a fake CMD_INIT again. Just ONE is
 * enough to freeze audio_src_hz at 0 for the rest of the run
 * (finish_audio bails out if src_hz==0). Mirrors the hardware receiver's
 * own fix (it rejects an invalid compression value): is_command_header
 * only accepts a CMD_INIT if compression <= 1 AND the rate code <= 3 --
 * which a real INIT always respects (gm_send_init: compression 0/1, code
 * 0..3). The fake INIT { 0x02, 217, 204, 232 } actually observed is then
 * noise, not a command. */
static void test_faux_init_rejete(void)
{
    gmc_config cfg; gmc_config_defaults(&cfg);
    gmc_t *c = gmc_open(&cfg);
    assert(c != NULL);

    /* real INIT: 44100 Hz stereo */
    uint8_t init_b[4] = { CMD_INIT, 0, 2, 2 };
    handle_header(c, init_b, 4);
    assert(c->audio_src_hz == 44100u);
    const unsigned long long inits_before = c->inits;

    /* the fake INIT actually measured on a real link: compression 217, rate 204 */
    uint8_t fake[4] = { CMD_INIT, 217u, 204u, 232u };
    handle_datagram(c, fake, sizeof fake, 0);   /* outside any block in progress */
    assert(c->audio_src_hz == 44100u);          /* NOT reset to 0 by the fake INIT */
    assert(c->inits == inits_before);            /* no INIT manufactured */

    /* plausible compression (0) but rate out of range (7): rejected too */
    uint8_t faux2[4] = { CMD_INIT, 0u, 7u, 2u };
    handle_datagram(c, faux2, sizeof faux2, 0);
    assert(c->audio_src_hz == 44100u);
    assert(c->inits == inits_before);

    /* a real, plausible INIT always goes through (48000 Hz) */
    uint8_t vrai2[4] = { CMD_INIT, 0u, 3u, 2u };
    handle_datagram(c, vrai2, sizeof vrai2, 0);
    assert(c->audio_src_hz == 48000u);
    assert(c->inits == inits_before + 1u);

    gmc_close(c);
    printf("Case 9 PASS: fake CMD_INIT (implausible compression/rate) rejected, src_hz intact\n");
}


/* Case 10 -- the contract behind the fix: gmc_audio_read NEVER returns more
 * than what it is asked for, so a well-filled ring drains in whole ticks,
 * one per call. Before the fix, the core requested 4096 frames at once
 * (half the ring, 85 ms of sound), which blocked the frontend under
 * audio_sync = true. This test locks the bound on the library side; it
 * would fail if gmc_audio_read went back to pouring out more than
 * max_frames. */
static void test_lecture_par_tick(void)
{
    gmc_config cfg; gmc_config_defaults(&cfg);
    gmc_t *c = gmc_open(&cfg);
    assert(c != NULL);

    uint8_t init_b[4] = { CMD_INIT, 0, 3, 2 };   /* 48000 Hz, stereo, 1:1 ratio */
    handle_header(c, init_b, 4);
    assert(c->audio_src_hz == 48000u);

    /* 4000 pairs in the ring: well beyond a tick, well beyond the pre-roll.
     * Pushed in TWO blocks of 2000: apend_buf is GMC_AUDIO_PEND_CAP = 8192
     * bytes, i.e. 2048 stereo pairs at most -- writing 4000 pairs at once
     * corrupts the heap (found while writing this test). Same bound as
     * test_ring_overflow. */
    const size_t par_bloc = 2000u;
    const size_t total = 2u * par_bloc;
    for (int b = 0; b < 2; b++) {
        for (size_t i = 0; i < par_bloc; i++) {
            const int16_t v = (int16_t)(((i + (size_t)b * par_bloc) * 3u) & 0x7FFFu);
            c->apend_buf[i * 4u]      = (uint8_t)(v & 0xFFu);
            c->apend_buf[i * 4u + 1u] = (uint8_t)((v >> 8) & 0xFFu);
            c->apend_buf[i * 4u + 2u] = (uint8_t)(v & 0xFFu);
            c->apend_buf[i * 4u + 3u] = (uint8_t)((v >> 8) & 0xFFu);
        }
        c->pend_got = par_bloc * 4u;
        finish_audio(c);
    }
    assert(c->acount == (unsigned)total);

    const size_t TICK = 800u;
    static int16_t pcm[2 * 800];
    size_t lus = 0u;
    int calls = 0;
    for (;;) {
        const size_t n = gmc_audio_read(c, pcm, TICK);
        if (n == 0u) break;
        assert(n <= TICK);            /* THE bound: never more than asked for */
        lus += n; calls++;
        if (calls > 10) break;        /* safety net: 4000 / 800 = 5 full calls */
    }
    /* five full draws, and not one more: 5 x 800 = 4000 */
    assert(calls == 5);
    assert(lus == total);
    assert(c->acount == 0u);
    assert(c->audio_frames_out == (unsigned long long)total);

    /* the empty ring returns zero, and the core will fill in with silence -- it does not block */
    assert(gmc_audio_read(c, pcm, TICK) == 0u);

    gmc_close(c);
    printf("Case 10 PASS: read by tick, 4000 pairs drained in 5 draws of 800, never more\n");
}

/* --- Case 11: the escape is split by the kind of block that was cut ------------ */
/* cont_escape used to count every interrupted block, video included, and
 * the core would advise audio=off even to a follower with no sound. The
 * counter is now split by pend_kind at the time of the escape (cont_escape
 * stays their sum), and gmc_audio_announced says whether the last
 * CMD_INIT announced sound. */
static void test_echappement_ventile(void)
{
    gmc_config cfg; gmc_config_defaults(&cfg);
    gmc_t *c = gmc_open(&cfg);
    assert(c != NULL);

    uint8_t init_b[4] = { CMD_INIT, 0, 2, 2 };   /* 44100 Hz stereo: sound announced */
    handle_header(c, init_b, 4);
    assert(gmc_audio_announced(c) == 1);

    /* 24-byte audio block cut after 10: the following header interrupts it */
    uint8_t hdr[3] = { CMD_AUDIO, 24u, 0u };
    uint8_t chunk[10];
    for (unsigned i = 0; i < sizeof chunk; i++) chunk[i] = (uint8_t)(0xA0u + i);  /* never an opcode */
    handle_datagram(c, hdr, 3, 0);
    handle_datagram(c, chunk, sizeof chunk, 0);
    handle_datagram(c, hdr, 3, 0);
    assert(c->cont_escape_audio == 1u);
    assert(c->cont_escape_video == 0u);

    /* video block cut: receive state set directly (same pattern as case 10) */
    c->pend_kind = PEND_VIDEO; c->pend_expected = 1000u; c->pend_got = 100u;
    handle_datagram(c, hdr, 3, 0);
    assert(c->cont_escape_video == 1u);
    assert(c->cont_escape_audio == 1u);
    assert(c->cont_escape == c->cont_escape_audio + c->cont_escape_video);

    /* a CMD_INIT with no sound withdraws the announcement */
    uint8_t init_muet[4] = { CMD_INIT, 0, 0, 0 };
    handle_header(c, init_muet, 4);
    assert(gmc_audio_announced(c) == 0);

    printf("Case 11 PASS: split escapes audio=%llu video=%llu sum=%llu, sound announcement tracked\n",
           (unsigned long long)c->cont_escape_audio, (unsigned long long)c->cont_escape_video,
           (unsigned long long)c->cont_escape);
    gmc_close(c);
}

/* Case 12 -- the CMD_INIT rate table now lives in gmclient itself (no more
 * groovy_audio.h include): every code the wire can carry, mapped once. */
static void test_rate_table_matches_cmd_init_codes(void)
{
    assert(groovy_audio_rate_hz(0) == 0u);
    assert(groovy_audio_rate_hz(1) == 22050u);
    assert(groovy_audio_rate_hz(2) == 44100u);
    assert(groovy_audio_rate_hz(3) == 48000u);
    assert(groovy_audio_rate_hz(4) == 0u);
    assert(groovy_audio_rate_hz(255) == 0u);
    printf("Case 12 PASS: CMD_INIT rate table (0,1,2,3 -> 0,22050,44100,48000), other codes -> 0\n");
}

int main(void)
{
    test_identity();
    test_44100();
    test_22050();
    test_phase_carry();
    test_mono_to_stereo();
    test_ring_overflow();
    test_starvation();
    test_queue_alias_init();
    test_faux_init_rejete();
    test_lecture_par_tick();
    test_echappement_ventile();
    test_rate_table_matches_cmd_init_codes();
    printf("ALL CASES PASSED (test_gmclient_audio)\n");
    return 0;
}
