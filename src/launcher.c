/*
 * launcher.c — process supervisor. Forks at startup: the child runs the app
 * (app_main()); this process supervises it and restores the system stack even
 * if the child crashes. Replaces the old run.sh wrapper.
 *
 * The hardware watchdog (fed by `sickel`) reboots the device if unfed for ~60s,
 * so sickel is only killed after the child's first WiFi attempt resolves.
 * Freezes connmand before launch so it can't fight the app's wpa_supplicant,
 * then SIGSTOPs hindenburg/nickel once WiFi resolves; all resumed on exit.
 *
 * Nickel is SIGSTOPped (never killed) so its reader fd on the
 * /tmp/nickel-hardware-status FIFO stays open for kiosk udhcpc hooks.
 */

#include "launcher.h"
#include "sysutil.h"
#include "fb.h"
#include <fbink.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/prctl.h>

/* run.log / last_run.log live beside the binary; built at runtime in
 * setup_logging() from sysutil_exe_dir() so they follow the executable. */
#define LOG_PATH_LEAF      "run.log"
#define LAST_LOG_PATH_LEAF "last_run.log"
#define LOCK_PATH        "/tmp/kobo_weather.lock"

/* Must match powersave.c's KIOSK_FLAG / WAKELOCK_NAME exactly: powersave.c
 * checks this flag to exempt long kiosk sleeps from the fail-safe, and
 * is_own_lock() special-cases this wake-lock name during suspend. */
#define KIOSK_FLAG       "/tmp/kobo_weather_kiosk"
#define WAKELOCK_NAME    "kobo_weather"

#define FAILSAFE_S              60  /* kill a wedged event loop ~1 min after it stops heartbeating */
#define WIFI_CONNECT_TIMEOUT_S  90  /* mirrors run.sh's WIFI_CONNECT_TIMEOUT */

/* Captured just before `killall -9 sickel` (Phase A stack-suspend block, see
 * main()) so sys_restore() has an exact-relaunch fallback if both
 * `/etc/init.d/sickel start` and `sickel &` fail to bring the watchdog
 * feeder back up. */
static char g_sickel_exe[PATH_MAX];
static char g_sickel_cwd[PATH_MAX];

/* Well-known location of Kobo's watchdog supervisor; used as a last-resort
 * relaunch target if capture came up empty (e.g. sickel was already gone). */
#define SICKEL_KNOWN_PATH "/usr/local/Kobo/sickel"

#define slog(...) sysutil_log("", true, __VA_ARGS__)

/* Find a running process by exact name via a self-contained /proc scan — no
 * `pgrep`/popen, so it doesn't depend on busybox `pgrep -x` behaving (the
 * old capture/verify hinged on `pgrep -x sickel`, which evidently returned
 * nothing on-device, leaving g_sickel_exe empty and the restart check blind).
 * Matches against /proc/PID/comm. On a match, optionally fills `exe`/`cwd`
 * (each PATH_MAX, may be NULL) by readlink-ing /proc/PID/{exe,cwd}. Returns
 * the pid, or 0 if not found. */
static int find_proc(const char *name, char *exe, char *cwd) {
    if (exe) exe[0] = '\0';
    if (cwd) cwd[0] = '\0';

    DIR *d = opendir("/proc");
    if (!d) return 0;

    int found = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        int pid = atoi(de->d_name);
        if (pid <= 0) continue;

        char path[64], comm[64];
        snprintf(path, sizeof path, "/proc/%d/comm", pid);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n <= 0) continue;
        if (comm[n - 1] == '\n') n--;     /* comm is newline-terminated */
        comm[n] = '\0';
        if (strcmp(comm, name) != 0) continue;

        found = pid;
        if (exe) {
            snprintf(path, sizeof path, "/proc/%d/exe", pid);
            ssize_t m = readlink(path, exe, PATH_MAX - 1);
            exe[m > 0 ? m : 0] = '\0';
        }
        if (cwd) {
            snprintf(path, sizeof path, "/proc/%d/cwd", pid);
            ssize_t m = readlink(path, cwd, PATH_MAX - 1);
            cwd[m > 0 ? m : 0] = '\0';
        }
        break;
    }
    closedir(d);
    return found;
}

