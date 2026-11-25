/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gm_shard.c -- see gm_shard.h. Reed-Solomon over GF(256) with a Cauchy matrix:
 * parity row j, data column i = 1 / ((k + j) xor i); any k of the k + m shards
 * rebuild the data. */
#include "gm_shard.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- GF(256) */
static uint8_t g_exp[512];
static uint8_t g_log[256];
static int     g_gf_ready;

static void gf_init(void)
{
    unsigned x = 1u, i;
    if (g_gf_ready) return;
    for (i = 0; i < 255u; i++) {
        g_exp[i] = (uint8_t)x;
        g_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100u) x ^= 0x11Du;
    }
    for (i = 255u; i < 512u; i++) g_exp[i] = g_exp[i - 255u];
    g_gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    return (a && b) ? g_exp[(unsigned)g_log[a] + g_log[b]] : 0u;
}

static uint8_t gf_inv(uint8_t a)   /* a != 0 */
{
    return g_exp[255u - g_log[a]];
}

/* dst ^= c * src, byte by byte */
static void gf_axpy(uint8_t *dst, const uint8_t *src, uint8_t c, size_t n)
{
    size_t i;
    unsigned lc;
    if (c == 0u) return;
    if (c == 1u) { for (i = 0; i < n; i++) dst[i] ^= src[i]; return; }
    lc = g_log[c];
    for (i = 0; i < n; i++)
        if (src[i]) dst[i] ^= g_exp[lc + g_log[src[i]]];
}

/* Cauchy coefficient of parity shard j for data shard i */
static uint8_t cauchy(unsigned k, unsigned j, unsigned i)
{
    return gf_inv((uint8_t)((k + j) ^ i));
}

/* parity[j] = sum_i cauchy(k, j, i) * data[i], each shard `sz` bytes */
static void fec_encode(unsigned k, unsigned m, size_t sz,
                       const uint8_t *const *data, uint8_t *const *parity)
{
    unsigned i, j;
    for (j = 0; j < m; j++) {
        memset(parity[j], 0, sz);
        for (i = 0; i < k; i++) gf_axpy(parity[j], data[i], cauchy(k, j, i), sz);
    }
}

/* Rebuilds the missing DATA shards in place. shards[0..k+m) are all writable
 * buffers of `sz` bytes; present[] says which hold a received shard. Returns the
 * number of data shards rebuilt, or -1 when fewer than k shards are present. */
static int fec_decode(unsigned k, unsigned m, size_t sz, uint8_t *const *shards, const uint8_t *present)
{
    unsigned rows[GM_SHARD_MAX], nrows = 0, i, j, c, r;
    unsigned missing = 0;
    uint8_t A[GM_SHARD_MAX][GM_SHARD_MAX], I[GM_SHARD_MAX][GM_SHARD_MAX];

    for (i = 0; i < k; i++) if (!present[i]) missing++;
    if (missing == 0) return 0;
    for (i = 0; i < k && nrows < k; i++) if (present[i]) rows[nrows++] = i;
    for (j = 0; j < m && nrows < k; j++) if (present[k + j]) rows[nrows++] = k + j;
    if (nrows < k) return -1;

    for (r = 0; r < k; r++) {
        memset(A[r], 0, k);
        memset(I[r], 0, k);
        I[r][r] = 1u;
        if (rows[r] < k) A[r][rows[r]] = 1u;
        else for (c = 0; c < k; c++) A[r][c] = cauchy(k, rows[r] - k, c);
    }
    /* Gauss-Jordan: A -> identity, I -> A^-1 */
    for (c = 0; c < k; c++) {
        unsigned p = c;
        uint8_t inv;
        while (p < k && A[p][c] == 0u) p++;
        if (p == k) return -1;   /* cannot happen with a Cauchy matrix */
        if (p != c) {
            uint8_t t[GM_SHARD_MAX];
            memcpy(t, A[p], k); memcpy(A[p], A[c], k); memcpy(A[c], t, k);
            memcpy(t, I[p], k); memcpy(I[p], I[c], k); memcpy(I[c], t, k);
        }
        inv = gf_inv(A[c][c]);
        for (j = 0; j < k; j++) { A[c][j] = gf_mul(A[c][j], inv); I[c][j] = gf_mul(I[c][j], inv); }
        for (r = 0; r < k; r++) {
            uint8_t f = A[r][c];
            if (r == c || f == 0u) continue;
            for (j = 0; j < k; j++) { A[r][j] ^= gf_mul(f, A[c][j]); I[r][j] ^= gf_mul(f, I[c][j]); }
        }
    }
    /* data[i] = sum_c I[i][c] * shards[rows[c]] , for each missing data shard i */
    for (i = 0; i < k; i++) {
        if (present[i]) continue;
        memset(shards[i], 0, sz);
        for (c = 0; c < k; c++) gf_axpy(shards[i], shards[rows[c]], I[i][c], sz);
    }
    return (int)missing;
}

