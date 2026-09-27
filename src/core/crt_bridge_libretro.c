/* SPDX-License-Identifier: GPL-3.0-or-later */
/* crt_bridge_libretro.c -- the libretro display core for crt-bridge.
 *
 * It opens gmclient at retro_load_game (no content, SUPPORT_NO_GAME), and on every
 * retro_run pulls from the ring WITHOUT WAITING: a fresh frame goes to video_cb;
 * nothing goes to video_cb(NULL) (a dupe, the frontend re-presents it). A changed
 * geometry goes to RETRO_ENVIRONMENT_SET_GEOMETRY. A block of silence per tick keeps
 * the frontend's audio driver alive.
 *
 * Measured: retro_run's own duration, and the frontend's frame time via
 * RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK. Everything is written to JSON at
 * retro_unload_game, at a path resolved in three steps: GMC_STATS first (an explicit
 * per-run path); otherwise the frontend's own save directory
 * (RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY); otherwise a temporary directory as the last
 * resort ($TMPDIR/gmc-core.json on POSIX, %TEMP%\gmc-core.json on Windows). Settings
 * come from three menu options (groovy_depth, groovy_port, groovy_input), each with an
 * environment override (GMC_DEPTH, GMC_PORT, GMC_INPUT) that takes precedence. Two more
 * levers exist only as environment variables: GMC_VERIFY and GMC_STATUS.
 *
 * t_present = the return of video_cb: inside RetroArch, video_driver_frame presents
 * (swap under vsync) inside the call itself.
 *
 * atexit() safety net: on macOS, the AppKit-driven quit path calls exit() without going
 * back through the frontend deinitialization that would call retro_unload_game(), so an
 * atexit() hook writes the stats if retro_unload_game has not already done so.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "libretro.h"
#include "gmclient.h"
#include "gmclient_input.h"
#include "loss_hint.h"
#include "crt_bridge_version.h"

#define MAX_W 1024
#define MAX_H 1024
#define GMC_AUDIO_TICK_FRAMES 800u   /* 48000 / 60: one fallback silence tick */

static retro_environment_t        env_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

static gmc_t    *client;
static gmc_config cfg;
static gmc_input_t *input_chan;      /* Groovy input channel */
static int       input_enabled = 1;  /* Defaults to enabled -- mirrors the default on of
                                       * scripts/launch-emitter.ps1. */
static uint32_t *fb; static unsigned fb_w, fb_h;
static unsigned  cur_w = 320, cur_h = 240;
static uint64_t  runs, fresh_frames, dupes, geom_changes, ft_late, ft_n, audio_silence_ticks;
/* audio_partial_ticks: ticks where the ring had SOMETHING but less than a full tick, so
 * the remainder is filled with silence. Distinct from audio_silence_ticks, where the
 * ring had nothing at all. */
static uint64_t  audio_partial_ticks;
/* CONTENT of the inputs, not just their flow: counting only that datagrams arrived on
 * time does not prove they carry payload -- a client that sends zeros every frame would
 * give the exact same numbers on a flow-only measure.
 *   input_reads   : frames where all eighteen callbacks were read (have_peer gate open)
 *   joy_nonzero   : frames where joy1 != 0
 *   axes_nonzero  : frames where at least one of the four axes != 0
 *   joy_bits_seen : OR of every joy1 -- which buttons have ever been seen, once and for all */
static uint64_t  input_reads, joy_nonzero, axes_nonzero;
static uint16_t  joy_bits_seen;
/* Button-mask lever: how many frames took each path. mask_reads > 0 proves the mask
 * was used; fallback_reads > 0 proves the fallback (a frontend that refuses
 * RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, or GMC_INPUT_FORCE_FALLBACK=1 for diagnostics)
 * was actually exercised, not dead code. */
static uint64_t  input_mask_reads, input_fallback_reads;
static double   *copy_us; static size_t copy_n, copy_cap;   /* slot -> fb copy */
static double   *inp_us;  static size_t inp_n,  inp_cap;    /* frontend input callbacks */
static double   *poll_us; static size_t poll_n, poll_cap;   /* input_poll_cb ALONE */
/* Input block split into four segments */
static double   *ipoll_us; static size_t ipoll_n, ipoll_cap;  /* gmc_input_have_peer (lock only) */
static double   *ijoy_us;  static size_t ijoy_n,  ijoy_cap;   /* buttons: 1 call (mask) or 14 (fallback) */
static double   *iaxes_us; static size_t iaxes_n, iaxes_cap;  /* 4 input_state_cb axis calls */
static double   *isend_us; static size_t isend_n, isend_cap;  /* gmc_input_post (lock only) */
/* frontend_input_us: sum, per frame, of the frontend callbacks alone that read the pad --
 * input_poll_cb (poll_us) plus the button read (ijoy_us, 1 or 14 calls to input_state_cb
 * depending on the path) plus the axis read (iaxes_us, 4 calls). PUBLISHED separately,
 * never silently dropped. gmc_input_have_peer (ipoll_us) and gmc_input_post (isend_us)
 * stay in retro_run_work_us: CORE code, no frontend callback involved. */
static double   *frontend_input_us; static size_t frontend_input_n, frontend_input_cap;
static double   *run_us; static size_t run_n, run_cap;
static double   *work_us; static size_t work_n, work_cap;   /* retro_run before video_cb */
static double   *vcb_us;  static size_t vcb_n, vcb_cap;     /* video_cb duration alone */
static double   *audio_us; static size_t audio_n, audio_cap; /* audio_batch_cb duration (frontend pacing, not core work) */
static double   *ft_us;  static size_t ft_n_s, ft_cap;
static double    ft_max_us;
static uint64_t  t_load;
static int       g_stats_written;      /* atexit() safety net: writes only once */
static int       g_atexit_registered;
static int       g_collapse_stats_written;   /* one-shot: the collapse-edge write */

/* Frontend-owned save directory, captured once at load time. The string returned by the
 * frontend is copied immediately: the pointer is not guaranteed to outlive the call. */
static char save_dir[1024];
static int  have_save_dir;

