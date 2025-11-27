#include "maps_monitor.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static MmapEntry last_mmap_entries[MAPS_MAX_ENTRY];
static int last_mmap_count = 0;

typedef struct {
    char filename[256];
    off_t start;
    off_t end;
    size_t file_size;
} StartupMmapEntry;

static StartupMmapEntry startup_mmap_entries[MAPS_MAX_ENTRY];
static int startup_mmap_count = 0;
static time_t monitor_start_time = 0;

static int parse_proc_maps(pid_t pid, MmapEntry* entries, int max_entries) {
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);

    FILE* fp = fopen(maps_path, "r");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open %s: %s\n", maps_path, strerror(errno));
        return -1;
    }

    char line[1024];
    int count = 0;
    while (fgets(line, sizeof(line), fp) != NULL && count < max_entries) {
        unsigned long long start_addr, end_addr;
        unsigned long long offset;
        char pathname[256] = "unknown";

        int parsed = sscanf(line, "%llx-%llx %*s %llx %*s %*d %255s",
                            &start_addr, &end_addr, &offset, pathname);
        if (parsed >= 3) {
            if (pathname[0] == '/' &&
                strncmp(pathname, "/memfd:", 7) != 0 &&
                strncmp(pathname, "/SYSV", 5) != 0) {
                entries[count].start = (off_t)start_addr;
                entries[count].end   = (off_t)end_addr;
                entries[count].file_offset = offset;
                strncpy(entries[count].filename, pathname, sizeof(entries[count].filename) - 1);
                entries[count].filename[sizeof(entries[count].filename) - 1] = '\0';
                count++;
            }
        }
    }

    fclose(fp);
    return count;
}

void maps_set_start_time(time_t t) {
    monitor_start_time = t;
}

int maps_init_snapshot(pid_t pid, MmapEntry* out_entries, int max_out) {
    int count = parse_proc_maps(pid, out_entries, max_out);
    if (count < 0) {
        last_mmap_count = 0;
        return -1;
    }
    last_mmap_count = count;
    memcpy(last_mmap_entries, out_entries, count * sizeof(MmapEntry));
    return count;
}

void check_mmap_changes(pid_t pid) {
    MmapEntry current_entries[MAPS_MAX_ENTRY];
    int current_count = parse_proc_maps(pid, current_entries, MAPS_MAX_ENTRY);
    if (current_count == -1) return;

    for (int i = 0; i < current_count; i++) {
        int is_new = 1;
        for (int j = 0; j < last_mmap_count; j++) {
            if (strcmp(current_entries[i].filename, last_mmap_entries[j].filename) == 0 &&
                current_entries[i].start == last_mmap_entries[j].start &&
                current_entries[i].end == last_mmap_entries[j].end) {
                is_new = 0;
                break;
            }
        }

        if (is_new) {
            size_t file_size = 0;
            int fd = open(current_entries[i].filename, O_RDONLY);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0) {
                    file_size = (size_t)st.st_size;
                }
                close(fd);
            }

            ProfilerLogEntry entry = {
                .pid = pid,
                .op_type = OP_MMAP,
                .filename = current_entries[i].filename,
                .offset = 0,
                .size = file_size,
                .fd = -1,
                .timestamp = time(NULL),
                .addr_start = current_entries[i].start,
                .addr_end = current_entries[i].end,
                .file_offset = (off_t)current_entries[i].file_offset
            };
            profiler_log(&entry);

            if (monitor_start_time > 0 &&
                difftime(time(NULL), monitor_start_time) <= STARTUP_WINDOW_SEC) {
                if (startup_mmap_count < MAPS_MAX_ENTRY) {
                    strncpy(startup_mmap_entries[startup_mmap_count].filename,
                            current_entries[i].filename,
                            sizeof(startup_mmap_entries[startup_mmap_count].filename) - 1);
                    startup_mmap_entries[startup_mmap_count].filename[
                        sizeof(startup_mmap_entries[startup_mmap_count].filename) - 1] = '\0';
                    startup_mmap_entries[startup_mmap_count].start = current_entries[i].start;
                    startup_mmap_entries[startup_mmap_count].end   = current_entries[i].end;
                    startup_mmap_entries[startup_mmap_count].file_size = file_size;
                    startup_mmap_count++;
                }
            }
        }
    }

    last_mmap_count = current_count;
    memcpy(last_mmap_entries, current_entries, current_count * sizeof(MmapEntry));
}

void flush_startup_mmaps(pid_t pid) {
    for (int i = 0; i < startup_mmap_count; i++) {
        ProfilerLogEntry entry = {
            .pid = pid,
            .op_type = OP_MMAP,
            .filename = startup_mmap_entries[i].filename,
            .offset = 0,
            .size = startup_mmap_entries[i].file_size,
            .fd = -1,
            .timestamp = time(NULL),
            .addr_start = startup_mmap_entries[i].start,
            .addr_end = startup_mmap_entries[i].end,
            .file_offset = 0
        };
        profiler_log(&entry);
    }
}