static void capture_sickel_info(void) {
    int pid = find_proc("sickel", g_sickel_exe, g_sickel_cwd);
    if (pid > 0)
        slog("captured sickel pid=%d exe=%s cwd=%s", pid,
             g_sickel_exe[0] ? g_sickel_exe : "(unreadable)",
             g_sickel_cwd[0] ? g_sickel_cwd : "(unreadable)");
    else
        slog("capture_sickel_info: sickel not found in /proc");
}

static int  runf(const char *cmd) { return sysutil_runf(cmd); }
static void write_sysfs(const char *p, const char *v) { sysutil_write_sysfs(p, v); }

/* ---- startup helpers --------------------------------------------------*/

/* Rotates run.log -> last_run.log and redirects stdin/stdout/stderr so both
 * the supervisor and the forked app log to the same file (cmd_spawn gives
 * us no useful stdio). Run before fork — both processes inherit these fds. */
static void setup_logging(void) {
    char log_path[PATH_MAX], last_path[PATH_MAX];
    sysutil_path(log_path,  sizeof log_path,  LOG_PATH_LEAF);
    sysutil_path(last_path, sizeof last_path, LAST_LOG_PATH_LEAF);
    rename(log_path, last_path); /* rotate-on-start; ignore ENOENT */

    int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        if (devnull > STDIN_FILENO) close(devnull);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* cmd_spawn's cwd is not the app dir, but fonts load via ./fonts/ — chdir to
 * the executable's directory so they resolve beside the binary. */
static void chdir_to_exe_dir(void) {
    chdir(sysutil_exe_dir());
}

/* Exit silently (before touching any system state) if another instance
 * already holds the lock. The fd is left open for the life of the process
 * and released automatically on exit or crash. */
static void single_instance_lock(void) {
    int fd = open(LOCK_PATH, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) return; /* can't lock -- proceed anyway */
    if (flock(fd, LOCK_EX | LOCK_NB) < 0)
        _exit(0);
}

/* "up" if any wlan* or mlan* interface already has an IPv4 address — works
 * regardless of who connected it (Nickel/connman before launch). */
static int wifi_was_up_at_launch(void) {
    struct ifaddrs *ifap, *ifa;
    int up = 0;
    if (getifaddrs(&ifap) != 0) return 0;
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strncmp(ifa->ifa_name, "wlan", 4) != 0 &&
            strncmp(ifa->ifa_name, "mlan", 4) != 0)
            continue;
        up = 1;
        break;
    }
    freeifaddrs(ifap);
    return up;
}

/* ---- pre-fork system prep / post-exit restore ------------------------ */

/* Freeze connmand (so it can't race the app's own wpa_supplicant for the
 * interface) and take the wake lock before the child runs. Also clears a
 * stale kiosk flag from a previous crash, which would otherwise disable the
 * fail-safe timer for this run too. */
static void sys_prep(void) {
    slog("Freezing connmand/hindenburg/nickel (app owns WiFi+EPDC from here)...");
    runf("pkill -STOP connmand 2>/dev/null");
    runf("killall -STOP hindenburg 2>/dev/null");
    runf("pkill -STOP nickel 2>/dev/null");
    write_sysfs("/sys/power/wake_lock", WAKELOCK_NAME);
    unlink(KIOSK_FLAG);
}

/* Wash the panel white with a full GC16 refresh (no flash). Only used when
 * the child died uncleanly (crash/SIGKILL) and never ran ui_cleanup()'s own
 * screen clear. Reuses fb.c's existing paced-refresh machinery instead of
 * calling raw FBInk so the framebuffer geometry (8/16/32 bpp) is handled the
 * same way as the rest of the app. */
static void fb_rescue_flash(void) {
    FB fb = {0};
    if (fb_open(&fb) != 0) return;

    int fbfd = fbink_open();
    if (fbfd < 0) { fb_close(&fb); return; }

    FBInkConfig cfg = {0};
    if (fbink_init(fbfd, &cfg) < 0) { fbink_close(fbfd); fb_close(&fb); return; }

    fb_fill_rect(&fb, (Rect){0, 0, fb.w, fb.h}, 0xFF); /* white */
    fb_paced_init(fbfd);
    /* Pre-UI rescue path: no UIState exists yet, so this wash is not
     * gc16_flash-setting-aware. */
    fb_paced_refresh(RECT_FULLSCREEN, WFM_GC16, false);
    fb_paced_wait();

    fbink_close(fbfd);
    fb_close(&fb);
}