static void push(double **v, size_t *n, size_t *cap, double x)
{
    if (*n >= 400000) return;
    if (*n == *cap) { size_t nc = *cap ? *cap * 2 : 4096; double *nv = realloc(*v, nc * sizeof(double)); if (!nv) return; *v = nv; *cap = nc; }
    (*v)[(*n)++] = x;
}
static int cmpd(const void *a, const void *b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
static double pct(double *v, size_t n, double q) { if (!n) return 0; size_t i = (size_t)(q * (double)(n - 1) + 0.5); return v[i < n ? i : n - 1]; }

static void logf_(enum retro_log_level lvl, const char *fmt, ...)
{
    char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (log_cb) log_cb(lvl, "[crt-bridge-client] %s\n", buf); else fprintf(stderr, "[crt-bridge-client] %s\n", buf);
}

/* On-screen channel (SET_MESSAGE_EXT). The core had no screen channel before: logf_() only
 * ever reached the log, which a player sitting on a couch never reads. */
static void emit_msg(const char *text, unsigned duration_ms, unsigned priority,
                     enum retro_log_level level, enum retro_message_target target,
                     enum retro_message_type type)
{
    struct retro_message_ext m;
    if (!env_cb || !text) return;
    memset(&m, 0, sizeof m);
    m.msg      = text;
    m.duration = duration_ms;
    m.priority = priority;
    m.level    = level;
    m.target   = target;
    m.type     = type;
    m.progress = -1;
    env_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &m);
}

/* --- Why the loop stopped: the frontend answers, we do not guess -------------------------
 * RetroArch computes the frame-time delta it hands this core as follows (read in runloop.c
 * at the reference commit, not inferred):
 *   - while its own loop is LOCKED -- paused by pause_nonactive, fast-forwarding, or
 *     recording -- it zeroes its own timestamp and hands back frame_time.reference on the
 *     next call, never the elapsed time;
 *   - while it is merely NOT ITERATING (the macOS video driver asleep), it hands back the
 *     real elapsed time, stall included;
 *   - while it keeps iterating without running the core (menu open), it hands back a single
 *     frame.
 * Three signatures, read at the top of retro_run. The reference is the value THIS core
 * supplied, so recognising it is an exact integer match, not an approximation -- which is
 * why the same macro must feed the callback registration and the comparison. */
#define GMC_FRAME_TIME_REF_US 16667u        /* 1000000/60, handed to SET_FRAME_TIME_CALLBACK */
#define GMC_STALL_GAP_NS      300000000ull  /* 300 ms: 12x the existing ft_late threshold of 25 ms */

static retro_usec_t ft_last_us;             /* delta reported for the current iteration */
static uint64_t t_last_run_entry_ns;        /* 0 until the second retro_run: armed from then on */
static uint64_t stalls;                     /* every wall-clock gap over the threshold */
static double   stall_total_ms;
static uint64_t pause_count;                /* the frontend locked itself and said so */
static double   pause_total_ms;
static uint64_t driver_stalls;              /* the frontend was awake and did not iterate */
static double   driver_stall_ms;
static double   frontend_idle_ms_in_window; /* gaps the frontend owns, kept out of the rate */
static int      g_pause_msg_shown;          /* session-wide idempotence guard, g_ convention */

static const char GMC_MSG_PAUSE[] =
    "Core was paused while the stream kept arriving. "
    "Turn off Settings > User Interface > Pause Content When Not Active "
    "(pause_nonactive = false).";

#define GMC_RATE_WINDOW_NS     10000000000ull  /* 10 s */
#define GMC_COLLAPSE_ENTER_FPS 30.0            /* worst bad run 26.47, best healthy 59.25 */
#define GMC_COLLAPSE_EXIT_FPS  45.0            /* a stalling driver still averages 41 between stalls */
#define GMC_COLLAPSE_MSG_MS    5000u

static uint64_t t_win_start_ns, runs_win_start, fresh_win_start;
static int      collapsed;
static uint64_t collapse_triggered;
static double   collapse_total_ms;
/* Frozen-run detection: the two detectors above are retroactive -- they speak only when a LATER
 * retro_run completes. A TOTAL freeze (retro_run entered, never returns) leaves them both at 0.
 * runs_done counts the retro_run calls that RETURNED -- a plain counter, not run_n, which is
 * capped and can drop samples. At report time, runs - runs_done says whether the last call
 * entered never came back. g_stats_write_in_run marks the one write made from INSIDE retro_run
 * (the collapse rising edge), where the current call is legitimately still running. */
static uint64_t runs_done;
static int      g_stats_write_in_run;

#ifdef __APPLE__
static const char GMC_MSG_COLLAPSE[] =
    "Frame rate collapsed: RetroArch is not iterating this core. "
    "Set video_driver to glcore in Settings > Drivers > Video, then restart RetroArch.";
#else
static const char GMC_MSG_COLLAPSE[] =
    "Frame rate collapsed: RetroArch is not iterating this core. "
    "Check the video driver and vsync settings.";
#endif

/* The core names the symptom; no automatic guard exists
 * anywhere -- this only speaks. The fix lives on the EMITTER's machine, in the
 * GROOVY_FOLLOWERS entry declaring this follower, never on this one: the message
 * says so explicitly so a peer whose picture stutters knows who to ask. */
static struct gmc_loss_hint loss_hint;
#define GMC_LOSS_MSG_MS 6000u

static const char GMC_MSG_LOSS[] =
    "Packets are being lost between the emitter and this machine. If the picture stutters, "
    "ask the person running the emitter to set audio=off for this machine in GROOVY_FOLLOWERS.";

/* The audio=off advice above only applies when the emitter announced sound
 * to this machine (gmc_audio_announced). Without sound it is already applied: this second
 * message names the link instead. Both are quoted word for word in INSTALL.md,
 * section 9. */
static const char GMC_MSG_LOSS_LINK[] =
    "Packets are being lost between the emitter and this machine, and no sound is being sent "
    "to it: the network link itself is the problem.";

/* Forward declaration: write_stats_body() is defined further down the file, needed here
 * (below) before its definition. C11 has no implicit function declarations. */
static void write_stats_body(void);

/* Single entry point into the collapsed state. Both triggers go through it, so the log line
 * and the counter exist in exactly one place: ONE line per episode, never one per run. */
static void enter_collapse(const char *reason)
{
    if (collapsed) return;
    collapsed = 1;
    collapse_triggered++;
    logf_(RETRO_LOG_WARN, "frame rate collapsed: %s", reason);
    /* One extra report write, on the RISING EDGE only. If the user kills a frozen
     * client, this write survives; if they quit cleanly, the close-time write overwrites it
     * with a more complete state. Same file either way, so the surviving file is always the
     * most complete one available.
     * It deliberately does NOT set g_stats_written: consuming that flag would suppress the
     * close-time write. */
    if (!g_collapse_stats_written) {
        g_collapse_stats_written = 1;
        g_stats_write_in_run = 1;   /* called from retro_run: this call is not frozen */
        write_stats_body();
        g_stats_write_in_run = 0;
    }
}

