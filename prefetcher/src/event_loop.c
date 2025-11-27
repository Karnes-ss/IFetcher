// d:\OS_lab\IFecther\prefetcher\src\core\event_loop.c
#include "event_loop.h"
#include "config.h"
#include "inotify_wrapper.h"
#include "prefetch.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

static int verbose() { const char* v = getenv("IFETCHER_VERBOSE"); return (v == NULL || strcmp(v, "0") != 0); }

static FileNode* find_prefetch_list(PrefetcherConfig* config, int wd) {
    if (config == NULL || config->watch_map_head == NULL) return NULL;
    WatchMap* current = config->watch_map_head;
    while (current != NULL) {
        if (current->wd == wd) return current->prefetch_list;
        current = current->next;
    }
    return NULL;
}

static long get_env_ms(const char* name, long def_ms) {
    const char* v = getenv(name);
    if (!v || v[0] == '\0') return def_ms;
    char* endp = NULL;
    long x = strtol(v, &endp, 10);
    if (endp == v || x < 0) return def_ms;
    return x;
}

typedef struct {
    int wd;
    time_t last;
} CoolItem;

static CoolItem cool_items[128];
static size_t cool_count = 0;

static int cooldown_hit(int wd, long cooldown_ms) {
    time_t now = time(NULL);
    for (size_t i = 0; i < cool_count; i++) {
        if (cool_items[i].wd == wd) {
            long diff_ms = (long)(difftime(now, cool_items[i].last) * 1000.0);
            if (diff_ms < cooldown_ms) return 1;
            cool_items[i].last = now;
            return 0;
        }
    }
    if (cool_count < sizeof(cool_items)/sizeof(cool_items[0])) {
        cool_items[cool_count].wd = wd;
        cool_items[cool_count].last = now;
        cool_count++;
    }
    return 0;
}

int event_loop_run(PrefetcherConfig* config, pid_t app_pid) {
    char event_buf[EVENT_BUF_SIZE];
    if (verbose()) printf("[MAIN] Waiting for trigger file access...\n\n");
    long cooldown_ms = get_env_ms("PREFETCH_COOLDOWN_MS", 0);
    long poll_ms = get_env_ms("EVENT_LOOP_POLL_MS", 1000);
    long idle_exit_ms = get_env_ms("EVENT_LOOP_IDLE_EXIT_MS", 0);
    time_t last_activity = time(NULL);

    while (1) {
        int ready = 1;
        if (poll_ms > 0) {
            struct pollfd pfd;
            pfd.fd = config->inotify_fd;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, (int)poll_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                perror("[MAIN ERROR] poll inotify fd");
                break;
            }
            ready = (pr > 0 && (pfd.revents & POLLIN));
        }

        if (ready) {
            ssize_t num_read = inotify_read_events(config->inotify_fd, event_buf, sizeof(event_buf));
            if (num_read < 0) {
                if (errno == EINTR) continue;
                perror("[MAIN ERROR] read inotify fd");
                break;
            }
            if (num_read > 0) {
                struct inotify_event* event = NULL;
                for (char* ptr = event_buf; ptr < event_buf + num_read; ptr += sizeof(struct inotify_event) + event->len) {
                    event = (struct inotify_event*)ptr;
                    if ((event->mask & IN_ACCESS) || (event->mask & IN_OPEN)) {
                        last_activity = time(NULL);
                        if (verbose()) printf("[MAIN] Detected trigger file access (WD: %d)\n", event->wd);
                        if (cooldown_hit(event->wd, cooldown_ms)) {
                            if (verbose()) printf("[MAIN] Cooldown active, skip prefetch for WD: %d\n", event->wd);
                            continue;
                        }
                        FileNode* prefetch_list = find_prefetch_list(config, event->wd);
                        if (prefetch_list == NULL) {
                            fprintf(stderr, "[MAIN WARNING] No prefetch list found for WD: %d\n", event->wd);
                            continue;
                        }
                        if (prefetch_create_thread(prefetch_list) != 0) {
                            fprintf(stderr, "[MAIN ERROR] Failed to create prefetch thread for WD: %d\n", event->wd);
                        }
                    }
                }
            }
        }

        int app_status;
        if (app_pid > 0 && waitpid(app_pid, &app_status, WNOHANG) > 0) {
            if (WIFEXITED(app_status)) {
                if (verbose()) printf("\n[MAIN] App exited normally (exit code: %d)\n", WEXITSTATUS(app_status));
            } else {
                if (verbose()) printf("\n[MAIN] App exited abnormally\n");
            }
            break;
        }

        if (idle_exit_ms > 0) {
            long diff_ms = (long)(difftime(time(NULL), last_activity) * 1000.0);
            if (diff_ms >= idle_exit_ms) {
                if (verbose()) printf("[MAIN] Idle timeout reached (%ld ms), exiting\n", idle_exit_ms);
                break;
            }
        }
    }

    return 0;
}