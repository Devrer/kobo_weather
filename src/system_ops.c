#include "system_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

/* default (real-hardware) implementation */

static int default_runf(const char *cmd) {
    int st = system(cmd);
    return (st == -1) ? -1 : (WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

static void log_sysfs_err(const char *what, const char *path) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d DIAG: sysfs %s %s failed errno=%d (%s)\n",
            tm.tm_hour, tm.tm_min, tm.tm_sec, what, path, errno, strerror(errno));
}

static int default_write_sysfs(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (!f) { log_sysfs_err("open", path); return -1; }
    int ok = (fputs(val, f) != EOF);
    if (!ok) log_sysfs_err("write", path);
    if (fclose(f) == EOF) { log_sysfs_err("flush", path); ok = 0; }
    return ok ? 0 : -1;
}

static const SystemOps g_default_ops = {
    .write_sysfs = default_write_sysfs,
    .runf        = default_runf,
};

static const SystemOps *g_ops = &g_default_ops;

const SystemOps *system_ops(void) { return g_ops; }

void system_ops_set(const SystemOps *ops) { g_ops = ops ? ops : &g_default_ops; }