static void frame_time_cb(retro_usec_t usec)
{
    ft_last_us = usec;
    ft_n++;
    push(&ft_us, &ft_n_s, &ft_cap, (double)usec);
    if ((double)usec > ft_max_us) ft_max_us = (double)usec;
    if (usec > 25000) ft_late++;   /* > 1.5x 16.667 ms */
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

/* Core options, v2 interface only -- no v1/v0 fallback and no version probe.
 * groovy_verify and groovy_status are deliberately absent: they are bench-only levers,
 * driven by GMC_VERIFY and GMC_STATUS. */
static const struct retro_core_option_v2_definition option_defs[] = {
   {
      "groovy_depth",
      "Frame buffer depth",
      NULL,
      "How many frames the client holds back before showing them. Higher values ride out "
      "network jitter but add that many frames of latency. Lower values respond faster but "
      "stutter when the network hiccups. Raise this only if the image stutters.",
      NULL,
      NULL,
      {
         { "0", "0 (lowest latency, least tolerant)" },
         { "1", "1 (default)" },
         { "2", "2" },
         { "3", "3" },
         { "4", "4 (most tolerant, highest latency)" },
         { NULL, NULL }
      },
      "1"
   },
   {
      "groovy_port",
      "UDP listen port",
      NULL,
      "The port this core listens on for the video stream from the emitter PC. The gamepad "
      "channel uses this port plus one. Change it only if the default is already taken on "
      "this machine.",
      NULL,
      NULL,
      {
         { "32100", NULL },
         { "32102", NULL },
         { "32110", NULL },
         { NULL, NULL }
      },
      "32100"
   },
   {
      "groovy_input",
      "Send gamepad to emitter",
      NULL,
      "Forwards player 1's gamepad back to the emitter, so a pad plugged into this machine "
      "drives the game running on the emitter PC. Turn it off to play with the pad attached "
      "to the emitter instead.",
      NULL,
      NULL,
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL }
      },
      "enabled"
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, {{ NULL, NULL }}, NULL }
};

static const struct retro_core_options_v2 options_v2 = {
   NULL,
   (struct retro_core_option_v2_definition *)option_defs
};

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    env_cb = cb;
    bool no_game = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, (void *)&options_v2);
    struct retro_log_callback lc;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &lc)) log_cb = lc.log;
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof *info);
    info->library_name = "crt-bridge client";
    info->library_version = CRT_BRIDGE_CLIENT_VERSION;
    info->valid_extensions = "";
    info->need_fullpath = false;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof *info);
    info->geometry.base_width = cur_w; info->geometry.base_height = cur_h;
    info->geometry.max_width = MAX_W; info->geometry.max_height = MAX_H;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = 60.0; info->timing.sample_rate = 48000.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
RETRO_API void retro_reset(void) {}

static const char *getenv_or(const char *k, const char *d) { const char *v = getenv(k); return (v && *v) ? v : d; }

/* Which core settings the environment overrode, and the announcement text for each.
 * Index order is fixed and shared with the run report. */
enum { GMC_OPT_DEPTH = 0, GMC_OPT_PORT, GMC_OPT_VERIFY, GMC_OPT_STATUS, GMC_OPT_INPUT, GMC_OPT_COUNT };
static const char *const gmc_opt_key[GMC_OPT_COUNT] = {
    "groovy_depth", "groovy_port", "groovy_verify", "groovy_status", "groovy_input"
};
static const char *const gmc_opt_env[GMC_OPT_COUNT] = {
    "GMC_DEPTH", "GMC_PORT", "GMC_VERIFY", "GMC_STATUS", "GMC_INPUT"
};
static int  gmc_opt_forced[GMC_OPT_COUNT];
static char gmc_opt_forced_msg[GMC_OPT_COUNT][160];

static void read_options(void)
{
    struct retro_variable v;
    v.key = "groovy_depth";  v.value = NULL;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value) cfg.target_depth = (unsigned)atoi(v.value);
    v.key = "groovy_port";   v.value = NULL;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value) cfg.port = (unsigned)atoi(v.value);
    v.key = "groovy_input"; v.value = NULL;
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value) input_enabled = strcmp(v.value, "disabled") != 0;
    /* Environment variables outrank the menu (bench run scripts) */
    if (getenv(gmc_opt_env[GMC_OPT_DEPTH])) {
        const char *val = getenv(gmc_opt_env[GMC_OPT_DEPTH]);
        cfg.target_depth = (unsigned)atoi(val);
        gmc_opt_forced[GMC_OPT_DEPTH] = 1;
        snprintf(gmc_opt_forced_msg[GMC_OPT_DEPTH], sizeof gmc_opt_forced_msg[GMC_OPT_DEPTH],
                 "%s: forced to %s by %s, menu value ignored",
                 gmc_opt_key[GMC_OPT_DEPTH], val, gmc_opt_env[GMC_OPT_DEPTH]);
    }
    if (getenv(gmc_opt_env[GMC_OPT_PORT])) {
        const char *val = getenv(gmc_opt_env[GMC_OPT_PORT]);
        cfg.port = (unsigned)atoi(val);
        gmc_opt_forced[GMC_OPT_PORT] = 1;
        snprintf(gmc_opt_forced_msg[GMC_OPT_PORT], sizeof gmc_opt_forced_msg[GMC_OPT_PORT],
                 "%s: forced to %s by %s, menu value ignored",
                 gmc_opt_key[GMC_OPT_PORT], val, gmc_opt_env[GMC_OPT_PORT]);
    }
    if (getenv(gmc_opt_env[GMC_OPT_VERIFY])) {
        const char *val = getenv(gmc_opt_env[GMC_OPT_VERIFY]);
        cfg.verify_pattern = atoi(val) != 0;
        gmc_opt_forced[GMC_OPT_VERIFY] = 1;
        snprintf(gmc_opt_forced_msg[GMC_OPT_VERIFY], sizeof gmc_opt_forced_msg[GMC_OPT_VERIFY],
                 "%s: forced to %s by %s, menu value ignored",
                 gmc_opt_key[GMC_OPT_VERIFY], val, gmc_opt_env[GMC_OPT_VERIFY]);
    }
    if (getenv(gmc_opt_env[GMC_OPT_STATUS])) {
        const char *val = getenv(gmc_opt_env[GMC_OPT_STATUS]);
        cfg.send_status = atoi(val) != 0;
        gmc_opt_forced[GMC_OPT_STATUS] = 1;
        snprintf(gmc_opt_forced_msg[GMC_OPT_STATUS], sizeof gmc_opt_forced_msg[GMC_OPT_STATUS],
                 "%s: forced to %s by %s, menu value ignored",
                 gmc_opt_key[GMC_OPT_STATUS], val, gmc_opt_env[GMC_OPT_STATUS]);
    }
    if (getenv(gmc_opt_env[GMC_OPT_INPUT])) {
        const char *val = getenv(gmc_opt_env[GMC_OPT_INPUT]);
        input_enabled = atoi(val) != 0;
        gmc_opt_forced[GMC_OPT_INPUT] = 1;
        snprintf(gmc_opt_forced_msg[GMC_OPT_INPUT], sizeof gmc_opt_forced_msg[GMC_OPT_INPUT],
                 "%s: forced to %s by %s, menu value ignored",
                 gmc_opt_key[GMC_OPT_INPUT], val, gmc_opt_env[GMC_OPT_INPUT]);
    }
    cfg.event_log_path = getenv("GMC_EVENTS");
    cfg.trace_log_path = getenv("GMC_TRACE");   /* diagnostic trace, off unless the variable is set */
}

