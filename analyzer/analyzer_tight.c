/*
 * analyzer_tight.c
 * 收紧触发器选择：
 * 1. 仅当记录为mmap或单次读>4KB才考虑触发；
 * 2. 同一文件5秒内只触发一次；
 * 3. 单触发器预取条目≤32、总大小≤256KB；
 * 4. 输出格式保持兼容。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "reader.h"
static char g_data_dir[256];
static int get_env_int(const char* name, int defv){ const char* s=getenv(name); if(!s||!*s) return defv; char* e=NULL; long v=strtol(s,&e,10); if(e==s) return defv; return (int)v; }
static double get_env_double(const char* name, double defv){ const char* s=getenv(name); if(!s||!*s) return defv; char* e=NULL; double v=strtod(s,&e); if(e==s) return defv; return v; }
static int seen_in_reads_all(const ReadRecord* rr,int rc,const char* path){ if(!path) return 0; for(int i=0;i<rc;i++){ if(rr[i].file_path && strcmp(rr[i].file_path,path)==0) return 1; } return 0; }
static void canonical_path(const char* in,char* out,size_t outsz){ if(!in){ if(outsz>0) out[0]='\0'; return;} char r[512]; char* rp = realpath(in, r); if(rp){ strncpy(out, rp, outsz-1); out[outsz-1]='\0'; } else { strncpy(out, in, outsz-1); out[outsz-1]='\0'; } }
typedef struct { char path[256]; int off; int len; } Assigned;
static Assigned assigned[MAX_RECORDS];
static int assigned_cnt = 0;
static int assigned_has(const char* p,int off,int len){ if(!p) return 0; for(int i=0;i<assigned_cnt;i++){ if(strcmp(assigned[i].path,p)==0 && assigned[i].off==off && assigned[i].len==len) return 1; } return 0; }
static void assigned_add(const char* p,int off,int len){ if(!p) return; if(assigned_cnt<MAX_RECORDS){ strncpy(assigned[assigned_cnt].path,p,sizeof(assigned[assigned_cnt].path)-1); assigned[assigned_cnt].path[sizeof(assigned[assigned_cnt].path)-1]='\0'; assigned[assigned_cnt].off=off; assigned[assigned_cnt].len=len; assigned_cnt++; } }
typedef struct { char path[256]; } AssignedPath;
static AssignedPath assigned_paths[MAX_RECORDS];
static int assigned_paths_cnt = 0;
static int assigned_path_has(const char* p){ if(!p) return 0; for(int i=0;i<assigned_paths_cnt;i++){ if(strcmp(assigned_paths[i].path,p)==0) return 1; } return 0; }
static void assigned_path_add(const char* p){ if(!p) return; if(assigned_paths_cnt<MAX_RECORDS){ strncpy(assigned_paths[assigned_paths_cnt].path,p,sizeof(assigned_paths[assigned_paths_cnt].path)-1); assigned_paths[assigned_paths_cnt].path[sizeof(assigned_paths[assigned_paths_cnt].path)-1]='\0'; assigned_paths_cnt++; } }

static int MAX_PREFETCH_PER_TRIGGER = 16;
static int MAX_PREFETCH_BYTES  = (128*1024);
static int MAX_LEN_PER_ITEM    = (64*1024);
static double SAME_FILE_COOLDOWN_SEC = 5.0;
static int READ_SIZE_THRESHOLD = 4096;
static double PREFETCH_WINDOW_SEC = 3.0;
static int MIN_WINDOW_READS = 2;
static int MIN_WINDOW_BYTES = 32*1024;

static int is_legal_path(const char *p) {
    if (!p || p[0] != '/') return 0;
    if (strncmp(p, "/proc/", 6) == 0) return 0;
    if (strncmp(p, "/sys/", 5) == 0) return 0;
    if (strncmp(p, "/dev/", 5) == 0) return 0;
    if (strstr(p, "/usr/share/drirc.d/") == p) return 0;
    if (g_data_dir[0] && strncmp(p, g_data_dir, strlen(g_data_dir)) == 0) return 1;
    if (strstr(p, "/maps/") != NULL) return 1;
    if (strncmp(p, "/usr/", 5) == 0) return 1;
    if (strncmp(p, "/lib/", 5) == 0) return 1;
    if (strncmp(p, "/tmp/", 5) == 0) return 1;
    return 0;
}

static int has_suffix(const char* p,const char* ext){ size_t lp=strlen(p), le=strlen(ext); if(lp<le) return 0; return strcmp(p+lp-le, ext)==0; }
static int skip_ext_path(const char* path){
    if (has_suffix(path, ".ini")) return 1;
    if (has_suffix(path, ".conf")) return 1;
    if (has_suffix(path, ".txt")) return 1;
    return 0;
}
/* 触发器不强制扩展类型偏好，保留在预取项上做过滤 */

