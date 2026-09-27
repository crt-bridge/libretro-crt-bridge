/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gmclient_input.c -- see gmclient_input.h.
 *
 * A second protocol running in the INVERSE direction from the video channel:
 * here it is the CLIENT that sends and the EMITTER that listens. The client
 * therefore plays the role that the hardware receiver's own input handling
 * already plays on the Linux receiver side: bind a FIXED port derived from the video port
 * (setup_input_socket), wait for a handshake byte sent from an EPHEMERAL port
 * -- gm_bind_inputs never calls bind() on its input socket --, retain that
 * exact address (drain_socket), and send only to it from then on.
 *
 * Deliberate difference from the hardware receiver's own input handling: drain_socket handshakes on ANY
 * datagram (its size is never checked). Here, the handshake accepts ONLY the
 * exact length of 1 byte -- strict routing, any other length is rejected and
 * counted (a third party on the LAN sending a single byte on video_port+1
 * would otherwise hijack the input channel toward its own address).
 */
#include "gmclient_input.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET gmc_input_sock;
#  define GMC_INPUT_BAD_SOCK INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
typedef int gmc_input_sock;
#  define GMC_INPUT_BAD_SOCK (-1)
#endif

/* Same hard bound as the emitter side's own input polling -- a flooding peer
 * must not lengthen the core's tick. */
/* Lock shared with gmclient.c. Included AFTER <winsock2.h> above. */
#include "gmc_mtx.h"

#define GMC_INPUT_MAX_POLL 64

struct gmc_input {
    gmc_input_sock     sock;
    /* --- state shared between the receiving thread and retro_run ---------------
     * EVERYTHING below `mtx` is read or written by both threads and only ever
     * touched under `mtx`. `sock` stays outside: it is set once by
     * gmc_input_open before the second thread exists, and is never modified
     * afterward -- only the service function reads it to receive. */
    gmc_mutex          mtx;
    struct sockaddr_in bound_addr;
    socklen_t          bound_addr_len;
    int                have_bound;
    uint8_t            order;
    unsigned           hello_seen;
    unsigned long long sent;
    unsigned long long no_peer;
    unsigned long long rejected;
    /* Mailbox: retro_run deposits, the receiving thread sends. A STATE, not a
     * queue -- a deposit overwrites the previous one, and the latest known
     * value is the right one. `box_pending` says whether there is something
     * new to send. */
    uint32_t           box_frame;
    uint16_t           box_joy1;
    int8_t             box_axes[8];
    int                box_has_axes;
    int                box_pending;
#ifdef _WIN32
    int wsa_started;
#endif
};

/* ---------------------------------------------------------------------------
 * groovy_input_pack_joy -- a CONSCIOUS, bit-for-bit copy of the hardware
 * receiver's own groovy_input_pack_joy. Same "return 0, never a partial
 * write" discipline if `cap` is insufficient: gmclient_input.h's pack(1)
 * structures are the only write, no byte laid down by hand. `order` is a
 * uint8_t that wraps naturally.
 * ------------------------------------------------------------------------- */
static size_t groovy_input_pack_joy(uint8_t *out, size_t cap, uint32_t frame,
                                    uint8_t order, uint16_t joy1, uint16_t joy2,
                                    const int8_t axes[8], int with_analog)
{
    if (!with_analog) {
        struct groovy_joy_dig_inputs pkt;
        if (cap < sizeof(pkt)) return 0;
        pkt.frame = frame;
        pkt.order = order;
        pkt.joy1  = joy1;
        pkt.joy2  = joy2;
        memcpy(out, &pkt, sizeof(pkt));
        return sizeof(pkt);
    } else {
        struct groovy_joy_inputs pkt;
        if (cap < sizeof(pkt)) return 0;
        pkt.frame = frame;
        pkt.order = order;
        pkt.joy1  = joy1;
        pkt.joy2  = joy2;
        pkt.joy1_lx = axes[0]; pkt.joy1_ly = axes[1];
        pkt.joy1_rx = axes[2]; pkt.joy1_ry = axes[3];
        pkt.joy2_lx = axes[4]; pkt.joy2_ly = axes[5];
        pkt.joy2_rx = axes[6]; pkt.joy2_ry = axes[7];
        memcpy(out, &pkt, sizeof(pkt));
        return sizeof(pkt);
    }
}

/* ---------------------------------------------------------------------------
 * gmc_input_joy_from_retropad -- RETRO_DEVICE_ID_JOYPAD_* -> Groovy bit
 * table, verified bit-for-bit against the fork's consumer side
 * (mister_joypad.c). Local identifiers instead of an #include libretro.h:
 * this file stays free of any frontend dependency, the same discipline as
 * gmclient.c/gmclient.h.
 * ------------------------------------------------------------------------- */
