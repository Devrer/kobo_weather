#include "sysutil.h"
#include "system_ops.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

extern bool g_debug;

/* Both primitives funnel through the injectable SystemOps seam (system_ops.h). */
int sysutil_runf(const char *cmd) {
    return system_ops()->runf(cmd);
}

int sysutil_write_sysfs(const char *path, const char *val) {
    return system_ops()->write_sysfs(path, val);
}

void sysutil_log(const char *prefix, bool always, const char *fmt, ...) {
    if (!always && !g_debug) return;
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char msg[256];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    fprintf(stderr, "%02d:%02d:%02d DIAG: %s%s\n",
            tm.tm_hour, tm.tm_min, tm.tm_sec, prefix, msg);
    fflush(stderr);
}

const char *sysutil_exe_dir(void) {
    /* Resolved once and cached. The child runs app_main() by direct call (no
     * exec), so /proc/self/exe is the same binary in supervisor and app. */
    static char dir[PATH_MAX];
    if (dir[0]) return dir;
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) { dir[0] = '.'; dir[1] = '\0'; return dir; }
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (slash && slash != path) {
        *slash = '\0';
        snprintf(dir, sizeof dir, "%s", path);
    } else {
        dir[0] = '.'; dir[1] = '\0';   /* exe at filesystem root or no slash */
    }
    return dir;
}

const char *sysutil_path(char *out, size_t n, const char *leaf) {
    snprintf(out, n, "%s/%s", sysutil_exe_dir(), leaf);
    return out;
}
