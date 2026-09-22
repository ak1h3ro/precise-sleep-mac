/*
 * precise_run — Steam launch wrapper.
 *
 * Usage (Steam launch options):   /path/to/precise_run %command%
 *
 * Prepends precise_sleep.dylib (found next to this executable) to
 * DYLD_INSERT_LIBRARIES — keeping whatever Steam already put there, such as its
 * overlay — resolves a .app bundle to the executable inside it, and exec()s the
 * game in place (same PID, so Steam keeps tracking it).
 *
 * This has to be a compiled, non-platform binary: macOS strips DYLD_* variables
 * from the environment of platform binaries such as /bin/sh, which would drop
 * Steam's overlay as well as our library.
 *
 *   precise_run --dry-run <game>   print what would be exec'd, do not launch
 *   precise_run --selftest         load the library into this process and print timings
 */
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>

#define LIB_NAME "precise_sleep.dylib"
#define LOG_NAME "precise_run.log"
#define LOG_MAX  (64 * 1024)   /* truncate the log when it grows past this */

static char self_dir[PATH_MAX];

static int find_self_dir(void)
{
    char raw[PATH_MAX]; uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0) return -1;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return -1;
    char *slash = strrchr(real, '/');
    if (!slash) return -1;
    *slash = '\0';
    snprintf(self_dir, sizeof self_dir, "%s", real);
    return 0;
}

static FILE *open_log(void)
{
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/%s", self_dir, LOG_NAME);
    struct stat st;
    const char *mode = (stat(path, &st) == 0 && st.st_size > LOG_MAX) ? "w" : "a";
    return fopen(path, mode);
}

static void logf_(const char *fmt, ...)
{
    FILE *log = open_log(); if (!log) return;
    time_t t = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(log, "%s ", ts);
    va_list ap; va_start(ap, fmt); vfprintf(log, fmt, ap); va_end(ap);
    fputc('\n', log); fclose(log);
}

static int is_executable_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0;
}

/* Steam passes the .app bundle directory; resolve it to the real executable inside. */
static const char *resolve_target(const char *target, char *out, size_t outsz)
{
    size_t n = strlen(target);
    while (n > 1 && target[n - 1] == '/') n--;                 /* tolerate trailing slash */
    if (n <= 4 || strncmp(target + n - 4, ".app", 4) != 0) return target;

    char app[PATH_MAX]; snprintf(app, sizeof app, "%.*s", (int)n, target);
    const char *base = strrchr(app, '/'); base = base ? base + 1 : app;

    /* 1. <App>.app/Contents/MacOS/<App> */
    snprintf(out, outsz, "%s/Contents/MacOS/%.*s", app, (int)(strlen(base) - 4), base);
    if (is_executable_file(out)) return out;

    /* 2. the only executable in Contents/MacOS */
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s/Contents/MacOS", app);
    DIR *d = opendir(dir); if (!d) return target;
    struct dirent *e; int found = 0; char cand[PATH_MAX] = "";
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_executable_file(p)) { if (++found == 1) snprintf(cand, sizeof cand, "%s", p); }
    }
    closedir(d);
    if (found == 1) { snprintf(out, outsz, "%s", cand); return out; }
    return target;
}

int main(int argc, char **argv)
{
    if (find_self_dir() != 0) { fprintf(stderr, "precise_run: cannot locate own directory\n"); return 1; }
    char lib[PATH_MAX]; snprintf(lib, sizeof lib, "%s/%s", self_dir, LIB_NAME);

    int dry = 0, selftest = 0, ai = 1;
    if (argc > ai && !strcmp(argv[ai], "--dry-run"))  { dry = 1; ai++; }
    if (argc > ai && !strcmp(argv[ai], "--selftest")) { selftest = 1; ai++; }

    if (access(lib, R_OK) != 0) {
        fprintf(stderr, "precise_run: %s not found next to the launcher\n", LIB_NAME);
        logf_("ERROR library missing: %s", lib);
        if (!dry && argc > ai) { execv(argv[ai], argv + ai); perror("precise_run: execv"); }
        return 1;
    }

    const char *old = getenv("DYLD_INSERT_LIBRARIES");
    char env[8192];
    if (old && *old) snprintf(env, sizeof env, "%s:%s", lib, old);
    else             snprintf(env, sizeof env, "%s", lib);

    if (selftest) {
        if (getenv("PRECISE_RUN_CHILD")) {                    /* we are the re-exec'd child */
            struct timespec t = { 0, 16666667 };
            for (int i = 0; i < 3; i++) nanosleep(&t, NULL);  /* the library's constructor already printed */
            return 0;
        }
        setenv("DYLD_INSERT_LIBRARIES", env, 1);
        setenv("DST_PRECISE_SLEEP_TEST", "1", 1);
        setenv("PRECISE_RUN_CHILD", "1", 1);
        char self[PATH_MAX]; snprintf(self, sizeof self, "%s/precise_run", self_dir);
        char *args[] = { self, "--selftest", NULL };
        execv(self, args); perror("precise_run: execv self"); return 1;
    }

    if (argc <= ai) { fprintf(stderr, "usage: precise_run [--dry-run] <game.app | executable> [args...]\n"); return 2; }

    char resolved[PATH_MAX];
    const char *target = resolve_target(argv[ai], resolved, sizeof resolved);

    if (dry) {
        printf("launcher dir : %s\nlibrary      : %s\nDYLD_INSERT_LIBRARIES=%s\nexec         : %s\n", self_dir, lib, env, target);
        for (int i = ai + 1; i < argc; i++) printf("arg          : %s\n", argv[i]);
        return 0;
    }

    setenv("DYLD_INSERT_LIBRARIES", env, 1);
    logf_("DYLD_INSERT_LIBRARIES=%s", env);
    logf_("exec %s (from %s)", target, argv[ai]);

    argv[ai] = (char *)target;
    execv(target, argv + ai);
    perror("precise_run: execv");
    logf_("ERROR execv %s: %s", target, strerror(errno));
    return 1;
}