#define GMC_JOYPAD_B      0u
#define GMC_JOYPAD_Y      1u
#define GMC_JOYPAD_SELECT 2u
#define GMC_JOYPAD_START  3u
#define GMC_JOYPAD_UP     4u
#define GMC_JOYPAD_DOWN   5u
#define GMC_JOYPAD_LEFT   6u
#define GMC_JOYPAD_RIGHT  7u
#define GMC_JOYPAD_A      8u
#define GMC_JOYPAD_X      9u
#define GMC_JOYPAD_L      10u
#define GMC_JOYPAD_R      11u
#define GMC_JOYPAD_L2     12u
#define GMC_JOYPAD_R2     13u
#define GMC_JOYPAD_L3     14u   /* no bit -- do not invent one */
#define GMC_JOYPAD_R3     15u   /* no bit -- do not invent one */

uint16_t gmc_input_joy_from_retropad(int (*get)(unsigned id))
{
    uint16_t j = 0;
    if (!get) return 0;
    if (get(GMC_JOYPAD_UP))     j |= GROOVY_JOY_UP;
    if (get(GMC_JOYPAD_DOWN))   j |= GROOVY_JOY_DOWN;
    if (get(GMC_JOYPAD_LEFT))   j |= GROOVY_JOY_LEFT;
    if (get(GMC_JOYPAD_RIGHT))  j |= GROOVY_JOY_RIGHT;
    if (get(GMC_JOYPAD_B))      j |= GROOVY_JOY_B1;
    if (get(GMC_JOYPAD_A))      j |= GROOVY_JOY_B2;
    if (get(GMC_JOYPAD_Y))      j |= GROOVY_JOY_B3;
    if (get(GMC_JOYPAD_X))      j |= GROOVY_JOY_B4;
    if (get(GMC_JOYPAD_SELECT)) j |= GROOVY_JOY_B5;
    if (get(GMC_JOYPAD_START))  j |= GROOVY_JOY_B6;
    if (get(GMC_JOYPAD_L))      j |= GROOVY_JOY_B7;
    if (get(GMC_JOYPAD_R))      j |= GROOVY_JOY_B8;
    if (get(GMC_JOYPAD_L2))     j |= GROOVY_JOY_B9;
    if (get(GMC_JOYPAD_R2))     j |= GROOVY_JOY_B10;
    /* GMC_JOYPAD_L3 (14) and GMC_JOYPAD_R3 (15): deliberately never tested here --
     * neither GroovyMAME nor the fork assigns them a bit. */
    return j;
}

/* ---------------------------------------------------------------------------
 * gmc_input_joy_from_mask -- the same table as gmc_input_joy_from_retropad,
 * but read from the mask of a single call to
 * input_state_cb(..., RETRO_DEVICE_ID_JOYPAD_MASK). Bit N of the mask is the
 * same local identifier as get(N) above (RETRO_DEVICE_ID_JOYPAD_* --
 * libretro.h; GMC_JOYPAD_* is bit-for-bit identical to it, the same "no
 * frontend dependency" discipline as the rest of this file). L3/R3: never
 * tested, same rule.
 * ------------------------------------------------------------------------- */
uint16_t gmc_input_joy_from_mask(uint16_t mask)
{
    uint16_t j = 0;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_UP))     j |= GROOVY_JOY_UP;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_DOWN))   j |= GROOVY_JOY_DOWN;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_LEFT))   j |= GROOVY_JOY_LEFT;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_RIGHT))  j |= GROOVY_JOY_RIGHT;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_B))      j |= GROOVY_JOY_B1;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_A))      j |= GROOVY_JOY_B2;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_Y))      j |= GROOVY_JOY_B3;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_X))      j |= GROOVY_JOY_B4;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_SELECT)) j |= GROOVY_JOY_B5;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_START))  j |= GROOVY_JOY_B6;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_L))      j |= GROOVY_JOY_B7;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_R))      j |= GROOVY_JOY_B8;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_L2))     j |= GROOVY_JOY_B9;
    if (mask & (uint16_t)(1u << GMC_JOYPAD_R2))     j |= GROOVY_JOY_B10;
    return j;
}

/* ---------------------------------------------------------------------------
 * gmc_input_open -- binds INADDR_ANY:<video_port + 1>. Same
 * socket()/bind()/non-blocking sequence as the hardware receiver's own
 * setup_input_socket, ported to Winsock/POSIX the same way gmclient.c
 * already does for the video socket.
 * ------------------------------------------------------------------------- */