/* 记录文件最近一次触发时间 */
static double last_trigger_ts(const char *path, char paths[MAX_RECORDS][256], double tss[MAX_RECORDS], int n) {
    for (int i = 0; i < n; i++)
        if (strcmp(paths[i], path) == 0) return tss[i];
    return -1.0;
}
static void set_trigger_ts(const char *path, double ts, char paths[MAX_RECORDS][256], double tss[MAX_RECORDS], int *n) {
    int idx = -1;
    for (int i = 0; i < *n; i++)
        if (strcmp(paths[i], path) == 0) { idx = i; break; }
    if (idx < 0 && *n < MAX_RECORDS) idx = (*n)++;
    if (idx >= 0) {
        strcpy(paths[idx], path);
        tss[idx] = ts;
    }
}

int main() {
    assigned_cnt = 0; assigned_paths_cnt = 0;

    MAX_PREFETCH_PER_TRIGGER = get_env_int("IFETCHER_PREFETCH_TOP_N", 16);
    SAME_FILE_COOLDOWN_SEC = get_env_double("IFETCHER_SAME_FILE_COOLDOWN_SEC", 5.0);
    READ_SIZE_THRESHOLD = get_env_int("IFETCHER_READ_THRESHOLD", 4096);
    PREFETCH_WINDOW_SEC = get_env_double("IFETCHER_WINDOW_SEC", 3.0);
    MIN_WINDOW_READS = get_env_int("IFETCHER_MIN_READS", 2);
    MIN_WINDOW_BYTES = get_env_int("IFETCHER_MIN_BYTES", 32*1024);
    MAX_PREFETCH_BYTES = get_env_int("IFETCHER_MAX_PREFETCH_BYTES_KB", 128) * 1024;
    MAX_LEN_PER_ITEM   = get_env_int("IFETCHER_MAX_LEN_PER_ITEM_KB", 64) * 1024;

    const char* dd = getenv("IFETCHER_DATA_DIR");
    if (dd && dd[0] != '\0') { strncpy(g_data_dir, dd, sizeof(g_data_dir)-1); g_data_dir[sizeof(g_data_dir)-1]='\0'; }
    const char *log_dir = getenv("IFETCHER_LOG_DIR");
    char read_path[256], mmap_path[256];
    if (log_dir && log_dir[0] != '\0') {
        snprintf(read_path, sizeof(read_path), "%s/read_log", log_dir);
        snprintf(mmap_path, sizeof(mmap_path), "%s/mmap_log", log_dir);
    } else {
        strcpy(read_path, "/tmp/read_log");
        strcpy(mmap_path, "/tmp/mmap_log");
    }
    ReadRecord  *reads  = malloc(sizeof(ReadRecord)  * MAX_RECORDS);
    MmapRecord  *mmaps  = malloc(sizeof(MmapRecord)  * MAX_RECORDS);
    int read_cnt = load_read_log(read_path, reads);
    int mmap_cnt = load_mmap_log(mmap_path, mmaps);
    if (read_cnt < 0) read_cnt = 0;
    if (mmap_cnt < 0) mmap_cnt = 0;

    /* 合并时间线 */
    typedef struct { double ts; int is_read; int idx; } Event;
    Event *events = malloc(sizeof(Event) * (read_cnt + mmap_cnt));
    int ec = 0;
    for (int i = 0; i < read_cnt; i++) {
        events[ec].ts = reads[i].timestamp;
        events[ec].is_read = 1;
        events[ec].idx = i;
        ec++;
    }
    for (int i = 0; i < mmap_cnt; i++) {
        events[ec].ts = mmaps[i].timestamp;
        events[ec].is_read = 0;
        events[ec].idx = i;
        ec++;
    }
    /* 按时间升序 */
    for (int i = 0; i < ec; i++) {
        for (int j = i + 1; j < ec; j++) {
            if (events[j].ts < events[i].ts) {
                Event t = events[i]; events[i] = events[j]; events[j] = t;
            }
        }
    }

    FILE *ft = fopen("trigger_log.txt", "w");
    FILE *fp = fopen("prefetch_log.txt", "w");
    if (!ft || !fp) {
        perror("fopen output");
        return 1;
    }
    /* 写APP行 */
    FILE *fr = fopen(read_path, "r");
    if (fr) {
        char line[512];
        if (fgets(line, sizeof(line), fr)) {
            if (strncmp(line, "APP=", 4) == 0) {
                fputs(line, ft);
                fputs(line, fp);
            }
        }
        fclose(fr);
    }
    double start_ts = get_env_double("IFETCHER_START_TS", -1.0);
    int allow_mmap_only = get_env_int("IFETCHER_ALLOW_MMAP_ONLY", 0);

    fprintf(stderr, "[Analyzer] Loaded %d reads, %d mmaps\n", read_cnt, mmap_cnt);
    if (start_ts > 0) {
        int any_after = 0;
        for (int i = 0; i < ec; i++) { if (events[i].ts >= start_ts) { any_after = 1; break; } }
        if (!any_after) start_ts = -1.0;
    }
    fprintf(stderr, "[Analyzer] Start TS: %.2f\n", start_ts);
    fprintf(stderr, "[Analyzer] READ_THRESHOLD: %d, COOLDOWN: %.2f, WINDOW: %.2f\n", READ_SIZE_THRESHOLD, SAME_FILE_COOLDOWN_SEC, PREFETCH_WINDOW_SEC);
    fprintf(stderr, "[Analyzer] ALLOW_MMAP_ONLY: %d\n", allow_mmap_only);

    char cool_paths[MAX_RECORDS][256];
    double cool_tss[MAX_RECORDS];
    int cool_cnt = 0;

    typedef struct { int idx; long bsum; int rcnt; char path[256]; int off; int len; double ts; } Cand;
    Cand cand[MAX_RECORDS];
    int cand_cnt = 0;

    int rejected_ts = 0;
    int rejected_len = 0;
    int rejected_mmap_rule = 0;
    int rejected_path = 0;
    int rejected_cooldown = 0;
    int passed_cand = 0;

    for (int i = 0; i < ec; i++) {
        if (start_ts>0 && events[i].ts < start_ts) { rejected_ts++; continue; }
        const char *path = NULL; int offset = 0, len = 0;
        if (events[i].is_read) {
            ReadRecord *r = &reads[events[i].idx]; path = r->file_path; offset = r->offset; len = r->req_len;
            if (len < READ_SIZE_THRESHOLD) { rejected_len++; continue; }
        } else {
            MmapRecord *m = &mmaps[events[i].idx]; path = m->file_path; offset = m->file_offset; len = m->size;
            if (len < READ_SIZE_THRESHOLD) { rejected_len++; continue; }
            if (!allow_mmap_only && !seen_in_reads_all(reads, read_cnt, path)) { rejected_mmap_rule++; continue; }
        }
        if (!is_legal_path(path)) { rejected_path++; continue; }
        { char tp[512]; canonical_path(path, tp, sizeof(tp)); if (assigned_path_has(tp)) continue; }
        double last = last_trigger_ts(path, cool_paths, cool_tss, cool_cnt);
        if (last >= 0 && (events[i].ts - last) < SAME_FILE_COOLDOWN_SEC) { rejected_cooldown++; continue; }
        
        passed_cand++;
        double t_end = events[i].ts + PREFETCH_WINDOW_SEC;
        long bsum = 0; int rcnt = 0;
        for (int j = i + 1; j < ec && events[j].ts <= t_end; j++) {
            if (start_ts>0 && events[j].ts < start_ts) continue;
            const char *p2 = NULL; int l2 = 0;
            if (events[j].is_read) { p2 = reads[events[j].idx].file_path; l2 = reads[events[j].idx].req_len; }
            else { p2 = mmaps[events[j].idx].file_path; l2 = mmaps[events[j].idx].size; }
            if (!is_legal_path(p2)) continue;
            /* 预取项不再要求同目录，保留扩展过滤避免配置/图片类 */
            if (skip_ext_path(p2)) continue;
            if (l2 <= 0) continue;
            bsum += l2; rcnt++;
        }
        if (rcnt >= MIN_WINDOW_READS && bsum >= MIN_WINDOW_BYTES) {
            if (cand_cnt < MAX_RECORDS) {
                cand[cand_cnt].idx = i; cand[cand_cnt].bsum = bsum; cand[cand_cnt].rcnt = rcnt; strncpy(cand[cand_cnt].path, path, sizeof(cand[cand_cnt].path)-1); cand[cand_cnt].path[sizeof(cand[cand_cnt].path)-1] = '\0'; cand[cand_cnt].off = offset; cand[cand_cnt].len = len; cand[cand_cnt].ts = events[i].ts; cand_cnt++;
                set_trigger_ts(path, events[i].ts, cool_paths, cool_tss, &cool_cnt);
            }
        }
    }
    for (int a = 0; a < cand_cnt; a++) { for (int b = a + 1; b < cand_cnt; b++) { if (cand[b].bsum > cand[a].bsum) { Cand t = cand[a]; cand[a] = cand[b]; cand[b] = t; } } }
    int segments_out = 0; const int MAX_TRIGGERS = get_env_int("IFETCHER_MAX_TRIGGERS", 3);
    for (int k = 0; k < cand_cnt && segments_out < MAX_TRIGGERS; k++) {
        int i = cand[k].idx; const char* path = cand[k].path; int offset = cand[k].off; int len = cand[k].len; if (len > MAX_LEN_PER_ITEM) len = MAX_LEN_PER_ITEM; char cpath[512]; canonical_path(path, cpath, sizeof(cpath));
        fprintf(ft, "%s,%d,%d\n", cpath, offset, len);
        fprintf(fp, "===TRIGGER===\n");
        fprintf(fp, "%s,%d,%d\n", cpath, offset, len);
        double t_end = cand[k].ts + PREFETCH_WINDOW_SEC;
        int out_items = 0; long out_bytes = 0;
        for (int j = i + 1; j < ec && events[j].ts <= t_end; j++) {
            if (out_items >= MAX_PREFETCH_PER_TRIGGER) break;
            if (out_bytes >= MAX_PREFETCH_BYTES) break;
            const char *p2 = NULL; int o2 = 0; int l2 = 0;
            if (events[j].is_read) { p2 = reads[events[j].idx].file_path; o2 = reads[events[j].idx].offset; l2 = reads[events[j].idx].req_len; }
            else { p2 = mmaps[events[j].idx].file_path; o2 = mmaps[events[j].idx].file_offset; l2 = mmaps[events[j].idx].size; }
            if (!is_legal_path(p2)) continue;
            if (skip_ext_path(p2)) continue;
            if (strcmp(p2, path) == 0 && o2 == offset) continue;
            if (l2 <= 0) continue;
            if (l2 > MAX_LEN_PER_ITEM) l2 = MAX_LEN_PER_ITEM;
            {
                char cp[512];
                canonical_path(p2, cp, sizeof(cp));
                if (!assigned_has(cp, o2, l2)) {
                    fprintf(fp, "%s,%d,%d\n", cp, o2, l2);
                    assigned_add(cp, o2, l2);
                    assigned_path_add(cp);
                }
            }
            out_items++; out_bytes += l2;
        }
        segments_out++;
    }

    fprintf(stderr, "[Analyzer] Events processed: %d\n", ec);
    fprintf(stderr, "[Analyzer] Rejected by TS: %d\n", rejected_ts);
    fprintf(stderr, "[Analyzer] Rejected by Len: %d\n", rejected_len);
    fprintf(stderr, "[Analyzer] Rejected by MmapRule: %d\n", rejected_mmap_rule);
    fprintf(stderr, "[Analyzer] Rejected by Path: %d\n", rejected_path);
    fprintf(stderr, "[Analyzer] Rejected by Cooldown: %d\n", rejected_cooldown);
    fprintf(stderr, "[Analyzer] Candidates found: %d\n", passed_cand);
    fprintf(stderr, "[Analyzer] Segments generated: %d\n", segments_out);

    fclose(ft);
    fclose(fp);
    free(reads); free(mmaps); free(events);
    return 0;
}