#define GM_SHARD_PREFIX 10u   /* u32 length, u32 Groovy frame_id, u16 vsync */

/* ---------------------------------------------------------------- sender */
int gm_shard_plan(size_t len, unsigned dgram_max, unsigned parity_pct,
                  unsigned *k_out, unsigned *m_out, size_t *sz_out)
{
    size_t total = len + GM_SHARD_PREFIX, sz_max, k, sz, m;
    if (dgram_max < GM_SHARD_MIN_DGRAM || dgram_max > 1472u || !k_out || !m_out || !sz_out) return -1;
    sz_max = (size_t)dgram_max - GM_SHARD_HDR;
    k = (total + sz_max - 1u) / sz_max;
    if (k == 0u) k = 1u;
    sz = (total + k - 1u) / k;
    if (sz < GM_SHARD_MIN_DGRAM - GM_SHARD_HDR) sz = GM_SHARD_MIN_DGRAM - GM_SHARD_HDR;
    m = parity_pct ? (k * parity_pct + 99u) / 100u : 0u;
    if (parity_pct && m == 0u) m = 1u;
    if (k + m > GM_SHARD_MAX) return -1;
    *k_out = (unsigned)k; *m_out = (unsigned)m; *sz_out = sz;
    return 0;
}

int gm_shard_send(const uint8_t *bitstream, size_t len, uint16_t frame_id, uint8_t flags,
                  unsigned dgram_max, unsigned parity_pct, gm_shard_emit_fn emit, void *user)
{
    return gm_shard_send_ex(bitstream, len, frame_id, 0u, 0u, flags, dgram_max, parity_pct, emit, user);
}

