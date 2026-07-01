#ifndef SYSTEM_OPS_H
#define SYSTEM_OPS_H

/* Injectable adapter over raw OS effects (sysfs writes, shell helpers) so
 * tests can stub them via system_ops_set() instead of touching hardware. */
typedef struct {
    int (*write_sysfs)(const char *path, const char *val); /* 0 ok, -1 on failure */
    int (*runf)(const char *cmd);                          /* exit code, -1 on error */
} SystemOps;

/* The active ops — never NULL. */
const SystemOps *system_ops(void);

/* Install custom ops (NULL restores the built-in default). For tests. */
void system_ops_set(const SystemOps *ops);

#endif /* SYSTEM_OPS_H */
