/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_frozen_run -- the frozen-run detector: the core's report says a
 * retro_run was entered and never returned.
 *
 * The core's two collapse detectors are retroactive: they only speak on the
 * return of a LATER retro_run call. A total freeze (video_cb never gives
 * back control) leaves them both at zero, and the report reads like a clean
 * run. The shell.collapse.last_run_incomplete field must say so.
 *
 * This harness plays the frontend: it loads the compiled core, calls
 * retro_run from a thread, and makes video_cb block forever on the second
 * call -- exactly the pattern seen on a real frozen run (runs = 2,
 * video_cb_us.n = 1). The main thread then calls exit(): the core's
 * atexit() safety net writes the report.
 *
 *   test_frozen_run freeze <core> <report.json>  -> freezes, then exits via exit()
 *   test_frozen_run check <report.json> <expected> -> reads back the field
 *   test_frozen_run clean  <core> <report.json>  -> ten complete calls,
 *                          retro_unload_game, reads back: expects 0
 *   test_frozen_run smoke <core> <report.json> <seconds> -> a minimal
 *                          frontend for a headless machine: loads the core,
 *                          drives it like a real frontend for <seconds>
 *                          seconds against a real stream, and reports
 *                          whether picture and AUDIBLE sound actually
 *                          arrived (a fallback tick of silence is not
 *                          sound -- see smoke_audio_batch_cb below; drive
 *                          the emitter against a game with audible sound,
 *                          not a silent menu, for this verdict to prove
 *                          anything about audio)
 *
 * The freeze and its readback are two separate processes: on Windows, a
 * DLL's atexit() hooks only run at its unload, after the program's own; a
 * reader in the same process would read before the write.
 * The core listens on GMC_PORT: 42100 by default for freeze/check/clean
 * (chosen so this harness never collides with a real receiver on the
 * protocol's own port), with no input channel. smoke leaves the input
 * channel on -- as a real frontend would -- and defaults to 32100, the
 * protocol's own port, since it plays against a real stream rather than a
 * synthetic one.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#define LIB_OPEN(p)    ((void *)LoadLibraryA(p))
#define LIB_SYM(h, s)  ((void *)GetProcAddress((HMODULE)(h), s))
static void sleep_ms(unsigned ms) { Sleep(ms); }
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <dlfcn.h>
#include <time.h>
#define LIB_OPEN(p)    dlopen(p, RTLD_NOW)
#define LIB_SYM(h, s)  dlsym(h, s)
static void sleep_ms(unsigned ms)
{
    struct timespec t = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&t, NULL);
}
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

#include "libretro.h"

static bool freeze_mode;
static volatile unsigned video_calls;
static pthread_mutex_t never_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  never_cv = PTHREAD_COND_INITIALIZER;

static bool env_cb(unsigned cmd, void *data)
{
    (void)data;
    return cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT;
}

static void video_cb(const void *d, unsigned w, unsigned h, size_t pitch)
{
    (void)d; (void)w; (void)h; (void)pitch;
    video_calls++;
    if (freeze_mode && video_calls == 2) {
        /* The frontend that never presents again: blocks forever. */
        pthread_mutex_lock(&never_mx);
        for (;;) pthread_cond_wait(&never_cv, &never_mx);
    }
}

static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id)
{
    (void)p; (void)d; (void)i; (void)id;
    return 0;
}

static void (*p_run)(void);

static void *run_thread(void *arg)
{
    int n = *(int *)arg;
    for (int i = 0; i < n; ++i) {
        p_run();
        sleep_ms(16);
    }
    return NULL;
}

static int check_report(const char *path, int expected)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: report missing (%s)\n", path); return 3; }
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *k = strstr(buf, "\"last_run_incomplete\":");
    if (!k) { printf("FAIL: last_run_incomplete field missing\n"); return 3; }
    int got = atoi(k + strlen("\"last_run_incomplete\":"));
    const char *runs = strstr(buf, "\"runs\":");
    printf("%s: last_run_incomplete=%d (expected %d), %.14s\n",
           got == expected ? "OK" : "FAIL", got, expected, runs ? runs : "?");
    return got == expected ? 0 : 1;
}

