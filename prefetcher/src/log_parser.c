#include "log_parser.h"
#include "list.h"
#include "inotify_wrapper.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdlib.h>

#define MAX_PATH_LEN 256
#define MAX_LINE_LEN 512

static int verbose() { const char* v = getenv("IFETCHER_VERBOSE"); return (v == NULL || strcmp(v, "0") != 0); }

static long get_env_long(const char* name, long def) {
    const char* s = getenv(name);
    if (!s || s[0] == '\0') return def;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    return (end == s) ? def : v;
}


static int should_skip_path(const char* path) {
    const char* env = getenv("PREFETCH_SKIP_PREFIXES");
    if (!env || env[0] == '\0') return 0;
    char buf[512];
    strncpy(buf, env, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* tok = strtok(buf, ",");
    while (tok) {
        size_t n = strlen(tok);
        if (n > 0 && strncmp(path, tok, n) == 0) return 1;
        tok = strtok(NULL, ",");
    }
    return 0;
}

static int parse_log_line_parts(char* line, char** out_path, off_t* out_off, size_t* out_len) {
    if (!line) return 0;
    if (line[0] == '\0') return 0;
    char* p = line;
    char* nl = strchr(p, '\n'); if (nl) *nl = '\0';
    char* rr = strchr(p, '\r'); if (rr) *rr = '\0';
    char* c1 = strchr(p, ',');
    off_t off = 0; size_t len = 0;
    if (c1) {
        *c1 = '\0';
        char* rest = c1 + 1;
        char* c2 = strchr(rest, ',');
        if (c2) {
            *c2 = '\0';
            off = (off_t)strtoll(rest, NULL, 10);
            len = (size_t)strtoull(c2 + 1, NULL, 10);
        }
    }
    *out_path = p;
    *out_off = off;
    *out_len = len;
    return 1;
}

static FileNode* build_segment_for_trigger(const char* prefetch_path,
                                           const char* trigger_path,
                                           long topn) {
    FILE* fp = fopen(prefetch_path, "r");
    if (!fp) return NULL;
    char line[MAX_LINE_LEN];
    int in_seg = 0, match = 0;
    size_t copied = 0;
    FileNode* lo = NULL;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "===TRIGGER===", 13) == 0) {
            in_seg = 1;
            match = 0;
            if (!fgets(line, sizeof(line), fp)) break;
            char* seg_path = NULL; off_t seg_off = 0; size_t seg_len = 0;
            if (!parse_log_line_parts(line, &seg_path, &seg_off, &seg_len)) continue;
            match = (strcmp(seg_path, trigger_path) == 0);
            continue;
        }
        if (!in_seg || !match) continue;
        char* p = NULL; off_t off = 0; size_t len = 0;
        if (!parse_log_line_parts(line, &p, &off, &len)) continue;
        if (p[0] == '\0') continue;
        if (strncmp(p, "/proc/", 6) == 0 || strncmp(p, "/sys/", 5) == 0 || strncmp(p, "/dev/", 5) == 0) continue;
        if (should_skip_path(p)) continue;
        if (topn > 0 && copied >= (size_t)topn) break;
        if (list_add_node_ex(&lo, p, off, len) == 0) copied++;
    }
    fclose(fp);
    return lo;
}

int log_parser_load(PrefetcherConfig* config) {
    if (!config || !config->trigger_log_path || !config->prefetch_log_path) {
        return -1;
    }
    FILE* tf = fopen(config->trigger_log_path, "r");
    if (!tf) {
        return -1;
    }
    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), tf)) {
        char* path = NULL; off_t off = 0; size_t len = 0;
        if (!parse_log_line_parts(line, &path, &off, &len)) continue;
        if (strncmp(path, "APP=", 4) == 0) continue;
        if (path[0] == '\0') continue;
        if (strncmp(path, "/proc/", 6) == 0 || strncmp(path, "/sys/", 5) == 0 || strncmp(path, "/dev/", 5) == 0) continue;
        char canon[MAX_PATH_LEN];
        {
            char* rp = realpath(path, canon);
            if (!rp) {
                strncpy(canon, path, sizeof(canon)-1);
                canon[sizeof(canon)-1] = '\0';
            }
        }
        int wd = inotify_add_watch_wrapper(config->inotify_fd, canon);
        if (wd != -1 && verbose()) {
            printf("[LOG PARSER] Added watch: %s (wd=%d)\n", canon, wd);
        }
        if (wd == -1) {
            char dir[MAX_PATH_LEN];
            char* p = strstr(path, "/cache2/entries/");
            if (p) {
                size_t n = (size_t)(p - path) + strlen("/cache2/entries");
                if (n >= sizeof(dir)) n = sizeof(dir) - 1;
                memcpy(dir, path, n);
                dir[n] = '\0';
                wd = inotify_add_watch_wrapper(config->inotify_fd, dir);
            }
            if (wd == -1) {
                perror("[LOG PARSER ERROR] inotify_add_watch");
                fprintf(stderr, "[LOG PARSER ERROR] Skip trigger file: %s\n", path);
                continue;
            }
        }
        long topn = get_env_long("PREFETCH_TOP_N", 0);
        FileNode* seg = build_segment_for_trigger(config->prefetch_log_path, canon, topn);
        if (seg && verbose()) {
            printf("[LOG PARSER] Prefetch list built for trigger (wd=%d): %zu files\n", wd, list_get_length(seg));
        }
        if (!seg) {
            fprintf(stderr, "[LOG PARSER WARNING] No segmented prefetch found for trigger: %s\n", path);
            inotify_rm_watch_wrapper(config->inotify_fd, wd);
            continue;
        }
        WatchMap* m = (WatchMap*)malloc(sizeof(WatchMap));
        if (!m) {
            perror("[LOG PARSER ERROR] malloc WatchMap");
            list_free(seg);
            inotify_rm_watch_wrapper(config->inotify_fd, wd);
            continue;
        }
        m->wd = wd;
        m->prefetch_list = seg;
        m->next = config->watch_map_head;
        config->watch_map_head = m;
    }
    fclose(tf);
    if (!config->watch_map_head) {
        return -1;
    }
    return 0;
}

void log_parser_free_map(PrefetcherConfig* config) {
    if (!config) return;
    while (config->watch_map_head) {
        WatchMap* t = config->watch_map_head;
        config->watch_map_head = config->watch_map_head->next;
        inotify_rm_watch_wrapper(config->inotify_fd, t->wd);
        list_free(t->prefetch_list);
        free(t);
    }
}