gmc_input_t *gmc_input_open(unsigned video_port)
{
    gmc_input_t *in = (gmc_input_t *)calloc(1, sizeof *in);
    if (!in) return NULL;
    /* Before ANY error path: the returns below go through gmc_input_close,
     * which destroys the lock. */
    MTX_INIT(&in->mtx);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { free(in); return NULL; }
    in->wsa_started = 1;
#endif

    in->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (in->sock == GMC_INPUT_BAD_SOCK) { gmc_input_close(in); return NULL; }

    {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port        = htons((uint16_t)(video_port + 1u));   /* NEVER hardcode this port */
        if (bind(in->sock, (struct sockaddr *)&a, sizeof a) != 0) { gmc_input_close(in); return NULL; }
    }

#ifdef _WIN32
    { u_long nb = 1; ioctlsocket(in->sock, FIONBIO, &nb); }
#else
    /* Check this return value, as the emitter side's own input binding
     * already does -- without this guard, a failure would leave the
     * socket BLOCKING, and gmc_input_service_rx could then hang indefinitely
     * on recvfrom if the datagram announced by select() has already been
     * consumed. */
    if (fcntl(in->sock, F_SETFL, fcntl(in->sock, F_GETFL, 0) | O_NONBLOCK) != 0) {
        gmc_input_close(in);
        return NULL;
    }
#endif

    return in;
}

/* drain_locked -- the historical body of gmc_input_poll, ALREADY UNDER `mtx`.
 * Unchanged in its logic: at most GMC_INPUT_MAX_POLL datagrams, handshake on
 * the EXACT length of 1 byte, everything else rejected and counted. */
static int drain_locked(gmc_input_t *in)
{
    int i;
    uint8_t buf[64];

    for (i = 0; i < GMC_INPUT_MAX_POLL; i++) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof src;
#ifdef _WIN32
        int n = recvfrom(in->sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&src, &src_len);
        if (n < 0) break;   /* WSAEWOULDBLOCK or error -- nothing more to drain */
#else
        int n = (int)recvfrom(in->sock, buf, sizeof buf, 0, (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            if (errno == EINTR) { i--; continue; }
            break;   /* EAGAIN/EWOULDBLOCK or unrecoverable error -- stay silent */
        }
#endif
        if ((size_t)src_len > sizeof in->bound_addr) continue;   /* never an out-of-bounds write */

        if (n != 1) {
            in->rejected++;
            continue;   /* strict routing: only the exact length of 1 handshakes */
        }

        memcpy(&in->bound_addr, &src, (size_t)src_len);
        in->bound_addr_len = src_len;
        in->have_bound = 1;
        in->hello_seen++;
    }
    return in->have_bound;
}

/* gmc_input_poll -- kept: the SDL harness and the tests use it, and it remains
 * the simplest way to write a single-threaded caller. The CORE no longer calls
 * it: gmc_input_service_rx receives instead, on the receiving thread. */
int gmc_input_poll(gmc_input_t *in)
{
    int b;
    if (!in) return 0;
    MTX_LOCK(&in->mtx);
    b = drain_locked(in);
    MTX_UNLOCK(&in->mtx);
    return b;
}

/* --- What the RECEIVING THREAD calls -------------------------------------------- */

int gmc_input_fdset_add(gmc_input_t *in, void *readfds)
{
    if (!in || !readfds || in->sock == GMC_INPUT_BAD_SOCK) return -1;
    FD_SET(in->sock, (fd_set *)readfds);
    return (int)in->sock;   /* nfds for select is ignored on Windows */
}

int gmc_input_fdset_isset(gmc_input_t *in, void *readfds)
{
    if (!in || !readfds || in->sock == GMC_INPUT_BAD_SOCK) return 0;
    return FD_ISSET(in->sock, (fd_set *)readfds) ? 1 : 0;
}

void gmc_input_service_rx(gmc_input_t *in)
{
    if (!in) return;
    MTX_LOCK(&in->mtx);
    (void)drain_locked(in);
    MTX_UNLOCK(&in->mtx);
}