/* The nine libretro entry points every mode needs, resolved together: the
 * freeze/clean path and smoke load the exact same core, they only differ in
 * what they wire to it and how long they drive it. retro_run itself stays in
 * the file-scope p_run used by run_thread. */
struct core_api {
    void (*set_env)(retro_environment_t);
    void (*set_video)(retro_video_refresh_t);
    void (*set_audio)(retro_audio_sample_t);
    void (*set_batch)(retro_audio_sample_batch_t);
    void (*set_poll)(retro_input_poll_t);
    void (*set_state)(retro_input_state_t);
    void (*init)(void);
    bool (*load)(const struct retro_game_info *);
    void (*unload)(void);
    void (*deinit)(void);
};

static void *load_core(const char *path, struct core_api *api)
{
    void *h = LIB_OPEN(path);
    if (!h) { fprintf(stderr, "failed to load core: %s\n", path); return NULL; }
    api->set_env   = (void (*)(retro_environment_t))LIB_SYM(h, "retro_set_environment");
    api->set_video = (void (*)(retro_video_refresh_t))LIB_SYM(h, "retro_set_video_refresh");
    api->set_audio = (void (*)(retro_audio_sample_t))LIB_SYM(h, "retro_set_audio_sample");
    api->set_batch = (void (*)(retro_audio_sample_batch_t))LIB_SYM(h, "retro_set_audio_sample_batch");
    api->set_poll  = (void (*)(retro_input_poll_t))LIB_SYM(h, "retro_set_input_poll");
    api->set_state = (void (*)(retro_input_state_t))LIB_SYM(h, "retro_set_input_state");
    api->init      = (void (*)(void))LIB_SYM(h, "retro_init");
    api->load      = (bool (*)(const struct retro_game_info *))LIB_SYM(h, "retro_load_game");
    api->unload    = (void (*)(void))LIB_SYM(h, "retro_unload_game");
    api->deinit    = (void (*)(void))LIB_SYM(h, "retro_deinit");
    p_run          = (void (*)(void))LIB_SYM(h, "retro_run");
    if (!api->set_env || !api->set_video || !api->set_audio || !api->set_batch || !api->set_poll
        || !api->set_state || !api->init || !api->load || !api->unload || !api->deinit || !p_run) {
        fprintf(stderr, "libretro symbol missing\n");
        return NULL;
    }
    return h;
}

/* smoke's own callbacks: a real frontend counts frames it can actually show,
 * not ones it merely receives. video_cb(NULL, ...) above is the dupe path
 * the core takes when nothing new arrived (see crt_bridge_libretro.c's own
 * video_cb call) and must not count as fresh. The single-sample audio_cb
 * above is reused unchanged: the core never calls it, only the batch one.
 *
 * smoke_audio_nonzero counts frames that carry at least one non-zero
 * sample. The core delivers a fallback tick of silence on every retro_run
 * even with no emitter reachable at all (see crt_bridge_libretro.c's own
 * comment on GMC_AUDIO_TICK_FRAMES): smoke_audio_frames alone grows on
 * silence just as it would on real sound, so it cannot tell "sound
 * received" from "no sound, filled with silence" -- exactly what smoke is
 * meant to prove for the plan that reads this verdict. A run against a
 * silent source (a menu, or no emitter at all) is expected to leave
 * smoke_audio_nonzero at 0 and this mode's own exit code at 1; drive smoke
 * against a game that plays audible sound to get a real proof. */
static uint64_t smoke_fresh;
static uint64_t smoke_audio_frames;
static uint64_t smoke_audio_nonzero;

static void smoke_video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    (void)w; (void)h; (void)pitch;
    if (data) smoke_fresh++;
}

static size_t smoke_audio_batch_cb(const int16_t *data, size_t frames)
{
    smoke_audio_frames += frames;
    if (data)
        for (size_t i = 0; i < frames; i++)
            if (data[2 * i] != 0 || data[2 * i + 1] != 0) smoke_audio_nonzero++;
    return frames;
}