/* Build platform stamp. This is what makes the collapse flag readable from a distance:
 * a macOS build plus a collapse that fired means a bad video driver -- without the core
 * ever naming one, which it cannot do. A hand-filled "which driver do you use" field was
 * rejected: a diagnostic that invites you to believe unverified data is worse than its
 * honest absence. */
#ifdef _WIN32
static const char *const gmc_platform = "windows";
#elif defined(__APPLE__)
static const char *const gmc_platform = "macos";
#elif defined(__linux__)
static const char *const gmc_platform = "linux";
#else
static const char *const gmc_platform = "unknown";
#endif

/* Writes the run report. Split from its guard so that the collapse-edge write can reuse the
 * body without consuming the close-time write's idempotence flag. Idempotent at the
 * write_stats() level (that guard alone): called from retro_unload_game() in normal
 * operation, AND from an atexit() hook set by retro_load_game() as a safety net for the
 * macOS quit path that short-circuits retro_unload_game() (see the file header comment).
 * Does not close `client`: in the worst case (atexit without unload), the receive thread
 * is still running and gmc_stats_json() takes its mutex normally; the process terminates
 * right after, so a clean socket close is unnecessary. */
static void write_stats_body(void)
{
    /* Computed once, used twice: as elapsed_s itself, and as the denominator of runs_per_s. */
    const double elapsed_s = (double)(gmc_now_ns() - t_load) / 1e9;
    /* Sized beyond save_dir[1024] plus the "\gmc-core.json" suffix: with save_dir now a
     * fixed-size buffer in this translation unit, GCC can prove a worst-case save_dir near
     * its own capacity would truncate a 1024-byte path --
     * snprintf stays memory-safe either way, but silent truncation of the destination path
     * is a real defect, however unlikely the path length. */
    char path[1024 + 32];
    const char *p = getenv("GMC_STATS");
    if (!p) {
        /* Three-level precedence: GMC_STATS is the bench's explicit per-run path; the
         * frontend's save directory is where a user can actually find the file, and where
         * RetroArch's own menu points; the temporary directory is the last resort, taken
         * when the frontend hands back nothing usable. */
        if (have_save_dir) {
#ifdef _WIN32
            snprintf(path, sizeof path, "%s\\gmc-core.json", save_dir);
#else
            snprintf(path, sizeof path, "%s/gmc-core.json", save_dir);
#endif
        } else {
#ifdef _WIN32
            snprintf(path, sizeof path, "%s\\gmc-core.json", getenv_or("TEMP", "."));
#else
            snprintf(path, sizeof path, "%s/gmc-core.json", getenv_or("TMPDIR", "/tmp"));
#endif
        }
        p = path;
    }
    FILE *out = fopen(p, "w");
    unsigned input_hello_seen = 0;
    unsigned long long input_sent = 0, input_no_peer = 0, input_rejected = 0;
    if (!out) return;
    gmc_input_stats(input_chan, &input_hello_seen, &input_sent, &input_no_peer, &input_rejected);

    if (run_n > 1) qsort(run_us, run_n, sizeof(double), cmpd);
    if (work_n > 1) qsort(work_us, work_n, sizeof(double), cmpd);
    if (vcb_n > 1) qsort(vcb_us, vcb_n, sizeof(double), cmpd);
    if (audio_n > 1) qsort(audio_us, audio_n, sizeof(double), cmpd);
    if (copy_n > 1) qsort(copy_us, copy_n, sizeof(double), cmpd);
    if (inp_n > 1) qsort(inp_us, inp_n, sizeof(double), cmpd);
    if (poll_n > 1) qsort(poll_us, poll_n, sizeof(double), cmpd);
    if (ipoll_n > 1) qsort(ipoll_us, ipoll_n, sizeof(double), cmpd);
    if (ijoy_n > 1) qsort(ijoy_us, ijoy_n, sizeof(double), cmpd);
    if (iaxes_n > 1) qsort(iaxes_us, iaxes_n, sizeof(double), cmpd);
    if (isend_n > 1) qsort(isend_us, isend_n, sizeof(double), cmpd);
    if (frontend_input_n > 1) qsort(frontend_input_us, frontend_input_n, sizeof(double), cmpd);
    if (ft_n_s > 1) qsort(ft_us, ft_n_s, sizeof(double), cmpd);
    /* 1 when a retro_run was entered and never returned -- a total freeze the two
     * retroactive detectors cannot see. The write made from inside retro_run discounts its
     * own, still-running call. */
    const uint64_t in_flight = g_stats_write_in_run ? 1u : 0u;
    const int last_run_incomplete = runs > runs_done + in_flight;
    fprintf(out, "{\"shell\":{\"kind\":\"libretro\",\"platform\":\"%s\","
                 "\"runs\":%llu,\"fresh\":%llu,\"dupes\":%llu,\"geom_changes\":%llu,\"elapsed_s\":%.3f,"
                 "\"runs_per_s\":%.2f,"
                 "\"effective_options\":{\"groovy_depth\":%u,\"groovy_port\":%u,\"groovy_verify\":%d,"
                                       "\"groovy_status\":%d,\"groovy_input\":%d},"
                 "\"forced_by_env\":{\"groovy_depth\":%d,\"groovy_port\":%d,\"groovy_verify\":%d,"
                                   "\"groovy_status\":%d,\"groovy_input\":%d},"
                 "\"stalls\":{\"count\":%llu,\"total_ms\":%.1f},"
                 "\"driver_stalls\":{\"count\":%llu,\"total_ms\":%.1f},"
                 "\"pauses\":{\"count\":%llu,\"total_ms\":%.1f},"
                 "\"collapse\":{\"triggered\":%llu,\"total_ms\":%.1f,\"last_run_incomplete\":%d},"
                 "\"loss_hint\":{\"triggered\":%llu,\"last_window_escapes\":%llu},"
                 "\"audio_silence_ticks\":%llu,\"audio_partial_ticks\":%llu,"
                 "\"retro_run_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"retro_run_work_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"video_cb_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"frame_copy_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"input_cb_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"input_poll_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"in_have_peer_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"in_joy_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"in_axes_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"in_post_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"frontend_input_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"audio_batch_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                 "\"frontend_frame_time_us\":{\"n\":%zu,\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f,\"late_gt_25ms\":%llu},"
                 "\"input\":{\"hello_seen\":%u,\"sent\":%llu,\"no_peer\":%llu,\"rejected\":%llu,"
                 "\"reads\":%llu,\"joy_nonzero\":%llu,\"axes_nonzero\":%llu,\"joy_bits_seen\":%u,"
                 "\"mask_reads\":%llu,\"fallback_reads\":%llu}},\"client\":",
            gmc_platform,
            (unsigned long long)runs, (unsigned long long)fresh_frames, (unsigned long long)dupes,
            (unsigned long long)geom_changes, elapsed_s,
            elapsed_s > 0.0 ? (double)runs / elapsed_s : 0.0,
            cfg.target_depth, cfg.port, cfg.verify_pattern, cfg.send_status, input_enabled,
            gmc_opt_forced[GMC_OPT_DEPTH], gmc_opt_forced[GMC_OPT_PORT], gmc_opt_forced[GMC_OPT_VERIFY],
            gmc_opt_forced[GMC_OPT_STATUS], gmc_opt_forced[GMC_OPT_INPUT],
            (unsigned long long)stalls, stall_total_ms,
            (unsigned long long)driver_stalls, driver_stall_ms,
            (unsigned long long)pause_count, pause_total_ms,
            (unsigned long long)collapse_triggered, collapse_total_ms, last_run_incomplete,
            (unsigned long long)loss_hint.triggered, (unsigned long long)loss_hint.last_window,
            (unsigned long long)audio_silence_ticks,
            (unsigned long long)audio_partial_ticks,
            run_n, pct(run_us, run_n, 0.5), pct(run_us, run_n, 0.99), run_n ? run_us[run_n - 1] : 0.0,
            work_n, pct(work_us, work_n, 0.5), pct(work_us, work_n, 0.99), work_n ? work_us[work_n - 1] : 0.0,
            vcb_n, pct(vcb_us, vcb_n, 0.5), pct(vcb_us, vcb_n, 0.99), vcb_n ? vcb_us[vcb_n - 1] : 0.0,
            copy_n, pct(copy_us, copy_n, 0.5), pct(copy_us, copy_n, 0.99), copy_n ? copy_us[copy_n - 1] : 0.0,
            inp_n, pct(inp_us, inp_n, 0.5), pct(inp_us, inp_n, 0.99), inp_n ? inp_us[inp_n - 1] : 0.0,
            poll_n, pct(poll_us, poll_n, 0.5), pct(poll_us, poll_n, 0.99), poll_n ? poll_us[poll_n - 1] : 0.0,
            ipoll_n, pct(ipoll_us, ipoll_n, 0.5), pct(ipoll_us, ipoll_n, 0.99), ipoll_n ? ipoll_us[ipoll_n - 1] : 0.0,
            ijoy_n, pct(ijoy_us, ijoy_n, 0.5), pct(ijoy_us, ijoy_n, 0.99), ijoy_n ? ijoy_us[ijoy_n - 1] : 0.0,
            iaxes_n, pct(iaxes_us, iaxes_n, 0.5), pct(iaxes_us, iaxes_n, 0.99), iaxes_n ? iaxes_us[iaxes_n - 1] : 0.0,
            isend_n, pct(isend_us, isend_n, 0.5), pct(isend_us, isend_n, 0.99), isend_n ? isend_us[isend_n - 1] : 0.0,
            frontend_input_n, pct(frontend_input_us, frontend_input_n, 0.5), pct(frontend_input_us, frontend_input_n, 0.99),
                frontend_input_n ? frontend_input_us[frontend_input_n - 1] : 0.0,
            audio_n, pct(audio_us, audio_n, 0.5), pct(audio_us, audio_n, 0.99), audio_n ? audio_us[audio_n - 1] : 0.0,
            ft_n_s, pct(ft_us, ft_n_s, 0.5), pct(ft_us, ft_n_s, 0.99), ft_max_us, (unsigned long long)ft_late,
            input_hello_seen, (unsigned long long)input_sent, (unsigned long long)input_no_peer, (unsigned long long)input_rejected,
            (unsigned long long)input_reads, (unsigned long long)joy_nonzero,
            (unsigned long long)axes_nonzero, (unsigned)joy_bits_seen,
            (unsigned long long)input_mask_reads, (unsigned long long)input_fallback_reads);
    if (client) gmc_stats_json(client, out); else fprintf(out, "null");
    fprintf(out, "}\n");
    fclose(out);
    logf_(RETRO_LOG_INFO, "stats written: %s", p);
}

