/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_gmclient_input.c -- proof of the Groovy input channel, client side.
 * Same pattern as the hardware receiver's own input test: #include
 * "gmclient_input.c" then assert(), no hardware dependency for cases 1 to 5
 * -- case 6 (handshake in a local loop) opens a real loopback UDP socket.
 *
 * Test video port: 32104 (so it listens on 32105) -- deliberately different
 * from the two ports already taken by the existing bench suites (the test
 * video channel and the production input channel).
 */
#include "gmclient_input.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
static void test_sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void test_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

/* --- Test RetroPad accessor (case 5): only one id "held" at a time --- */
static unsigned g_test_pressed_id = 0xFFFFFFFFu;
static int test_get_pad(unsigned id) { return id == g_test_pressed_id; }


/* ---------------------------------------------------------------------------
 * Cases 7 and 8: the receiving thread owns the input socket, retro_run makes
 * no system call anymore. Both cases run a REAL second thread, because that
 * is exactly what the port introduces: state shared between the receiving
 * thread and retro_run.
 *
 * Test video port 32106 (so it listens on 32107) -- distinct from case 6's
 * 32104/32105, so the two cases never fight over the same socket if one
 * leaves a datagram in flight.
 * ------------------------------------------------------------------------- */
#ifdef _WIN32
#  include <process.h>
typedef HANDLE test_thread_t;
static unsigned __stdcall test_rx_body(void *arg);
static void test_thread_start(test_thread_t *t, void *arg)
{ *t = (HANDLE)_beginthreadex(NULL, 0, test_rx_body, arg, 0, NULL); assert(*t != NULL); }
static void test_thread_join(test_thread_t t)
{ WaitForSingleObject(t, 5000); CloseHandle(t); }
#else
#  include <pthread.h>
typedef pthread_t test_thread_t;
static void *test_rx_body(void *arg);
static void test_thread_start(test_thread_t *t, void *arg)
{ assert(pthread_create(t, NULL, test_rx_body, arg) == 0); }
static void test_thread_join(test_thread_t t) { pthread_join(t, NULL); }
#endif

/* The fake receiving thread: exactly what rx_loop does in gmclient.c -- it
 * SERVICES the input socket, and it empties the mailbox. Nothing else. */
static gmc_input_t *g_rx_chan;
static volatile int g_rx_stop;
static volatile unsigned long long g_rx_flushed;   /* bytes sent by the fake thread */