/* Restores the whole system stack. Always runs (clean exit, crash, or
 * orphan fallback) and is idempotent. `unclean` selects the framebuffer
 * rescue; `wifi_was_up` controls whether WiFi is turned back off. Mirrors
 * run.sh's cleanup() trap line-for-line. */
static void sys_restore(int wifi_was_up, int unclean) {
    slog("Restoring system stack (wifi_was_up=%d unclean=%d)...",
         wifi_was_up, unclean);

    /* Re-take the wake lock first: a crash inside a kiosk suspend cycle may
     * have released every lock just before dying, and autosleep must not
     * suspend the device again mid-restore. */
    write_sysfs("/sys/power/wake_lock", WAKELOCK_NAME);

    if (unclean) fb_rescue_flash();

    write_sysfs("/sys/power/wake_unlock", WAKELOCK_NAME);

    runf("pkill -CONT connmand 2>/dev/null");
    runf("killall -CONT hindenburg 2>/dev/null");

    if (find_proc("sickel", NULL, NULL) > 0) {
        runf("killall -CONT sickel 2>/dev/null");
        slog("sickel: already running (resumed).");
    } else {
        /* The init.d/PATH restart (start script, or bare `sickel &`) never
         * succeeds on this device — only relaunching the exact binary
         * captured before the kill does — so go straight there instead of
         * paying a guaranteed-to-fail attempt + 1s wait first. Falls back to
         * the well-known Kobo path if capture came up empty. */
        const char *exe = g_sickel_exe[0] ? g_sickel_exe
                        : (access(SICKEL_KNOWN_PATH, X_OK) == 0 ? SICKEL_KNOWN_PATH : NULL);
        if (exe) {
            pid_t cpid = fork();
            if (cpid == 0) {
                setsid();
                if (g_sickel_cwd[0]) chdir(g_sickel_cwd);
                int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
                }
                char *argv[2] = { (char *)exe, NULL };
                execv(exe, argv);
                _exit(127);
            }
            if (cpid > 0) sleep(1);
            if (find_proc("sickel", NULL, NULL) > 0)
                slog("sickel: restarted OK via %s (watchdog re-armed).", exe);
            else
                slog("sickel: FAILED to restart (watchdog stays disabled until reboot).");
        } else {
            slog("sickel: FAILED to restart (no exe path; watchdog stays disabled until reboot).");
        }
    }

    if (runf("pkill -CONT nickel 2>/dev/null") != 0) {
        slog("Warning: Nickel not running, attempting restart...");
        if (access("/etc/init.d/nickel", X_OK) == 0)
            runf("/etc/init.d/nickel start 2>/dev/null");
        else
            runf("nickel 2>/dev/null &");
    }

    if (!wifi_was_up) {
        slog("Restoring WiFi to original 'down' state.");
        runf("command -v connmanctl >/dev/null 2>&1 && connmanctl disable wifi 2>/dev/null");
    }

    unlink(KIOSK_FLAG);
    sleep(2); /* give Nickel time to wake up and refresh its screen */
    slog("Done.");
}

void sys_restore_orphaned(void) {
    if (getppid() == 1) {
        slog("Supervisor is gone (orphaned) -- restoring stack from app process.");
        sys_restore(/*wifi_was_up=*/1, /*unclean=*/0);
    }
}

/* ---- child -> supervisor notification --------------------------------*/

static int g_wifi_pipe_wfd = -1;
static int g_wifi_notified = 0;

void launcher_notify_wifi_resolved(void) {
    if (g_wifi_notified || g_wifi_pipe_wfd < 0) return;
    g_wifi_notified = 1;
    (void)!write(g_wifi_pipe_wfd, "x", 1);
}

static int g_liveness_pipe_wfd = -1;

void launcher_notify_liveness(void) {
    if (g_liveness_pipe_wfd < 0) return;
    (void)!write(g_liveness_pipe_wfd, "L", 1);  /* non-blocking; full pipe is fine */
}

/* ---- supervisor helpers ------------------------------------------------*/

static void arm_timer(int fd, int seconds) {
    struct itimerspec its = {0};
    its.it_value.tv_sec = seconds;
    timerfd_settime(fd, 0, &its, NULL);
}