static void write_stats(void)
{
    if (g_stats_written) return;
    g_stats_written = 1;
    write_stats_body();
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    (void)game;
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) { logf_(RETRO_LOG_ERROR, "XRGB8888 refused by the frontend"); return false; }
    gmc_config_defaults(&cfg);
    read_options();
    /* An environment variable outranks the menu (that is what makes a bench run reproducible).
     * Say so on screen: a setting with no effect is discovered exactly when the documentation
     * is not in front of you. */
    {
        int i;
        for (i = 0; i < GMC_OPT_COUNT; i++) {
            if (!gmc_opt_forced[i]) continue;
            logf_(RETRO_LOG_INFO, "%s", gmc_opt_forced_msg[i]);
            if (i == GMC_OPT_DEPTH || i == GMC_OPT_PORT || i == GMC_OPT_INPUT)
                emit_msg(gmc_opt_forced_msg[i], 5000u, 3u, RETRO_LOG_INFO,
                         RETRO_MESSAGE_TARGET_ALL, RETRO_MESSAGE_TYPE_NOTIFICATION);
        }
    }
    {
        const char *dir = NULL;
        if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir && *dir) {
            snprintf(save_dir, sizeof save_dir, "%s", dir);
            have_save_dir = 1;
        }
    }
    /* Groovy input channel: the port is DERIVED from the port video is actually
     * configured on, never from a constant. A bind failure is NOT fatal -- the client must
     * still display even when the input channel is unavailable.
     * OPENED BEFORE gmc_open: its socket is watched by the client's receive thread, so it
     * must exist before that thread starts. The wiring goes through cfg.input_chan, never
     * through an accessor set afterwards. */
    if (input_enabled) {
        input_chan = gmc_input_open(cfg.port);
        if (input_chan)
            logf_(RETRO_LOG_INFO, "gamepad channel: listening UDP %u", cfg.port + 1);
        else
            logf_(RETRO_LOG_WARN, "gamepad channel unavailable (bind %u failed?), gamepad disabled", cfg.port + 1);
    }
    cfg.input_chan = input_chan;
    client = gmc_open(&cfg);
    if (!client) {
        logf_(RETRO_LOG_ERROR, "gmc_open failed (port %u busy?)", cfg.port);
        if (input_chan) { gmc_input_close(input_chan); input_chan = NULL; }
        return false;
    }
    struct retro_frame_time_callback ftc = { frame_time_cb, GMC_FRAME_TIME_REF_US };
    env_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, &ftc);
    fb_w = MAX_W; fb_h = MAX_H; fb = calloc((size_t)fb_w * fb_h, sizeof(uint32_t));
    t_load = gmc_now_ns();
    if (!g_atexit_registered) { atexit(write_stats); g_atexit_registered = 1; }
    logf_(RETRO_LOG_INFO, "listening UDP %u, depth %u, verify %d", cfg.port, cfg.target_depth, cfg.verify_pattern);
    return fb != NULL;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) { (void)type; (void)info; (void)num; return false; }

