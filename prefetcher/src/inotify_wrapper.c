#include "inotify_wrapper.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <errno.h>

int inotify_init_wrapper(void) {
    int fd = inotify_init();
    if (fd == -1) {
        perror("[INOTIFY ERROR] inotify_init");
    } else {
        printf("[INOTIFY] Initialized (fd=%d)\n", fd);
    }
    return fd;
}

int inotify_add_watch_wrapper(int inotify_fd, const char* path) {
    if (inotify_fd < 0 || path == NULL) {
        fprintf(stderr, "[INOTIFY ERROR] Invalid parameter for add_watch\n");
        return -1;
    }

    int wd = inotify_add_watch(inotify_fd, path, IN_ACCESS | IN_OPEN);
    if (wd == -1) {
        perror("[INOTIFY ERROR] inotify_add_watch");
        fprintf(stderr, "[INOTIFY ERROR] Path: %s\n", path);
    }
    return wd;
}

int inotify_rm_watch_wrapper(int inotify_fd, int wd) {
    if (inotify_fd < 0 || wd < 0) {
        fprintf(stderr, "[INOTIFY ERROR] Invalid parameter for rm_watch\n");
        return -1;
    }

    if (inotify_rm_watch(inotify_fd, wd) == -1) {
        perror("[INOTIFY ERROR] inotify_rm_watch");
        fprintf(stderr, "[INOTIFY ERROR] WD: %d\n", wd);
        return -1;
    }
    return 0;
}

ssize_t inotify_read_events(int inotify_fd, char* buf, size_t buf_size) {
    if (inotify_fd < 0 || buf == NULL || buf_size == 0) {
        fprintf(stderr, "[INOTIFY ERROR] Invalid parameter for read_events\n");
        return -1;
    }

    for (;;) {
        ssize_t num_read = read(inotify_fd, buf, buf_size);
        if (num_read == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("[INOTIFY ERROR] read events");
        }
        return num_read;
    }
}