#ifdef _WIN32
static unsigned __stdcall test_rx_body(void *arg)
#else
static void *test_rx_body(void *arg)
#endif
{
    (void)arg;
    while (!g_rx_stop) {
        fd_set rf;
        struct timeval tv;
        int nfds;
        FD_ZERO(&rf);
        nfds = gmc_input_fdset_add(g_rx_chan, &rf);
        tv.tv_sec = 0; tv.tv_usec = 2000;
        if (select(nfds + 1, &rf, NULL, NULL, &tv) > 0
            && gmc_input_fdset_isset(g_rx_chan, &rf))
            gmc_input_service_rx(g_rx_chan);
        g_rx_flushed += (unsigned long long)gmc_input_flush(g_rx_chan);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_two_threads_handshake_and_mailbox(void)
{
    gmc_input_t *in;
    struct sockaddr_in dst;
    test_thread_t th;
    unsigned hs; unsigned long long sent, no_peer, rejected;
    uint8_t rx[64];
    int i, rc;
#ifdef _WIN32
    SOCKET tsock; DWORD tmo = 500;
#else
    int tsock; struct timeval tmo;
#endif

    in = gmc_input_open(32106u);
    assert(in != NULL);

    /* --- before any handshake: no peer, and a deposit does not go out ------- */
    assert(gmc_input_have_peer(in) == 0);
    gmc_input_post(in, 1u, GROOVY_JOY_UP, NULL);
    gmc_input_stats(in, &hs, &sent, &no_peer, &rejected);
    assert(no_peer == 1ull);           /* same accounting as before */
    assert(sent == 0ull);
    assert(gmc_input_flush(in) == 0u); /* nothing to send without a peer */

    /* --- start the fake receiving thread, then handshake -------------------- */
    g_rx_chan = in; g_rx_stop = 0; g_rx_flushed = 0;
    test_thread_start(&th, NULL);

#ifdef _WIN32
    tsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(tsock != INVALID_SOCKET);
    setsockopt(tsock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
#else
    tsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(tsock >= 0);
    tmo.tv_sec = 0; tmo.tv_usec = 500000;
    setsockopt(tsock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
#endif
    memset(&dst, 0, sizeof dst);
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(32107);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    {   /* the handshake byte, received by the RECEIVING THREAD and it alone */
        uint8_t hello = 0u;
        rc = (int)sendto(tsock, (const char *)&hello, 1u, 0,
                         (struct sockaddr *)&dst, sizeof dst);
        assert(rc == 1);
    }
    for (i = 0; i < 200 && !gmc_input_have_peer(in); i++) test_sleep_ms(5);
    assert(gmc_input_have_peer(in) == 1);   /* CASE 7: the handshake crossed both threads */
    gmc_input_stats(in, &hs, &sent, &no_peer, &rejected);
    assert(hs == 1u);

    /* --- CASE 8: the mailbox. Two deposits before the thread sends: a
     * STATE, not a queue -- the last one overwrites the previous one, and it
     * is indeed the LAST one that goes out. Depositing both back-to-back
     * maximizes the chance that the thread has not interleaved; if it did,
     * the read loop below will see two datagrams and keep the last one. --- */
    {
        int8_t axes[8];
        memset(axes, 0, sizeof axes);
        axes[0] = 42; axes[1] = -7;
        gmc_input_post(in, 100u, GROOVY_JOY_LEFT, NULL);
        gmc_input_post(in, 101u, GROOVY_JOY_RIGHT, axes);
    }
    {   /* read everything that arrives for 1 s, and keep the last one */
        int got = 0, last_len = 0;
        uint8_t last[64];
        for (i = 0; i < 200; i++) {
            rc = (int)recvfrom(tsock, (char *)rx, sizeof rx, 0, NULL, NULL);
            if (rc > 0) { got++; last_len = rc; memcpy(last, rx, (size_t)rc); }
            else if (got) break;
            test_sleep_ms(5);
            if (got && i > 20) break;
        }
        assert(got >= 1);            /* the receiving thread did send */
        assert(last_len == 17);      /* 17-byte form: the last deposit carried axes */
        /* frame == 101 up front, little-endian, as groovy_input_pack_joy writes it */
        assert(last[0] == 101u && last[1] == 0u && last[2] == 0u && last[3] == 0u);
        assert(last[last_len - 8] == 42);        /* axes[0] */
        assert((int8_t)last[last_len - 7] == -7); /* axes[1] */
    }

    /* --- an emptied mailbox returns nothing: flush is idempotent ------------ */
    g_rx_stop = 1;
    test_thread_join(th);
    assert(gmc_input_flush(in) == 0u);

    gmc_input_stats(in, &hs, &sent, &no_peer, &rejected);
    assert(sent >= 1ull);

#ifdef _WIN32
    closesocket(tsock);
#else
    close(tsock);
#endif
    gmc_input_close(in);
    printf("Case 7 PASS: handshake received by the receiving thread, seen by the other thread\n");
    printf("Case 8 PASS: mailbox -- deposit with no system call, sent by the thread, last state wins\n");
}

int main(void)
{
    uint8_t buf[64];

    /* --- Case 1: sizes -- redundant with gmclient_input.h's
     * _Static_assert, and that is deliberate: the test fails with a
     * readable message, the assert fails by breaking the build. --- */
    assert(sizeof(struct groovy_joy_dig_inputs) == 9);
    assert(sizeof(struct groovy_joy_inputs) == 17);
    printf("Case 1 PASS: sizes 9 and 17 confirmed\n");

    /* --- Case 2: exact byte image, 9-byte form -- same value and same
     * expected result as the hardware receiver's own 9-byte
     * groovy_input_pack_joy case, to stay bit-identical to that earlier proof. --- */
    {
        size_t written;
        memset(buf, 0xAA, sizeof buf);
        written = groovy_input_pack_joy(buf, 9, 0x11223344u, 0x55, 0x0009, 0x0000, NULL, 0);
        assert(written == 9);
        assert(buf[0] == 0x44); assert(buf[1] == 0x33);
        assert(buf[2] == 0x22); assert(buf[3] == 0x11);
        assert(buf[4] == 0x55);
        assert(buf[5] == 0x09); assert(buf[6] == 0x00);
        assert(buf[7] == 0x00); assert(buf[8] == 0x00);
        assert(buf[9] == 0xAA);
        printf("Case 2 PASS: 9-byte image matches (hardware receiver's own test)\n");
    }

    /* --- Case 3: exact byte image, 17-byte form --- */
    {
        int8_t axes[8] = { 1, -1, 127, -128, 0, 0, 0, 0 };
        size_t written;
        memset(buf, 0xAA, sizeof buf);
        written = groovy_input_pack_joy(buf, 17, 0x11223344u, 0x55, 0x0009, 0x0000, axes, 1);
        assert(written == 17);
        assert(buf[9] == 0x01);
        assert((int8_t)buf[10] == -1);
        assert(buf[11] == 0x7F);
        assert((int8_t)buf[12] == -128);
        assert(buf[17] == 0xAA);
        printf("Case 3 PASS: 17-byte image conforms\n");
    }

    /* --- Case 4: clean refusal under insufficient capacity -- never a
     * partial write. --- */
    {
        size_t written;
        memset(buf, 0xAA, sizeof buf);
        written = groovy_input_pack_joy(buf, 8, 0x11223344u, 0x55, 0x0009, 0x0000, NULL, 0);
        assert(written == 0);
        assert(buf[0] == 0xAA);
        printf("Case 4 PASS: clean refusal, buffer untouched under insufficient capacity\n");
    }

    /* --- Case 5: RetroPad -> bits table, fourteen identifiers one by one +
     * L3/R3 with no bit. --- */
    {
        static const struct { unsigned id; uint16_t bit; } table[] = {
            { GMC_JOYPAD_UP,     GROOVY_JOY_UP   },
            { GMC_JOYPAD_DOWN,   GROOVY_JOY_DOWN },
            { GMC_JOYPAD_LEFT,   GROOVY_JOY_LEFT },
            { GMC_JOYPAD_RIGHT,  GROOVY_JOY_RIGHT},
            { GMC_JOYPAD_B,      GROOVY_JOY_B1   },
            { GMC_JOYPAD_A,      GROOVY_JOY_B2   },
            { GMC_JOYPAD_Y,      GROOVY_JOY_B3   },
            { GMC_JOYPAD_X,      GROOVY_JOY_B4   },
            { GMC_JOYPAD_SELECT, GROOVY_JOY_B5   },
            { GMC_JOYPAD_START,  GROOVY_JOY_B6   },
            { GMC_JOYPAD_L,      GROOVY_JOY_B7   },
            { GMC_JOYPAD_R,      GROOVY_JOY_B8   },
            { GMC_JOYPAD_L2,     GROOVY_JOY_B9   },
            { GMC_JOYPAD_R2,     GROOVY_JOY_B10  },
        };
        size_t i;
        for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            uint16_t j;
            g_test_pressed_id = table[i].id;
            j = gmc_input_joy_from_retropad(test_get_pad);
            assert(j == table[i].bit);
        }
        /* L3 and R3: no bit, even when "held" */
        g_test_pressed_id = GMC_JOYPAD_L3;
        assert(gmc_input_joy_from_retropad(test_get_pad) == 0);
        g_test_pressed_id = GMC_JOYPAD_R3;
        assert(gmc_input_joy_from_retropad(test_get_pad) == 0);
        g_test_pressed_id = 0xFFFFFFFFu;   /* nothing held, reset to idle */
        printf("Case 5 PASS: RetroPad -> bits table, fourteen identifiers + L3/R3 with no bit\n");
    }

    /* --- Case 5bis: gmc_input_joy_from_mask -- the same table, read from a
     * single call's mask. Bit-for-bit identical to
     * gmc_input_joy_from_retropad, THEN a combination of several buttons at
     * once -- something a button-by-button test cannot see (a misplaced bit
     * OR would stay invisible with only one bit held). --- */
    {
        static const struct { unsigned id; uint16_t bit; } table[] = {
            { GMC_JOYPAD_UP,     GROOVY_JOY_UP   },
            { GMC_JOYPAD_DOWN,   GROOVY_JOY_DOWN },
            { GMC_JOYPAD_LEFT,   GROOVY_JOY_LEFT },
            { GMC_JOYPAD_RIGHT,  GROOVY_JOY_RIGHT},
            { GMC_JOYPAD_B,      GROOVY_JOY_B1   },
            { GMC_JOYPAD_A,      GROOVY_JOY_B2   },
            { GMC_JOYPAD_Y,      GROOVY_JOY_B3   },
            { GMC_JOYPAD_X,      GROOVY_JOY_B4   },
            { GMC_JOYPAD_SELECT, GROOVY_JOY_B5   },
            { GMC_JOYPAD_START,  GROOVY_JOY_B6   },
            { GMC_JOYPAD_L,      GROOVY_JOY_B7   },
            { GMC_JOYPAD_R,      GROOVY_JOY_B8   },
            { GMC_JOYPAD_L2,     GROOVY_JOY_B9   },
            { GMC_JOYPAD_R2,     GROOVY_JOY_B10  },
        };
        size_t i;
        uint16_t combined_mask = 0u, combined_expect = 0u;

        for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            uint16_t mask = (uint16_t)(1u << table[i].id);
            assert(gmc_input_joy_from_mask(mask) == table[i].bit);
            combined_mask |= mask;
            combined_expect |= table[i].bit;
        }
        /* L3 (14) and R3 (15): never a bit, even in the mask */
        assert(gmc_input_joy_from_mask((uint16_t)(1u << GMC_JOYPAD_L3)) == 0);
        assert(gmc_input_joy_from_mask((uint16_t)(1u << GMC_JOYPAD_R3)) == 0);
        assert(gmc_input_joy_from_mask(0u) == 0);

        /* combination: up + B + L2, a single mask call */
        {
            uint16_t m = (uint16_t)((1u << GMC_JOYPAD_UP) | (1u << GMC_JOYPAD_B) | (1u << GMC_JOYPAD_L2));
            uint16_t expected = (uint16_t)(GROOVY_JOY_UP | GROOVY_JOY_B1 | GROOVY_JOY_B9);
            assert(gmc_input_joy_from_mask(m) == expected);
        }
        /* all fourteen at once: identical to the sum of the one-by-one calls
         * (gmc_input_joy_from_retropad), proof that the two paths are equivalent */
        assert(gmc_input_joy_from_mask(combined_mask) == combined_expect);
        {
            uint16_t j = 0;
            for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
                g_test_pressed_id = table[i].id;
                j = (uint16_t)(j | gmc_input_joy_from_retropad(test_get_pad));
            }
            g_test_pressed_id = 0xFFFFFFFFu;
            assert(j == combined_expect);   /* same result as the mask path, two routes */
        }
        printf("Case 5bis PASS: gmc_input_joy_from_mask -- bit table + combination + equivalence with gmc_input_joy_from_retropad\n");
    }

    /* --- Case 6: handshake in a local loop -- test video port 32104 (so it
     * listens on 32105), a second local socket with NO bind() that sends the
     * handshake byte, then a negative case (5 bytes before the byte). --- */
    {
        gmc_input_t *in;
        struct sockaddr_in dst;
#ifdef _WIN32
        SOCKET tsock;
        DWORD tmo = 500;
#else
        int tsock;
        struct timeval tmo;
#endif

        in = gmc_input_open(32104u);
        assert(in != NULL);

#ifdef _WIN32
        tsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(tsock != INVALID_SOCKET);
        setsockopt(tsock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
#else
        tsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(tsock >= 0);
        tmo.tv_sec = 0; tmo.tv_usec = 500000;
        setsockopt(tsock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
#endif

        memset(&dst, 0, sizeof dst);
        dst.sin_family      = AF_INET;
        dst.sin_port        = htons(32105);
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        /* --- negative case first: 5 bytes before the handshake byte --- */
        {
            uint8_t junk[5] = { 1, 2, 3, 4, 5 };
            unsigned hs; unsigned long long sent, no_peer, rejected;
            int bound, rc;

            rc = (int)sendto(tsock, (const char *)junk, sizeof junk, 0,
                             (struct sockaddr *)&dst, sizeof dst);
            assert(rc == (int)sizeof junk);
            test_sleep_ms(100);

            bound = gmc_input_poll(in);
            assert(bound == 0);
            gmc_input_stats(in, &hs, &sent, &no_peer, &rejected);
            assert(hs == 0);
            assert(rejected == 1);
        }

        /* --- the handshake byte --- */
        {
            uint8_t hello = 0;
            unsigned hs; unsigned long long sent, no_peer, rejected;
            int bound, rc;

            rc = (int)sendto(tsock, (const char *)&hello, 1, 0,
                             (struct sockaddr *)&dst, sizeof dst);
            assert(rc == 1);
            test_sleep_ms(100);

            bound = gmc_input_poll(in);
            assert(bound == 1);
            gmc_input_stats(in, &hs, &sent, &no_peer, &rejected);
            assert(hs == 1);
        }

        /* --- sending to the ephemeral port learned at handshake time, never
         * a recomputed port --- */
        {
            size_t written = gmc_input_send(in, 1u, 0u, NULL);
            uint8_t rxbuf[64];
            struct sockaddr_in from; socklen_t from_len = sizeof from;
            int n;
            assert(written == 9);

#ifdef _WIN32
            n = recvfrom(tsock, (char *)rxbuf, sizeof rxbuf, 0, (struct sockaddr *)&from, &from_len);
#else
            n = (int)recvfrom(tsock, rxbuf, sizeof rxbuf, 0, (struct sockaddr *)&from, &from_len);
#endif
            assert(n == 9);
        }

#ifdef _WIN32
        closesocket(tsock);
#else
        close(tsock);
#endif
        gmc_input_close(in);
        printf("Case 6 PASS: handshake in a local loop (negative then positive)\n");
    }

    test_two_threads_handshake_and_mailbox();

    printf("ALL CASES PASS (test_gmclient_input)\n");
    return 0;
}