/* read_pad_bit -- accessor passed to gmc_input_joy_from_retropad: returns
 * nonzero if RetroPad button `id` of player 1 (the only one, player 1 shared) is pressed. */
static int read_pad_bit(unsigned id)
{
    return input_state_cb ? (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0) : 0;
}

/* scale_axis -- maps the libretro range [-32768, 32767] to int8_t by dividing by 256 with
 * saturation to [-128, 127] (written out explicitly, not assumed). */
static int8_t scale_axis(int16_t v)
{
    int32_t s = (int32_t)v / 256;
    if (s > 127) s = 127;
    if (s < -128) s = -128;
    return (int8_t)s;
}

RETRO_API void retro_run(void)
{
    const uint64_t t0 = gmc_now_ns();
    runs++;
    /* Classify the gap since the previous retro_run. Detected retroactively on the first
     * wake-up, which is the only moment a core can see it -- by then the gap is already over. */
    if (t_last_run_entry_ns != 0) {
        const uint64_t gap = t0 - t_last_run_entry_ns;
        if (gap > GMC_STALL_GAP_NS) {
            const double   gap_ms = (double)gap / 1e6;
            const uint64_t ft_ns  = (uint64_t)ft_last_us * 1000ull;
            stalls++;
            stall_total_ms += gap_ms;
            if (ft_ns * 2ull >= gap) {
                /* The frontend accounts for the gap itself: it was awake and did not iterate.
                 * Measured at 6.72 s on a real driver stall, against 64 ms on a healthy run --
                 * the half-of-the-gap test has orders of magnitude of margin either way. */
                driver_stalls++;
                driver_stall_ms += gap_ms;
                {
                    char why[96];
                    snprintf(why, sizeof why, "frontend awake but did not iterate for %.0f ms", gap_ms);
                    enter_collapse(why);
                }
            } else {
                /* The frontend was not asking us to run. Its own rate must not be judged on
                 * this time, so it leaves the denominator of the rate window. */
                frontend_idle_ms_in_window += gap_ms;
                if (ft_last_us == (retro_usec_t)GMC_FRAME_TIME_REF_US) {
                    /* Exactly our own reference handed back: the frontend locked its loop and
                     * said so. Anything else here is the frontend iterating without running us,
                     * such as an open menu -- counted, but nothing to tell the player. */
                    pause_count++;
                    pause_total_ms += gap_ms;
                    if (!g_pause_msg_shown) {
                        g_pause_msg_shown = 1;
                        logf_(RETRO_LOG_WARN, "%s", GMC_MSG_PAUSE);
                        emit_msg(GMC_MSG_PAUSE, 6000u, 3u, RETRO_LOG_WARN,
                                 RETRO_MESSAGE_TARGET_ALL, RETRO_MESSAGE_TYPE_NOTIFICATION);
                    }
                }
            }
        }
    }
    t_last_run_entry_ns = t0;
    /* input_poll_cb and input_state_cb are FRONTEND CALLBACKS, exactly like video_cb and
     * audio_batch_cb (already excluded from retro_run_work_us) -- what they cost is not a
     * core computation, it is the frontend pacing itself. Each is measured, THEN the sum of
     * the three frontend segments for this frame (poll + buttons + axes) is removed from
     * retro_run_work_us and published separately under frontend_input_us. Nothing is
     * silently dropped: gmc_input_have_peer and gmc_input_post stay in work_us, they are
     * calls to the CORE, never to the frontend. */
    double frontend_input_this_frame = 0.0;

    const uint64_t t_in0 = gmc_now_ns();
    if (input_poll_cb) input_poll_cb();
    {
        const double d = (double)(gmc_now_ns() - t_in0) / 1e3;
        push(&poll_us, &poll_n, &poll_cap, d);
        frontend_input_this_frame += d;
    }

    if (input_chan) {
        /* Split into four segments: this block carries 93 to 97 % of retro_run_work_us's
         * p99, without saying which of its parts does. gmc_input_poll exits on the first
         * EAGAIN, so a single system call per frame; reading the pad calls input_state_cb
         * FOURTEEN times (once per button, via read_pad_bit) and reading the axes FOUR
         * times -- eighteen frontend callbacks per frame, not four. */
        const uint64_t t_s0 = gmc_now_ns();
        /* No system call at all here anymore. Reception is done by the client's receive
         * thread (gmc_input_service_rx, from its own select); this only asks whether it has
         * latched onto a peer -- a lock and a read. */
        const int have_peer = gmc_input_have_peer(input_chan);
        const uint64_t t_s1 = gmc_now_ns();   /* CORE -- stays in work_us */

        /* Button-mask lever: if the frontend accepts
         * RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, un SEUL appel a input_state_cb(...,
         * RETRO_DEVICE_ID_JOYPAD_MASK) returns the sixteen bits at once, instead of the
         * fourteen calls of gmc_input_joy_from_retropad. Probed once, cached. */
        static int mask_supported = -1;
        if (mask_supported < 0) {
            mask_supported = (env_cb && env_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL)) ? 1 : 0;
            logf_(RETRO_LOG_INFO, "button mask (RETRO_ENVIRONMENT_GET_INPUT_BITMASKS): %s",
                  mask_supported ? "accepted by the frontend" : "refused by the frontend -- falling back to fourteen calls");
        }
        /* GMC_INPUT_FORCE_FALLBACK=1: DIAGNOSTIC switch only. Forces the fourteen-call
         * fallback even when the frontend accepts the mask -- exercising the path a
         * refusing frontend would take, on a frontend that accepts it. Never set it in a
         * normal-regime proof run. */
        static int force_fallback = -1;
        if (force_fallback < 0) {
            const char *e = getenv("GMC_INPUT_FORCE_FALLBACK");
            force_fallback = (e && *e == '1') ? 1 : 0;
            if (force_fallback) logf_(RETRO_LOG_INFO, "GMC_INPUT_FORCE_FALLBACK=1: fourteen-call fallback forced (diagnostic)");
        }
        const int use_mask = mask_supported && !force_fallback;

        /* The core only queries the frontend when a peer is listening. Reading the input
         * state eighteen times to send it to nobody is work the core had no reason to do --
         * an avoidable callback is a defect of the core, not a cost of the frontend. joy1
         * and axes stay at zero without a peer: gmc_input_send will not send anything
         * anyway and will count no_peer. */
        /* GMC_INPUT_ALWAYS_READ=1: DIAGNOSTIC switch only. Restores the pre-fix behavior --
         * reading the eighteen callbacks even without a peer -- to measure both states on
         * the SAME binary. Absent or 0: normal, fixed behavior. Never set it in a proof
         * run. */
        static int always_read = -1;
        if (always_read < 0) { const char *e = getenv("GMC_INPUT_ALWAYS_READ"); always_read = (e && *e == '1') ? 1 : 0; }
        const int do_read = have_peer || always_read;
        uint16_t joy1 = 0u;
        int8_t axes[8];
        /* player 1 shared: joy2 and player 2's axes stay at zero */
        memset(axes, 0, sizeof axes);
        if (do_read) {
            if (use_mask && input_state_cb) {
                const uint16_t mask = (uint16_t)input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
                joy1 = gmc_input_joy_from_mask(mask);
                input_mask_reads++;
            } else {
                joy1 = gmc_input_joy_from_retropad(read_pad_bit);
                input_fallback_reads++;
            }
        }
        const uint64_t t_s2 = gmc_now_ns();
        if (do_read && input_state_cb) {
            axes[0] = scale_axis((int16_t)input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X));
            axes[1] = scale_axis((int16_t)input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y));
            axes[2] = scale_axis((int16_t)input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
            axes[3] = scale_axis((int16_t)input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));
        }
        if (do_read) {
            input_reads++;
            if (joy1 != 0u) { joy_nonzero++; joy_bits_seen = (uint16_t)(joy_bits_seen | joy1); }
            if (axes[0] || axes[1] || axes[2] || axes[3]) axes_nonzero++;
        }
        const uint64_t t_s3 = gmc_now_ns();
        /* Sent on every retro_run, even when nothing is pressed -- exactly like
         * the hardware receiver's own input handling, which emits every frame: the
         * fork's mister driver expects a regular stream, emitting only on change
         * would produce sticky inputs. */
        /* This DEPOSITS, it does not send. The receive thread will emit it, on the next
         * video frame or at its select's delay. No system call. */
        gmc_input_post(input_chan, (uint32_t)runs, joy1, axes);
        const uint64_t t_s4 = gmc_now_ns();
        push(&ipoll_us, &ipoll_n, &ipoll_cap, (double)(t_s1 - t_s0) / 1e3);   /* CORE */
        push(&ijoy_us,  &ijoy_n,  &ijoy_cap,  (double)(t_s2 - t_s1) / 1e3);   /* FRONTEND (1 or 14 calls) */
        push(&iaxes_us, &iaxes_n, &iaxes_cap, (double)(t_s3 - t_s2) / 1e3);   /* FRONTEND (4 calls) */
        push(&isend_us, &isend_n, &isend_cap, (double)(t_s4 - t_s3) / 1e3);   /* CORE */
        frontend_input_this_frame += (double)(t_s2 - t_s1) / 1e3 + (double)(t_s3 - t_s2) / 1e3;
    }
    push(&inp_us, &inp_n, &inp_cap, (double)(gmc_now_ns() - t_in0) / 1e3);
    push(&frontend_input_us, &frontend_input_n, &frontend_input_cap, frontend_input_this_frame);

    gmc_frame *f = NULL;
    uint64_t t_vcb0, t_vcb1;
    const int fresh = client ? gmc_acquire(client, &f) : 0;
    if (fresh) {
        if (f->width != cur_w || f->height != cur_h) {
            cur_w = f->width; cur_h = f->height;
            struct retro_game_geometry g = { cur_w, cur_h, MAX_W, MAX_H, 4.0f / 3.0f };
            env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
            geom_changes++;
            logf_(RETRO_LOG_INFO, "geometry %ux%u interlaced=%u (generation %u)", cur_w, cur_h, f->interlaced, f->generation);
        }
        /* This copy is the only large item in the segment measured by retro_run_work_us,
         * once lock contention is ruled out by measurement (acquire_wait_us p99 = 1 us).
         * 307 KB in 240p, 573 KB in 480i, per frame. */
        const uint64_t t_cp0 = gmc_now_ns();
        for (unsigned y = 0; y < cur_h; y++) memcpy(fb + (size_t)y * cur_w, f->xrgb + (size_t)y * cur_w, (size_t)cur_w * 4);
        push(&copy_us, &copy_n, &copy_cap, (double)(gmc_now_ns() - t_cp0) / 1e3);
        t_vcb0 = gmc_now_ns();
        video_cb(fb, cur_w, cur_h, (size_t)cur_w * 4);
        t_vcb1 = gmc_now_ns();
        gmc_note_present(client, f, t_vcb1);
        gmc_release(client, f);
        fresh_frames++;
    } else {
        t_vcb0 = gmc_now_ns();
        video_cb(NULL, cur_w, cur_h, (size_t)cur_w * 4);   /* dupe: the frontend re-presents */
        t_vcb1 = gmc_now_ns();
        if (client) gmc_note_repeat(client, t_vcb1);
        dupes++;
    }
    /* Rate judge. Only this second form needs a window: a driver that iterates regularly but
     * too slowly produces no gap at all (20.0 runs/s with a worst frame of 32.9 ms measured).
     * Time the frontend owns -- pause, menu -- is removed from the denominator, so a player who
     * alt-tabs is never mistaken for a slow driver. Driver stalls are NOT removed: they are
     * exactly what a low rate is meant to report. */
    if (t_win_start_ns == 0) {
        t_win_start_ns = t0; runs_win_start = runs; fresh_win_start = fresh_frames;
        frontend_idle_ms_in_window = 0.0;
        /* Arm the loss-hint detector on the same reference window as the rate
         * judge: the first call only records a starting point, never fires. */
        if (client) (void)gmc_loss_hint_window(&loss_hint, gmc_cont_escape(client));
    } else if (t0 - t_win_start_ns >= GMC_RATE_WINDOW_NS) {
        const double active_s = (double)(t0 - t_win_start_ns) / 1e9
                              - frontend_idle_ms_in_window / 1e3;
        if (active_s > 0.0) {
            const double rate = (double)(runs - runs_win_start) / active_s;
            if (!collapsed) {
                if (rate < GMC_COLLAPSE_ENTER_FPS && fresh_frames > fresh_win_start) {
                    char why[96];
                    snprintf(why, sizeof why, "%.1f runs/s over %.1f s of active time",
                             rate, active_s);
                    enter_collapse(why);
                }
            } else {
                collapse_total_ms += active_s * 1000.0;
                if (rate >= GMC_COLLAPSE_EXIT_FPS) collapsed = 0;
            }
        }
        /* This only speaks; no automatic guard exists. One log line per
         * episode (rising edge) and one OSD message per window while it lasts --
         * never one per frame. */
        if (client) {
            /* Name the remedy that applies -- audio=off only if there is sound. */
            const int with_sound = gmc_audio_announced(client);
            const char *loss_msg =
                (gmc_loss_hint_advice(with_sound) == GMC_LOSS_ADVICE_AUDIO_OFF)
                    ? GMC_MSG_LOSS : GMC_MSG_LOSS_LINK;
            if (gmc_loss_hint_window(&loss_hint, gmc_cont_escape(client))) {
                logf_(RETRO_LOG_WARN, "lossy link: %llu continuation escapes in the last window (sound announced: %s)",
                      (unsigned long long)loss_hint.last_window, with_sound ? "yes" : "no");
                emit_msg(loss_msg, GMC_LOSS_MSG_MS, 2u, RETRO_LOG_WARN,
                         RETRO_MESSAGE_TARGET_OSD, RETRO_MESSAGE_TYPE_NOTIFICATION);
            } else if (loss_hint.active) {
                emit_msg(loss_msg, GMC_LOSS_MSG_MS, 2u, RETRO_LOG_WARN,
                         RETRO_MESSAGE_TARGET_OSD, RETRO_MESSAGE_TYPE_NOTIFICATION);
            }
        }
        t_win_start_ns = t0; runs_win_start = runs; fresh_win_start = fresh_frames;
        frontend_idle_ms_in_window = 0.0;
    }
    if (collapsed)
        emit_msg(GMC_MSG_COLLAPSE, GMC_COLLAPSE_MSG_MS, 3u, RETRO_LOG_WARN,
                 RETRO_MESSAGE_TARGET_OSD, RETRO_MESSAGE_TYPE_STATUS);
    const uint64_t t_audio0 = gmc_now_ns();
    /* Sound. Delivers whatever the stream has provided; if it provided nothing,
     * still delivers a tick of silence, because it is the frontend's audio sync that
     * paces retro_run at 60 Hz -- delivering nothing would let the core run free. */
    {
        /* EXACTLY one tick per retro_run, never more.
         *
         * Before: up to 4,096 frames were pulled at once, i.e. HALF the ring and 85 ms of
         * sound. Under audio_sync = true, the frontend must consume everything pushed to it
         * before returning control: every giant push blocked it (audio_batch_us measured at
         * 81 ms at the maximum, for a maximum pull of 4,006 frames), the ring emptied, fell
         * back under GMC_AUDIO_PREROLL, and dozens of retro_run calls pushed nothing but
         * silence while it refilled -- then the cycle started over. Measured before the fix:
         * pulls at p50 = 1,601 and max = 4,006 frames, 1,089 silence ticks out of 2,116
         * calls, a rate of 44.3 frames/s.
         *
         * After: one tick is requested (800 frames at 48 kHz / 60 Hz) and not one more, and
         * only the SHORTFALL is filled with silence. The ring stays the jitter buffer --
         * what it holds in excess stays there instead of being dumped onto the frontend --
         * and it is the frontend's own audio_rate_control that absorbs clock drift, its own
         * role. audio_sync is NOT touched: it is what paces retro_run at 60 Hz. */
        static int16_t pcm[2 * GMC_AUDIO_TICK_FRAMES];
        size_t got = client ? gmc_audio_read(client, pcm, GMC_AUDIO_TICK_FRAMES) : 0u;
        if (got < (size_t)GMC_AUDIO_TICK_FRAMES) {
            memset(pcm + got * 2u, 0,
                   ((size_t)GMC_AUDIO_TICK_FRAMES - got) * 2u * sizeof(int16_t));
            if (got == 0u) audio_silence_ticks++;   /* fully silent tick: the ring had nothing */
            else           audio_partial_ticks++;   /* tick topped up with silence: the ring had too little */
        }
        if (audio_batch_cb) audio_batch_cb(pcm, (size_t)GMC_AUDIO_TICK_FRAMES);
    }
    const uint64_t t_end = gmc_now_ns();
    push(&run_us, &run_n, &run_cap, (double)(t_end - t0) / 1e3);
    /* `work_us` used to include (t_end - t_vcb1), i.e. the time spent INSIDE
     * audio_batch_cb. On macOS/CoreAudio3, this block commonly costs ~20 ms, which made
     * the p99 < 1 ms criterion fail even though the core's REAL work (before video_cb:
     * input_poll_cb, gmc_acquire, memcpy) stays under a hundred microseconds.
     * audio_batch_cb, like video_cb, is a FRONTEND pacing mechanism (here via the audio
     * buffer rather than vsync) -- not a core computation cost. `work_us` therefore now
     * only measures the segment BEFORE video_cb; `audio_batch_us`, new, isolates this
     * block so it stays visible, not hidden. `retro_run_us` (the total) has not changed
     * and still reveals the gap.
     *
     * The segment (t_vcb0 - t0) still contained input_poll_cb and the input_state_cb
     * callbacks that read the pad (buttons + axes) -- the same frontend callbacks as
     * above, pacing, not computation. frontend_input_this_frame (measured above,
     * published separately) is now REMOVED from it here. gmc_input_have_peer and
     * gmc_input_post, core code, stay in work_us. */
    push(&work_us, &work_n, &work_cap, (double)(t_vcb0 - t0) / 1e3 - frontend_input_this_frame);
    push(&vcb_us, &vcb_n, &vcb_cap, (double)(t_vcb1 - t_vcb0) / 1e3);
    push(&audio_us, &audio_n, &audio_cap, (double)(t_end - t_audio0) / 1e3);
    runs_done++;   /* last statement: this call returned */
}

RETRO_API void retro_unload_game(void)
{
    write_stats();
    if (client) { gmc_close(client); client = NULL; }
    if (input_chan) { gmc_input_close(input_chan); input_chan = NULL; }
    free(fb); fb = NULL;
    free(run_us); run_us = NULL; run_n = run_cap = 0;
    free(ft_us); ft_us = NULL; ft_n_s = ft_cap = 0;
    free(work_us); work_us = NULL; work_n = work_cap = 0;
    free(vcb_us); vcb_us = NULL; vcb_n = vcb_cap = 0;
    free(audio_us); audio_us = NULL; audio_n = audio_cap = 0;
    free(frontend_input_us); frontend_input_us = NULL; frontend_input_n = frontend_input_cap = 0;
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