int gm_shard_send_ex(const uint8_t *bitstream, size_t len, uint16_t frame_id, uint32_t gid, uint16_t vsync, uint8_t flags,
                     unsigned dgram_max, unsigned parity_pct, gm_shard_emit_fn emit, void *user)
{
    unsigned k, m, i;
    size_t sz, total;
    uint8_t *data = NULL, *par = NULL, *dg = NULL;
    const uint8_t *dptr[GM_SHARD_MAX];
    uint8_t *pptr[GM_SHARD_MAX];
    int rc = -1;

    if (!emit || (len > 0u && !bitstream) || len > 0x7FFFFFFFu) return -1;
    if (gm_shard_plan(len, dgram_max, parity_pct, &k, &m, &sz) != 0) return -1;
    gf_init();
    total = (size_t)k * sz;
    data = (uint8_t *)calloc(total ? total : 1u, 1u);
    par = (uint8_t *)calloc(m ? (size_t)m * sz : 1u, 1u);
    dg = (uint8_t *)malloc(GM_SHARD_HDR + sz);
    if (!data || !par || !dg) goto done;

    data[0] = (uint8_t)(len & 0xFFu); data[1] = (uint8_t)((len >> 8) & 0xFFu);
    data[2] = (uint8_t)((len >> 16) & 0xFFu); data[3] = (uint8_t)((len >> 24) & 0xFFu);
    data[4] = (uint8_t)(gid & 0xFFu); data[5] = (uint8_t)((gid >> 8) & 0xFFu);
    data[6] = (uint8_t)((gid >> 16) & 0xFFu); data[7] = (uint8_t)((gid >> 24) & 0xFFu);
    data[8] = (uint8_t)(vsync & 0xFFu); data[9] = (uint8_t)(vsync >> 8);
    if (len) memcpy(data + GM_SHARD_PREFIX, bitstream, len);
    for (i = 0; i < k; i++) dptr[i] = data + (size_t)i * sz;
    for (i = 0; i < m; i++) pptr[i] = par + (size_t)i * sz;
    fec_encode(k, m, sz, dptr, pptr);

    for (i = 0; i < k + m; i++) {
        dg[0] = (uint8_t)(frame_id & 0xFFu); dg[1] = (uint8_t)(frame_id >> 8);
        dg[2] = (uint8_t)i; dg[3] = (uint8_t)k; dg[4] = (uint8_t)m; dg[5] = flags;
        memcpy(dg + GM_SHARD_HDR, i < k ? dptr[i] : pptr[i - k], sz);
        if (emit(user, dg, GM_SHARD_HDR + sz) != 0) goto done;
    }
    rc = 0;
done:
    free(data); free(par); free(dg);
    return rc;
}

/* ---------------------------------------------------------------- receiver */
#define RX_SLOTS 4

struct rx_slot {
    int       used, delivered;
    uint16_t  id;
    uint8_t   flags;
    unsigned  k, m, got;
    size_t    sz, cap;
    uint8_t  *buf;                       /* (k + m) * sz */
    uint8_t   present[GM_SHARD_MAX];
};

struct gm_shard_rx {
    struct rx_slot  slot[RX_SLOTS];
    uint8_t        *out;
    size_t          out_cap;
    int             have_last;
    uint16_t        last;
    gm_shard_stats  st;
};

static int newer(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) > 0; }

gm_shard_rx *gm_shard_rx_new(void)
{
    gf_init();
    return (gm_shard_rx *)calloc(1u, sizeof(gm_shard_rx));
}

void gm_shard_rx_free(gm_shard_rx *r)
{
    unsigned i;
    if (!r) return;
    for (i = 0; i < RX_SLOTS; i++) free(r->slot[i].buf);
    free(r->out);
    free(r);
}

void gm_shard_rx_reset(gm_shard_rx *r)
{
    unsigned i;
    if (!r) return;
    for (i = 0; i < RX_SLOTS; i++) { r->slot[i].used = 0; r->slot[i].delivered = 0; }
    r->have_last = 0;
}

const gm_shard_stats *gm_shard_rx_stats(const gm_shard_rx *r) { return r ? &r->st : NULL; }

static struct rx_slot *slot_for(gm_shard_rx *r, uint16_t id)
{
    unsigned i;
    struct rx_slot *free_s = NULL, *oldest = NULL;
    for (i = 0; i < RX_SLOTS; i++)
        if (r->slot[i].used && r->slot[i].id == id) return &r->slot[i];
    for (i = 0; i < RX_SLOTS; i++) {
        if (!r->slot[i].used) { if (!free_s) free_s = &r->slot[i]; }
        else if (!oldest || newer(oldest->id, r->slot[i].id)) oldest = &r->slot[i];
    }
    if (free_s) return free_s;
    if (oldest && !oldest->delivered) r->st.lost_frames++;
    return oldest;
}

