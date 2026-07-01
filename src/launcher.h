#ifndef LAUNCHER_H
#define LAUNCHER_H

/* Renamed main() from main.c; invoked by the launcher's forked child. */
int app_main(int argc, char *argv[]);

/* Tells the supervisor it's safe to suspend nickel/hindenburg and kill
 * sickel, once the first wifi_connect_staged() has resolved. One-shot. */
void launcher_notify_wifi_resolved(void);

/* Heartbeat from the app's main loop; re-arms the supervisor's fail-safe so a
 * responsive interactive session is never killed. */
void launcher_notify_liveness(void);

/* If the forking supervisor has already died (reparented to init), restores
 * the system stack ourselves; no-op if the supervisor is still alive. */
void sys_restore_orphaned(void);

#endif /* LAUNCHER_H */
