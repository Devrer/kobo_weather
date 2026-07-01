#ifndef SYSUTIL_H
#define SYSUTIL_H
#include <stdbool.h>
#include <stddef.h>

/* Run a pre-built shell command; return exit code (or -1 on fork/exec error). */
int  sysutil_runf(const char *cmd);

/* Write a string to a sysfs/proc path.
 * Returns 0 on success, -1 on failure; logs to stderr on error. */
int  sysutil_write_sysfs(const char *path, const char *val);

/* Timestamped log line to stderr. If !always, skips when g_debug is false.
 * prefix is prepended after "DIAG: " (e.g. "WIFI ", "PS ", or ""). */
void sysutil_log(const char *prefix, bool always, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Absolute directory of the running executable (resolved once from
 * /proc/self/exe; cwd-independent). "." if unresolvable. Lets data files land
 * beside the binary. */
const char *sysutil_exe_dir(void);

/* Build "<exe_dir>/leaf" into out (size n); returns out. */
const char *sysutil_path(char *out, size_t n, const char *leaf);

#endif