int gm_shard_rx_push(gm_shard_rx *r, const uint8_t *d, size_t len, gm_shard_frame *out)
{
    uint16_t id;
    unsigned idx, k, m, i;
    size_t sz;
    struct rx_slot *s;
    int fresh = 0;

    if (!r || !d || !out) return -1;
    if (len < GM_SHARD_MIN_DGRAM || len > GM_SHARD_HDR + 1466u) { r->st.rejected++; return -1; }
    id = (uint16_t)(d[0] | (d[1] << 8));
    idx = d[2]; k = d[3]; m = d[4];
    sz = len - GM_SHARD_HDR;
    if (k == 0u || k + m > GM_SHARD_MAX || idx >= k + m) { r->st.rejected++; return -1; }

    if (r->have_last && !newer(id, r->last)) return 0;   /* an older frame than the last delivered: too late */

    s = slot_for(r, id);
    if (!s) return 0;
    if (s->used && s->id == id) {
        if (s->k != k || s->m != m || s->sz != sz) { r->st.rejected++; return -1; }
    } else {
        size_t need = (size_t)(k + m) * sz;
        if (need > s->cap) {
            uint8_t *nb = (uint8_t *)realloc(s->buf, need);
            if (!nb) return 0;
            s->buf = nb; s->cap = need;
        }
        s->used = 1; s->delivered = 0; s->id = id; s->flags = d[5];
        s->k = k; s->m = m; s->sz = sz; s->got = 0;
        memset(s->present, 0, sizeof s->present);
        fresh = 1;
    }
    (void)fresh;
    if (s->delivered) return 0;
    if (s->present[idx]) { r->st.duplicate_shards++; return 0; }
    memcpy(s->buf + (size_t)idx * sz, d + GM_SHARD_HDR, sz);
    s->present[idx] = 1u;
    s->got++;
    r->st.shards++;
    if (s->got < k) return 0;

    /* enough shards: rebuild what is missing and put the frame together */
    {
        uint8_t *sp[GM_SHARD_MAX];
        int rebuilt;
        size_t flen;
        for (i = 0; i < k + m; i++) sp[i] = s->buf + (size_t)i * sz;
        rebuilt = fec_decode(k, m, sz, sp, s->present);
        if (rebuilt < 0) return 0;
        if ((size_t)k * sz > r->out_cap) {
            uint8_t *no = (uint8_t *)realloc(r->out, (size_t)k * sz);
            if (!no) return 0;
            r->out = no; r->out_cap = (size_t)k * sz;
        }
        for (i = 0; i < k; i++) memcpy(r->out + (size_t)i * sz, sp[i], sz);
        flen = (size_t)r->out[0] | ((size_t)r->out[1] << 8) | ((size_t)r->out[2] << 16) | ((size_t)r->out[3] << 24);
        s->delivered = 1;
        if (flen > (size_t)k * sz - GM_SHARD_PREFIX) { r->st.rejected++; return -1; }
        if (r->have_last && !newer(id, r->last)) { r->st.stale_frames++; return 0; }
        out->frame_id = id;
        out->flags = s->flags;
        out->gid = (uint32_t)r->out[4] | ((uint32_t)r->out[5] << 8) | ((uint32_t)r->out[6] << 16) | ((uint32_t)r->out[7] << 24);
        out->vsync = (uint16_t)(r->out[8] | (r->out[9] << 8));
        out->data = r->out + GM_SHARD_PREFIX;
        out->len = flen;
        out->recovered = (unsigned)rebuilt;
        out->gap = r->have_last ? (unsigned)(uint16_t)(id - r->last - 1u) : 0u;
        r->have_last = 1; r->last = id;
        r->st.frames++;
        if (rebuilt > 0) r->st.recovered_frames++;
        /* frames older than this one can no longer be shown */
        for (i = 0; i < RX_SLOTS; i++) {
            struct rx_slot *o = &r->slot[i];
            if (o != s && o->used && !newer(o->id, id)) {
                if (!o->delivered) r->st.lost_frames++;
                o->used = 0;
            }
        }
        return 1;
    }
}