/* SIGTERM the child, give it 5 s to exit gracefully, then SIGKILL. Returns 1
 * if the kill had to be escalated (caller uses this for the fb rescue
 * decision), 0 if the child exited on SIGTERM. */
static int terminate_child(pid_t pid, int *status) {
    kill(pid, SIGTERM);
    for (int i = 0; i < 5; i++) {
        if (waitpid(pid, status, WNOHANG) == pid) return 0;
        sleep(1);
    }
    slog("Child did not exit on SIGTERM, sending SIGKILL.");
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return 1;
}

/* ---- main -------------------------------------------------------------*/

int main(int argc, char *argv[]) {
    setup_logging();
    chdir_to_exe_dir();
    single_instance_lock();

    slog("=== kobo_weather launcher starting ===");

    if (setsid() < 0)
        slog("setsid: %s (continuing)", strerror(errno));

    int wifi_was_up = wifi_was_up_at_launch();
    slog("WiFi state at launch: %s", wifi_was_up ? "up" : "down");

    int wifi_pipe[2] = {-1, -1};
    if (pipe2(wifi_pipe, O_CLOEXEC) != 0) {
        slog("pipe2: %s", strerror(errno));
        wifi_pipe[0] = wifi_pipe[1] = -1;
    }

    /* Liveness pipe: the app heartbeats into the write end; the supervisor reads
     * it in Phase B to re-arm the fail-safe. Non-blocking so a full pipe (many
     * pings between reads) never stalls the app. */
    int live_pipe[2] = {-1, -1};
    if (pipe2(live_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        slog("pipe2 (liveness): %s", strerror(errno));
        live_pipe[0] = live_pipe[1] = -1;
    }

    /* Block before fork so no SIGCHLD can be lost between fork() and the
     * signalfd() call below. */
    sigset_t mask, empty_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigemptyset(&empty_mask);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sigfd < 0) slog("signalfd: %s", strerror(errno));

    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timerfd < 0) slog("timerfd_create: %s", strerror(errno));
    else arm_timer(timerfd, FAILSAFE_S);

    sys_prep();

    pid_t pid = fork();
    if (pid < 0) {
        slog("fork: %s", strerror(errno));
        sys_restore(wifi_was_up, /*unclean=*/1);
        return 1;
    }

    if (pid == 0) {
        /* ---- child: becomes the app ---- */
        sigprocmask(SIG_SETMASK, &empty_mask, NULL); /* let app_main's signal() handlers fire */
        if (sigfd   >= 0) close(sigfd);
        if (timerfd >= 0) close(timerfd);
        if (wifi_pipe[0] >= 0) close(wifi_pipe[0]);
        g_wifi_pipe_wfd = wifi_pipe[1];
        if (live_pipe[0] >= 0) close(live_pipe[0]);
        g_liveness_pipe_wfd = live_pipe[1];

        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1)
            slog("Supervisor already gone at fork (race) -- "
                 "orphan fallback will restore the stack on exit.");

        exit(app_main(argc, argv));
    }

    /* ---- supervisor ---- */
    if (wifi_pipe[1] >= 0) close(wifi_pipe[1]);
    if (live_pipe[1] >= 0) close(live_pipe[1]);

    /* Phase A (mirrors run.sh:205-231): wait for the child's first WiFi
     * attempt to resolve, the child to exit, a termination signal, or a
     * timeout -- then, if the child is still alive, suspend the rest of
     * the stack so it can't fight the app's WiFi/EPDC use. */
    slog("Waiting up to %ds for WiFi to come up...", WIFI_CONNECT_TIMEOUT_S);

    int status = 0, child_exited = 0, escalated = 0, pending_sig = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += WIFI_CONNECT_TIMEOUT_S;

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int remain_ms = (int)((deadline.tv_sec  - now.tv_sec)  * 1000 +
                               (deadline.tv_nsec - now.tv_nsec) / 1000000);
        if (remain_ms < 0) remain_ms = 0;

        struct pollfd pfds[2] = {
            { .fd = wifi_pipe[0], .events = POLLIN },
            { .fd = sigfd,        .events = POLLIN },
        };
        int rc = poll(pfds, 2, remain_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) {
            slog("WiFi connect timeout after %ds -- suspending anyway.",
                 WIFI_CONNECT_TIMEOUT_S);
            break;
        }
        if (pfds[0].revents & (POLLIN | POLLHUP)) {
            slog("WiFi resolved by app -- killing sickel.");
            break;
        }
        if (pfds[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            if (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si) {
                if (si.ssi_signo == SIGCHLD) {
                    /* SIGCHLD also fires for stop/continue and for system()/
                     * reaped helper children; confirm a real termination
                     * before acting, otherwise a spurious early SIGCHLD would
                     * skip the stack-suspend below and leave Nickel running
                     * (its clock bleeds through). */
                    if (waitpid(pid, &status, WNOHANG) == pid &&
                        (WIFEXITED(status) || WIFSIGNALED(status))) {
                        child_exited = 1;
                        slog("App exited before WiFi up.");
                        break;
                    }
                    slog("Ignoring spurious SIGCHLD (app pid %d still running).",
                         (int)pid);
                    continue;   /* signalfd already dequeued it; keep waiting */
                }
                pending_sig = (int)si.ssi_signo;
                break;
            }
        }
    }
    if (wifi_pipe[0] >= 0) { close(wifi_pipe[0]); wifi_pipe[0] = -1; }

    if (!child_exited && !pending_sig) {
        capture_sickel_info();
        runf("killall -9 sickel 2>/dev/null");
        slog("sickel killed (watchdog disarmed).");
    }

    /* Phase B: supervise until the child exits, forwarding termination signals
     * (with TERM-then-KILL grace). The fail-safe is re-armed by the app's
     * liveness heartbeat (responsive loop) or by the kiosk flag (power-save);
     * only a wedged, non-kiosk app stops both and gets killed at the timeout. */
    if (child_exited) {
        /* Already reaped via waitpid(WNOHANG) in Phase A above, which set
         * `status`; waiting again on the same pid would target an already-
         * reaped (or recycled) process, so there is nothing more to do here. */
    } else if (pending_sig) {
        slog("Forwarding signal %d to app (pid %d)...", pending_sig, (int)pid);
        escalated = terminate_child(pid, &status);
    } else {
        /* Re-arm now that we actually poll the timer: Phase A doesn't, so the
         * short fail-safe could already be near-expired when Phase B begins. */
        if (timerfd >= 0) arm_timer(timerfd, FAILSAFE_S);
        for (;;) {
            struct pollfd pfds[3] = {
                { .fd = sigfd,        .events = POLLIN },
                { .fd = timerfd,      .events = POLLIN },
                { .fd = live_pipe[0], .events = POLLIN },
            };
            int rc = poll(pfds, 3, -1);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pfds[0].revents & POLLIN) {
                struct signalfd_siginfo si;
                if (read(sigfd, &si, sizeof si) != (ssize_t)sizeof si)
                    continue;
                if (si.ssi_signo == SIGCHLD) {
                    waitpid(pid, &status, 0);
                    child_exited = 1;
                    slog("App exited.");
                } else {
                    slog("Forwarding signal %d to app (pid %d)...",
                         (int)si.ssi_signo, (int)pid);
                    escalated = terminate_child(pid, &status);
                    child_exited = 1;
                }
                break;
            }
            if (pfds[1].revents & POLLIN) {
                uint64_t exp;
                (void)!read(timerfd, &exp, sizeof exp);
                if (access(KIOSK_FLAG, F_OK) == 0) {
                    slog("App is in kiosk/power-save mode -- re-arming fail-safe timer.");
                    arm_timer(timerfd, FAILSAFE_S);
                    continue;
                }
                slog("FAIL-SAFE: timeout reached (app loop wedged) -- killing app.");
                escalated = terminate_child(pid, &status);
                child_exited = 1;
                break;
            }
            if (pfds[2].revents & POLLIN) {
                /* Heartbeat: drain the pipe and pet the fail-safe. */
                char drain[64];
                while (read(live_pipe[0], drain, sizeof drain) > 0) { }
                arm_timer(timerfd, FAILSAFE_S);
                continue;
            }
            if (pfds[2].revents & POLLHUP) {
                /* App closed its end (exited); SIGCHLD handles the rest. */
                close(live_pipe[0]); live_pipe[0] = -1;
            }
        }
    }

    int unclean = escalated || WIFSIGNALED(status);
    sys_restore(wifi_was_up, unclean);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