static void smoke_input_poll_cb(void) {}

static int16_t smoke_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)port; (void)device; (void)index; (void)id;
    return 0;
}

/* smoke -- a minimal frontend for a headless machine: it loads the core,
 * drives it like a real frontend for <seconds> seconds against a real
 * stream, and says whether picture and sound actually arrived. Unlike
 * freeze/check/clean, GMC_INPUT is left untouched (the input channel stays
 * on, as it would for a real frontend) and GMC_PORT defaults to 32100, the
 * protocol's own port. */
static int smoke_mode(const char *core, const char *report, int seconds)
{
    if (seconds <= 0) {
        fprintf(stderr, "smoke: <seconds> must be a positive integer\n");
        return 2;
    }
    remove(report);
    set_env("GMC_STATS", report);
    if (!getenv("GMC_PORT")) set_env("GMC_PORT", "32100");

    struct core_api api;
    void *h = load_core(core, &api);
    if (!h) return 2;
    api.set_env(env_cb);
    api.set_video(smoke_video_cb);
    api.set_audio(audio_cb);
    api.set_batch(smoke_audio_batch_cb);
    api.set_poll(smoke_input_poll_cb);
    api.set_state(smoke_input_state_cb);
    api.init();
    if (!api.load(NULL)) { fprintf(stderr, "retro_load_game failed\n"); return 2; }

    int runs = 0;
    const int total_runs = (seconds * 1000) / 16;
    for (int i = 0; i < total_runs; i++) {
        p_run();
        runs++;
        sleep_ms(16);
    }

    api.unload();
    api.deinit();

    printf("smoke: seconds=%d fresh=%llu audio_frames=%llu audio_nonzero=%llu runs=%d\n",
           seconds, (unsigned long long)smoke_fresh, (unsigned long long)smoke_audio_frames,
           (unsigned long long)smoke_audio_nonzero, runs);
    return (smoke_fresh >= (uint64_t)(10 * seconds) && smoke_audio_nonzero > 0) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "check") == 0)
        return check_report(argv[2], atoi(argv[3]));
    if (argc == 5 && strcmp(argv[1], "smoke") == 0)
        return smoke_mode(argv[2], argv[3], atoi(argv[4]));
    if (argc != 4 || (strcmp(argv[1], "freeze") && strcmp(argv[1], "clean"))) {
        fprintf(stderr, "usage: freeze|clean <core> <report.json>\n"
                        "   or: check <report.json> <expected>\n"
                        "   or: smoke <core> <report.json> <seconds>\n");
        return 2;
    }
    freeze_mode = strcmp(argv[1], "freeze") == 0;
    const char *core = argv[2];
    const char *report = argv[3];
    remove(report);
    set_env("GMC_STATS", report);
    set_env("GMC_INPUT", "0");
    if (!getenv("GMC_PORT")) set_env("GMC_PORT", "42100");

    struct core_api api;
    void *h = load_core(core, &api);
    if (!h) return 2;
    api.set_env(env_cb);
    api.set_video(video_cb);
    api.set_audio(audio_cb);
    api.set_batch(audio_batch_cb);
    api.set_poll(input_poll_cb);
    api.set_state(input_state_cb);
    api.init();
    if (!api.load(NULL)) { fprintf(stderr, "retro_load_game failed\n"); return 2; }

    int n = freeze_mode ? 1000 : 10;
    pthread_t t;
    pthread_create(&t, NULL, run_thread, &n);
    if (freeze_mode) {
        /* Let the freeze settle in, then exit like a frontend being closed:
         * exit() triggers the core's atexit() safety net, which writes the
         * report. The readback is the next `check` invocation. */
        for (int i = 0; i < 200 && video_calls < 2; ++i) sleep_ms(10);
        if (video_calls < 2) { printf("FAIL: the freeze did not settle in\n"); return 1; }
        sleep_ms(300);
        exit(0);
    }
    pthread_join(t, NULL);
    api.unload();
    return check_report(report, 0);
}
