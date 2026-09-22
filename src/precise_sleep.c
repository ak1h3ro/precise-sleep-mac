/*
 * precise_sleep.dylib v2 — makes nanosleep() land on its deadline inside one process.
 *
 * macOS timer coalescing delivers sleep wake-ups late: ~1 ms for a foreground
 * app, up to 100 ms for a process in the Darwin-background band (pri 4) — which
 * is where Game Mode puts the DST server shards while the client is fullscreen.
 * A sleep-based frame limiter (client) or tick loop (shards) then overshoots.
 *
 * v1 waited with mach_wait_until() until (deadline - margin) and busy-waited
 * the rest. mach_wait_until() is subject to the caller's coalescing tier, so
 * under darwinbg the margin would have had to be ~100 ms — impossible.
 *
 * v2 waits on a kqueue EVFILT_TIMER armed with NOTE_CRITICAL. In XNU that flag
 * selects a zero-leeway timer tier regardless of the process's scheduling
 * policy (measured: 0.03 ms late at pri 4). The adaptive spin margin is kept
 * only as a safety net and now settles at ~0.1-0.2 ms, so CPU spent spinning
 * is ~1 % of a core instead of ~6 %. If kqueue is unavailable it falls back to
 * the v1 mach_wait_until path.
 *
 * Loaded with DYLD_INSERT_LIBRARIES; affects only the process it is loaded into.
 */
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/event.h>
#include <mach/mach_time.h>

#define MARGIN_MIN_NS    100000ull   /* 0.1 ms */
#define MARGIN_MAX_NS   8000000ull   /* 8 ms   */
#define MARGIN_PAD_NS    100000ull   /* headroom above observed overshoot */

static mach_timebase_info_data_t tb;
static uint64_t margin_ns = 300000ull;       /* start at 0.3 ms */
static pthread_key_t kq_key;
static pthread_once_t kq_once = PTHREAD_ONCE_INIT;

static uint64_t abs_to_ns(uint64_t t) { return t * tb.numer / tb.denom; }
static uint64_t ns_to_abs(uint64_t ns) { return ns * tb.denom / tb.numer; }

static void kq_close(void *p) { int fd = (int)(intptr_t)p - 1; if (fd >= 0) close(fd); }
static void kq_key_init(void) { pthread_key_create(&kq_key, kq_close); }

/* one kqueue per thread, created lazily; stored as fd+1 so NULL means "none yet" */
static int thread_kq(void)
{
    pthread_once(&kq_once, kq_key_init);
    intptr_t v = (intptr_t)pthread_getspecific(kq_key);
    if (v > 0) return (int)v - 1;
    int fd = kqueue();
    if (fd < 0) return -1;
    pthread_setspecific(kq_key, (void *)(intptr_t)(fd + 1));
    return fd;
}

/* wait until an absolute mach time using a NOTE_CRITICAL timer; returns 0 on success */
static int critical_wait_until(uint64_t deadline_abs)
{
    int kq = thread_kq();
    if (kq < 0) return -1;
    for (;;) {
        if (mach_absolute_time() >= deadline_abs) return 0;
        struct kevent64_s ke, out;
        EV_SET64(&ke, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
                 NOTE_MACHTIME | NOTE_ABSOLUTE | NOTE_CRITICAL, (int64_t)deadline_abs, 0, 0, 0);
        int r = kevent64(kq, &ke, 1, &out, 1, 0, NULL);
        if (r >= 0) return 0;              /* timer fired (r==1) */
        if (errno == EINTR) continue;      /* signal: re-arm and keep waiting */
        return -1;                         /* EINVAL etc.: let caller fall back */
    }
}

int precise_nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (tb.denom == 0) mach_timebase_info(&tb);
    if (req == NULL || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    uint64_t want_ns  = (uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec;
    uint64_t deadline = mach_absolute_time() + ns_to_abs(want_ns);

    if (want_ns > margin_ns) {
        uint64_t coarse_deadline = deadline - ns_to_abs(margin_ns);
        if (critical_wait_until(coarse_deadline) != 0)
            mach_wait_until(coarse_deadline);    /* fallback: v1 behaviour */
        uint64_t now = mach_absolute_time();
        /* adapt: how late did the kernel wake us? */
        uint64_t late_ns = now > coarse_deadline ? abs_to_ns(now - coarse_deadline) : 0;
        uint64_t target  = late_ns + MARGIN_PAD_NS;
        if (target > margin_ns)       margin_ns = target;                    /* react fast to lateness */
        else                          margin_ns -= (margin_ns - target) / 16; /* decay slowly */
        if (margin_ns < MARGIN_MIN_NS) margin_ns = MARGIN_MIN_NS;
        if (margin_ns > MARGIN_MAX_NS) margin_ns = MARGIN_MAX_NS;
    }
    while (mach_absolute_time() < deadline) {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#else
        __asm__ volatile("yield");
#endif
    }
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

/* dyld interposing table: {replacement, original} */
__attribute__((used)) static const struct { const void *replacement; const void *replacee; }
interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)precise_nanosleep, (const void *)nanosleep },
};

/* optional self-check: DST_PRECISE_SLEEP_TEST=1 prints measured timing at load */
__attribute__((constructor)) static void selftest(void)
{
    if (!getenv("DST_PRECISE_SLEEP_TEST")) return;
    if (tb.denom == 0) mach_timebase_info(&tb);
    struct timespec t = { 0, 16666667 };
    fprintf(stderr, "[precise_sleep v2] %s pid %d\n", getprogname(), getpid());
    for (int i = 0; i < 8; i++) {
        uint64_t a = mach_absolute_time();
        precise_nanosleep(&t, NULL);
        uint64_t b = mach_absolute_time();
        fprintf(stderr, "[precise_sleep v2] asked 16.667 ms, got %.3f ms (margin now %.2f ms)\n",
                abs_to_ns(b - a) / 1e6, margin_ns / 1e6);
    }
}