size_t gmc_input_flush(gmc_input_t *in)
{
    uint8_t buf[17];
    size_t n;
    struct sockaddr_in to;
    socklen_t to_len;

    if (!in) return 0;

    MTX_LOCK(&in->mtx);
    if (!in->box_pending || !in->have_bound) { MTX_UNLOCK(&in->mtx); return 0; }
    n = groovy_input_pack_joy(buf, sizeof buf, in->box_frame, in->order,
                              in->box_joy1, 0u,
                              in->box_has_axes ? in->box_axes : NULL,
                              in->box_has_axes);
    if (n == 0) { in->box_pending = 0; MTX_UNLOCK(&in->mtx); return 0; }
    in->order++;              /* uint8_t: wraps naturally */
    in->box_pending = 0;
    to = in->bound_addr;      /* copy: the send happens OUTSIDE the lock */
    to_len = in->bound_addr_len;
    MTX_UNLOCK(&in->mtx);

    /* Sending is a system call: do it outside the lock, so a preemption here
     * never blocks retro_run's deposit. */
    {
#ifdef _WIN32
        int rc = sendto(in->sock, (const char *)buf, (int)n, 0,
                        (struct sockaddr *)&to, to_len);
#else
        int rc = (int)sendto(in->sock, buf, n, 0, (struct sockaddr *)&to, to_len);
#endif
        if (rc < 0) return 0;
    }
    MTX_LOCK(&in->mtx);
    in->sent++;
    MTX_UNLOCK(&in->mtx);
    return n;
}

/* --- What retro_run calls. No system call. -------------------------------------- */

int gmc_input_have_peer(const gmc_input_t *in)
{
    int b;
    gmc_input_t *m = (gmc_input_t *)(uintptr_t)in;   /* the lock is not const */
    if (!in) return 0;
    MTX_LOCK(&m->mtx);
    b = in->have_bound;
    MTX_UNLOCK(&m->mtx);
    return b;
}

void gmc_input_post(gmc_input_t *in, uint32_t frame, uint16_t joy1,
                    const int8_t axes[8])
{
    if (!in) return;
    MTX_LOCK(&in->mtx);
    if (!in->have_bound) {
        in->no_peer++;        /* same accounting as before, when gmc_input_send counted here */
        MTX_UNLOCK(&in->mtx);
        return;
    }
    in->box_frame = frame;
    in->box_joy1  = joy1;
    in->box_has_axes = (axes != NULL);
    if (axes) memcpy(in->box_axes, axes, sizeof in->box_axes);
    in->box_pending = 1;
    MTX_UNLOCK(&in->mtx);
}

size_t gmc_input_send(gmc_input_t *in, uint32_t frame, uint16_t joy1, const int8_t axes[8])
{
    uint8_t buf[17];
    size_t n;
    struct sockaddr_in to;
    socklen_t to_len;

    if (!in) return 0;

    MTX_LOCK(&in->mtx);
    if (!in->have_bound) { in->no_peer++; MTX_UNLOCK(&in->mtx); return 0; }
    n = groovy_input_pack_joy(buf, sizeof buf, frame, in->order, joy1, 0u,
                              axes, axes != NULL);
    if (n == 0) { MTX_UNLOCK(&in->mtx); return 0; }
    in->order++;   /* uint8_t: wraps naturally */
    to = in->bound_addr;
    to_len = in->bound_addr_len;
    MTX_UNLOCK(&in->mtx);

    {
#ifdef _WIN32
        int rc = sendto(in->sock, (const char *)buf, (int)n, 0,
                        (struct sockaddr *)&to, to_len);
#else
        int rc = (int)sendto(in->sock, buf, n, 0, (struct sockaddr *)&to, to_len);
#endif
        if (rc < 0) return 0;
    }

    MTX_LOCK(&in->mtx);
    in->sent++;
    MTX_UNLOCK(&in->mtx);
    return n;
}

void gmc_input_stats(const gmc_input_t *in, unsigned *hello_seen,
                     unsigned long long *sent, unsigned long long *no_peer,
                     unsigned long long *rejected)
{
    if (!in) {
        if (hello_seen) *hello_seen = 0;
        if (sent)       *sent       = 0;
        if (no_peer)    *no_peer    = 0;
        if (rejected)   *rejected   = 0;
        return;
    }
    {   /* These four counters are written by the receiving thread (hello_seen,
         * rejected, sent) and by retro_run (no_peer) -- read under the lock, never raw. */
        gmc_input_t *m = (gmc_input_t *)(uintptr_t)in;
        MTX_LOCK(&m->mtx);
        if (hello_seen) *hello_seen = in->hello_seen;
        if (sent)       *sent       = in->sent;
        if (no_peer)    *no_peer    = in->no_peer;
        if (rejected)   *rejected   = in->rejected;
        MTX_UNLOCK(&m->mtx);
    }
}

void gmc_input_close(gmc_input_t *in)
{
    if (!in) return;
    if (in->sock != GMC_INPUT_BAD_SOCK) {
#ifdef _WIN32
        closesocket(in->sock);
#else
        close(in->sock);
#endif
    }
#ifdef _WIN32
    if (in->wsa_started) WSACleanup();
#endif
    /* The receiving thread must no longer touch this channel by the time we get
     * here: gmc_close joins its thread BEFORE the core closes the channel (see
     * crt_bridge_libretro.c). */
    MTX_DESTROY(&in->mtx);
    free(in);
}
