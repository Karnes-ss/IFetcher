#include "prefetch.h"
#include "list.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int verbose() { const char* v = getenv("IFETCHER_VERBOSE"); return (v == NULL || strcmp(v, "0") != 0); }
static long get_env_long(const char* name, long def) { const char* s = getenv(name); if (!s || s[0]=='\0') return def; char* end=NULL; long v=strtol(s,&end,10); return (end==s)?def:v; }

typedef struct PrefetchCtx {
    FileNode* head;
    pthread_mutex_t lock;
    unsigned int sleep_us;
    off_t max_bytes;
    size_t touch_kb;
    size_t files;
    size_t sum_len;
    size_t sum_touch;
    time_t t0;
} PrefetchCtx;

static void* prefetch_worker(void* arg) {
    PrefetchCtx* ctx = (PrefetchCtx*)arg;
    for (;;) {
        pthread_mutex_lock(&ctx->lock);
        FileNode* node = ctx->head;
        if (node) ctx->head = node->next;
        pthread_mutex_unlock(&ctx->lock);
        if (!node) break;
        int fd = open(node->path, O_RDONLY);
        if (fd == -1) {
            perror("[PREFETCH ERROR] open file");
            fprintf(stderr, "[PREFETCH ERROR] Skip file: %s\n", node->path);
            if (ctx->sleep_us) usleep(ctx->sleep_us);
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) == 0 && ctx->max_bytes > 0 && st.st_size > ctx->max_bytes) {
            close(fd);
            if (ctx->sleep_us) usleep(ctx->sleep_us);
            continue;
        }
        off_t off = node->offset;
        size_t len = node->length;
        if (len == 0 || (off + (off_t)len) > (off_t)st.st_size) {
            if ((off_t)st.st_size > 0 && off < (off_t)st.st_size) {
                len = (size_t)((off_t)st.st_size - off);
            } else {
                off = 0;
                len = (size_t)st.st_size;
            }
        }
        int err = posix_fadvise(fd, off, (off_t)len, POSIX_FADV_WILLNEED);
        if (err != 0) {
            fprintf(stderr, "[PREFETCH ERROR] posix_fadvise failed for %s (err=%d)\n", node->path, err);
        }
        size_t to_read = (size_t)(ctx->touch_kb * 1024);
        if (to_read > len) to_read = len;
        if (to_read > 0) {
            char* buf = (char*)malloc(to_read);
            if (buf) {
                if (off > 0) lseek(fd, off, SEEK_SET);
                ssize_t r = read(fd, buf, to_read);
                (void)r;
                free(buf);
            }
        }
        if (verbose()) printf("[PREFETCH THREAD] Success: %s\n", node->path);
        pthread_mutex_lock(&ctx->lock);
        ctx->files++;
        ctx->sum_len += (size_t)len;
        ctx->sum_touch += to_read;
        pthread_mutex_unlock(&ctx->lock);
        close(fd);
        if (ctx->sleep_us) usleep(ctx->sleep_us);
    }
    return NULL;
}

void* prefetch_thread(void* arg) {
    FileNode* prefetch_list = (FileNode*)arg;
    if (prefetch_list == NULL) {
        fprintf(stderr, "[PREFETCH ERROR] Thread got empty prefetch list\n");
        pthread_exit(NULL);
    }
    unsigned int sleep_us = (unsigned int)get_env_long("PREFETCH_SLEEP_US", 0);
    off_t max_bytes = (off_t)(get_env_long("PREFETCH_MAX_SIZE_KB", 0) * 1024L);
    size_t touch_kb = (size_t)get_env_long("PREFETCH_TOUCH_KB", 64);
    long conc_env = get_env_long("PREFETCH_CONCURRENCY", 4);
    if (conc_env < 1) conc_env = 1;
    if (verbose()) printf("[PREFETCH THREAD] Started (files to prefetch: %zu, concurrency=%ld)\n", list_get_length(prefetch_list), conc_env);
    PrefetchCtx ctx;
    ctx.head = list_clone(prefetch_list);
    ctx.sleep_us = sleep_us;
    ctx.max_bytes = max_bytes;
    ctx.touch_kb = touch_kb;
    ctx.files = 0;
    ctx.sum_len = 0;
    ctx.sum_touch = 0;
    ctx.t0 = time(NULL);
    pthread_mutex_init(&ctx.lock, NULL);
    pthread_t* tids = (pthread_t*)malloc(sizeof(pthread_t) * (size_t)conc_env);
    if (!tids) {
        list_free(prefetch_list);
        pthread_mutex_destroy(&ctx.lock);
        pthread_exit(NULL);
    }
    for (long i = 0; i < conc_env; i++) {
        if (pthread_create(&tids[i], NULL, prefetch_worker, &ctx) != 0) {
            perror("[PREFETCH ERROR] pthread_create");
        }
    }
    for (long i = 0; i < conc_env; i++) {
        pthread_join(tids[i], NULL);
    }
    if (verbose()) printf("[PREFETCH THREAD] Finished\n");
    FILE* sf = fopen("time_summary.log", "w");
    if (sf) {
        fprintf(sf, "files=%zu\nbytes=%zu\ntouched=%zu\nstart=%ld\nend=%ld\n", ctx.files, ctx.sum_len, ctx.sum_touch, (long)ctx.t0, (long)time(NULL));
        fclose(sf);
    }
    list_free(ctx.head);
    list_free(prefetch_list);
    pthread_mutex_destroy(&ctx.lock);
    free(tids);
    pthread_exit(NULL);
}

int prefetch_create_thread(FileNode* prefetch_list) {
    if (prefetch_list == NULL) {
        fprintf(stderr, "[PREFETCH ERROR] No prefetch files to process\n");
        return -1;
    }

    pthread_t tid;
    // Create detached thread (automatic resource recovery, no need for main thread to wait)
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        perror("[PREFETCH ERROR] pthread_attr_init");
        return -1;
    }
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        perror("[PREFETCH ERROR] pthread_attr_setdetachstate");
        pthread_attr_destroy(&attr);
        return -1;
    }

    FileNode* list_copy = list_clone(prefetch_list);
    if (list_copy == NULL) {
        fprintf(stderr, "[PREFETCH ERROR] Failed to clone prefetch list\n");
        pthread_attr_destroy(&attr);
        return -1;
    }
    if (pthread_create(&tid, &attr, prefetch_thread, (void*)list_copy) != 0) {
        perror("[PREFETCH ERROR] pthread_create");
        list_free(list_copy);
        pthread_attr_destroy(&attr);
        return -1;
    }

    pthread_attr_destroy(&attr);
    printf("[PREFETCH] Created thread (TID: %lu) for %zu files\n", (unsigned long)tid, list_get_length(prefetch_list));
    return 0;
}