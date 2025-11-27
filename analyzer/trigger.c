#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "reader.h"
#include "density.h"
#define MAX_RECORDS 10000
    
// 预取请求结构体，用于合并和去重
typedef struct {
    char file_path[128];
    int offset;
    int length;
} PrefetchReq;
    
static int seen_prefetch(const PrefetchReq* a,int n,const char* path,int off,int len){
    for(int i=0;i<n;i++){ if(a[i].offset==off && a[i].length==len && strcmp(a[i].file_path,path)==0) return 1; }
    return 0;
}

static int skip_trigger_path(const char* path){
    if(!path) return 1;
    if (strncmp(path, "/tmp/ifetcher_profile/datareporting/glean/", 41) == 0) return 1;
    if (strstr(path, ".uuid") != NULL) return 1;
    if (strncmp(path, "/tmp/ifetcher_profile/safebrowsing/", 34) == 0) return 1;
    if (strncmp(path, "/usr/bin/", 9) == 0) return 1;
    if (strncmp(path, "/bin/", 5) == 0) return 1;
    if (strncmp(path, "/usr/share/drirc.d/", 20) == 0) return 1;
    if (strncmp(path, "/usr/share/fonts/", 17) == 0) return 1;
    if (strncmp(path, "/usr/share/locale/", 19) == 0) return 1;
    const char* env = getenv("ANALYZER_SKIP_PREFIXES");
    if (env && env[0] != '\0') {
        char buf[512]; strncpy(buf, env, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
        char* tok = strtok(buf, ",");
        while (tok) { size_t n = strlen(tok); if (n>0 && strncmp(path, tok, n)==0) return 1; tok = strtok(NULL, ","); }
    }
    return 0;
}

static int skip_ext_path(const char* path);
static int path_monitorable(const char* p){
    if (!p || p[0] != '/') return 0;
    if (strncmp(p, "/proc/", 6) == 0 || strncmp(p, "/sys/", 5) == 0 || strncmp(p, "/dev/", 5) == 0) return 0;
    if (skip_trigger_path(p)) return 0;
    if (skip_ext_path(p)) return 0;
    return 1;
}

static int get_env_int(const char* name,int def){ const char* s=getenv(name); if(!s||s[0]=='\0') return def; char* e=NULL; long v=strtol(s,&e,10); return (e==s)?def:(int)v; }
static long get_env_long(const char* name,long def){ const char* s=getenv(name); if(!s||s[0]=='\0') return def; char* e=NULL; long v=strtol(s,&e,10); return (e==s)?def:v; }
static int has_suffix(const char* p,const char* ext){ size_t lp=strlen(p), le=strlen(ext); if(lp<le) return 0; return strcmp(p+lp-le, ext)==0; }
static int skip_ext_path(const char* path){
    const char* env=getenv("ANALYZER_SKIP_EXTS");
    if(env && env[0]){
        char buf[512]; strncpy(buf,env,sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
        char* tok=strtok(buf,",");
        while(tok){ if(has_suffix(path,tok)) return 1; tok=strtok(NULL,","); }
    }
    if (has_suffix(path, ".vlpset")) return 1;
    return 0;
}
static int read_count_in_window(const ReadRecord* rr,int rc,const char* path,double t_min,double t_max){ if(!path) return 0; int cnt=0; for(int r=0;r<rc;r++){ double ts=rr[r].timestamp; if(ts<t_min||ts>t_max) continue; if(rr[r].file_path && strcmp(rr[r].file_path,path)==0) cnt++; } return cnt; }
static long bytes_in_window(const ReadRecord* rr,int rc,const char* path,double t_min,double t_max){ if(!path) return 0; long sum=0; for(int r=0;r<rc;r++){ double ts=rr[r].timestamp; if(ts<t_min||ts>t_max) continue; if(rr[r].file_path && strcmp(rr[r].file_path,path)==0) sum+=rr[r].req_len; } return sum; }
static int has_subseq_read(const ReadRecord* rr,int rc,const char* path,double t_start,double t_end){ if(!path) return 0; for(int r=0;r<rc;r++){ double ts=rr[r].timestamp; if(ts<=t_start||ts>t_end) continue; if(rr[r].file_path && strcmp(rr[r].file_path,path)==0) return 1; } return 0; }
static int same_dir(const char* a,const char* b){ if(!a||!b) return 0; const char* pa=strrchr(a,'/'); const char* pb=strrchr(b,'/'); if(!pa||!pb) return 0; size_t la=(size_t)(pa-a); size_t lb=(size_t)(pb-b); if(la!=lb) return 0; return strncmp(a,b,la)==0; }
/* removed unused path_monitorable_ext to silence warnings */

// 合并和去重预取请求（先按文件+偏移排序，再合并连续区间）
int merge_prefetch_requests(const PrefetchReq *in, int in_count, PrefetchReq *out, int *out_count) {
    PrefetchReq tmp[MAX_RECORDS];
    int n = in_count;
    for (int i = 0; i < n; i++) tmp[i] = in[i];
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int cmp = strcmp(tmp[i].file_path, tmp[j].file_path);
            if (cmp > 0 || (cmp == 0 && tmp[i].offset > tmp[j].offset)) {
                PrefetchReq t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
            }
        }
    }
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (cnt > 0 && strcmp(out[cnt-1].file_path, tmp[i].file_path) == 0 &&
            tmp[i].offset == out[cnt-1].offset + out[cnt-1].length) {
            out[cnt-1].length += tmp[i].length;
            continue;
        }
        strcpy(out[cnt].file_path, tmp[i].file_path);
        out[cnt].offset = tmp[i].offset;
        out[cnt].length = tmp[i].length;
        cnt++;
    }
    *out_count = cnt;
    return cnt;
}

// 分析主流程：区间识别、触发器选择、预取目标输出
void analyzer_main(const StatRecord *stat_records, int stat_count,
                   const ReadRecord *read_records, int read_count,
                   const MmapRecord *mmap_records, int mmap_count) {
    // ... existing code ...
    // Algorithm 1: I/O 密度与密度变化量
    int window_size = 11;                    // 奇数窗口
    if (stat_count < window_size) {
        if (stat_count >= 9) window_size = 9;
        else if (stat_count >= 5) window_size = 5;
        else window_size = 3;
    }
    double weight[11];
    estimate_weight(weight, window_size);

    double ts_density[MAX_RECORDS] = {0.0};
    get_IO_density(stat_records, stat_count, ts_density, window_size, weight);

    double delta_ts_density[MAX_RECORDS] = {0.0};
    for (int t = 1; t < stat_count; t++) {
        delta_ts_density[t] = ts_density[t] - ts_density[t - 1];
    }

    // 构造“局部最小→局部最大”候选区间并累计 sum_delta
    typedef struct { int min_i; int max_i; double sum_delta; } Candidate;
    Candidate cand[MAX_RECORDS];
    int cand_cnt = 0;
    int in_range = 0, cur_min = -1;
    double sum_delta = 0.0;
    for (int t = 1; t < stat_count - 1; t++) {
        if (is_local_min(ts_density, t, stat_count)) {
            in_range = 1;
            cur_min = t;
            sum_delta = 0.0;
        }
        if (in_range) sum_delta += delta_ts_density[t];
        if (is_local_max(ts_density, t, stat_count) && in_range) {
            if (sum_delta > 0.0) {
                cand[cand_cnt].min_i = cur_min;
                cand[cand_cnt].max_i = t;
                cand[cand_cnt].sum_delta = sum_delta;
                cand_cnt++;
            }
            in_range = 0;
        }
    }

    // 按 sum_delta 降序排序（越大表示空闲后增长越剧烈）
    for (int i = 0; i < cand_cnt; i++) {
        for (int j = i + 1; j < cand_cnt; j++) {
            if (cand[j].sum_delta > cand[i].sum_delta) {
                Candidate tmp = cand[i]; cand[i] = cand[j]; cand[j] = tmp;
            }
        }
    }

    if (cand_cnt == 0 && stat_count > 1) {
        int min_i = -1, max_i = -1;
        double min_v = 1e300, max_v = -1e300;
        for (int t = 0; t < stat_count; t++) {
            double v = ts_density[t];
            if (v < min_v) { min_v = v; min_i = t; }
            if (v > max_v) { max_v = v; max_i = t; }
        }
        if (min_i >= 0 && max_i >= 0 && min_i < max_i) {
            cand[0].min_i = min_i;
            cand[0].max_i = max_i;
            double s = 0.0;
            for (int t = min_i + 1; t <= max_i; t++) s += delta_ts_density[t];
            cand[0].sum_delta = s;
            cand_cnt = 1;
        }
    }

    FILE *trigger_fp = fopen("trigger_log.txt", "w");
    FILE *prefetch_fp = fopen("prefetch_log.txt", "w");
    if (!trigger_fp || !prefetch_fp) {
        if (trigger_fp) fclose(trigger_fp);
        if (prefetch_fp) fclose(prefetch_fp);
        return;
    }
    /* 输出日志首行复制 APP=...，prefetch_log 用 ===TRIGGER=== 分段；简化实现，避免不必要的文件检查与重复输出 */
    {
        const char* log_dir = getenv("IFETCHER_LOG_DIR");
        char rp[256]; char mp[256];
        if (log_dir && log_dir[0] != '\0') {
            snprintf(rp, sizeof(rp), "%s/%s", log_dir, "read_log");
            snprintf(mp, sizeof(mp), "%s/%s", log_dir, "mmap_log");
        } else {
            snprintf(rp, sizeof(rp), "%s", "/tmp/read_log");
            snprintf(mp, sizeof(mp), "%s", "/tmp/mmap_log");
        }
        FILE* r = fopen(rp, "r");
        if (r) {
            char line[512]; int ok = 0; for (int k = 0; k < 32 && fgets(line, sizeof(line), r); k++) { if (strncmp(line, "APP=", 4) == 0) { fprintf(trigger_fp, "%s", line); fprintf(prefetch_fp, "%s", line); ok = 1; break; } }
            fclose(r);
            if (!ok) {
                FILE* m = fopen(mp, "r");
                if (m) { for (int k = 0; k < 32 && fgets(line, sizeof(line), m); k++) { if (strncmp(line, "APP=", 4) == 0) { fprintf(trigger_fp, "%s", line); fprintf(prefetch_fp, "%s", line); break; } } fclose(m); }
            }
        }
    }

    
    
    int K = 5;
    double tau_list[] = {4.0, 2.0, 1.0, 0.5};
    int tau_n = 4;

    for (int c = 0; c < cand_cnt && c < K; c++) {
        double t_min = stat_records[cand[c].min_i].timestamp;
        double t_max = stat_records[cand[c].max_i].timestamp;
        int extend = get_env_int("ANALYZER_TMAX_EXTEND_SEC", 3);
        double t_max2 = t_max + (double)extend;

        // Algorithm 2：在触发窗口内按“首个文件访问”选触发点
        int trigger_idx = -1;
        char trig_path[128]; int trig_off = 0; int trig_len = 0; double trig_ts = 0.0; int trig_set = 0;
        for (int ti = 0; ti < tau_n && !trig_set; ti++) {
            double t0 = t_min - tau_list[ti];
            double t1 = t_min;

            typedef struct { char path[128]; double ts; int from_read; int read_idx; int mmap_idx; } FirstAccess;
            FirstAccess firsts[256];
            int fc = 0;

            // 合并 mmap 首访
            for (int m = 0; m < mmap_count && fc < 256; m++) {
                double ts = mmap_records[m].timestamp;
                if (ts < t0 || ts > t1) continue;
                const char *path = mmap_records[m].file_path;
                if (!path || path[0] != '/') continue;
                if (skip_trigger_path(path)) continue;
                int exists = 0;
                for (int q = 0; q < fc; q++) if (strcmp(firsts[q].path, path) == 0) { exists = 1; break; }
                if (!exists) { strcpy(firsts[fc].path, path); firsts[fc].ts = ts; firsts[fc].from_read = 0; firsts[fc].read_idx = -1; firsts[fc].mmap_idx = m; fc++; }
            }
            // 合并 read 首访（并可能更新更早时间）
            for (int r = 0; r < read_count && fc < 256; r++) {
                double ts = read_records[r].timestamp;
                if (ts < t0 || ts > t1) continue;
                const char *path = read_records[r].file_path;
                if (!path || path[0] != '/') continue;
                if (skip_trigger_path(path)) continue;
                int pos = -1;
                for (int q = 0; q < fc; q++) if (strcmp(firsts[q].path, path) == 0) { pos = q; break; }
                if (pos < 0) { strcpy(firsts[fc].path, path); firsts[fc].ts = ts; firsts[fc].from_read = 1; firsts[fc].read_idx = r; fc++; }
                else if (ts < firsts[pos].ts) { firsts[pos].ts = ts; firsts[pos].from_read = 1; firsts[pos].read_idx = r; }
            }
            int earliest = -1;
            for (int x = 0; x < fc; x++) {
                if (skip_trigger_path(firsts[x].path)) continue;
                if (earliest < 0 || firsts[x].ts < firsts[earliest].ts) earliest = x;
            }
            int candidate = -1;
            int min_reads = get_env_int("ANALYZER_MIN_READS_IN_WINDOW", 2);
            long min_bytes = get_env_long("ANALYZER_MIN_BYTES_IN_WINDOW", 32768);
            long best_bsum = -1;
            for (int x = 0; x < fc; x++) {
                if (skip_trigger_path(firsts[x].path)) continue;
                int cnt = read_count_in_window(read_records, read_count, firsts[x].path, t_min, t_max2);
                long bsum = bytes_in_window(read_records, read_count, firsts[x].path, t_min, t_max2);
                if (cnt >= min_reads && bsum >= min_bytes && has_subseq_read(read_records, read_count, firsts[x].path, firsts[x].ts, t_max)) {
                    if (bsum > best_bsum || (bsum == best_bsum && (candidate < 0 || firsts[x].ts < firsts[candidate].ts))) { candidate = x; best_bsum = bsum; }
                }
            }
            if (candidate < 0) continue;

            if (candidate >= 0) {
                if (firsts[candidate].from_read && firsts[candidate].read_idx >= 0) {
                    trigger_idx = firsts[candidate].read_idx;
                    strcpy(trig_path, read_records[trigger_idx].file_path);
                    trig_off = read_records[trigger_idx].offset;
                    trig_len = read_records[trigger_idx].req_len;
                    trig_ts = read_records[trigger_idx].timestamp;
                    trig_set = 1;
                } else {
                    for (int r = 0; r < read_count; r++) {
                        double ts = read_records[r].timestamp;
                        if (ts < t0 || ts > t1) continue;
                        if (strcmp(read_records[r].file_path, firsts[candidate].path) == 0) { trigger_idx = r; strcpy(trig_path, read_records[r].file_path); trig_off = read_records[r].offset; trig_len = read_records[r].req_len; trig_ts = read_records[r].timestamp; trig_set = 1; break; }
                    }
                    if (!trig_set) { strcpy(trig_path, firsts[candidate].path); int mi = firsts[candidate].mmap_idx; trig_off = mmap_records[mi].file_offset; trig_len = mmap_records[mi].size; trig_ts = mmap_records[mi].timestamp; trig_set = 1; }
                }
            }
        }

        if (trig_set && !path_monitorable(trig_path)) trig_set = 0;
        if (!trig_set) continue;

        PrefetchReq prefetches[MAX_RECORDS];
        int prefetch_cnt = 0;
        size_t out_bytes = 0;
        int out_items = 0;
        long max_items = 12, max_bytes = 262144;
        { const char* s = getenv("ANALYZER_PREFETCH_MAX_ITEMS"); if (s && s[0] != '\0') { char* e = NULL; long v = strtol(s, &e, 10); if (e != s) max_items = v; } }
        { const char* s = getenv("ANALYZER_PREFETCH_MAX_BYTES"); if (s && s[0] != '\0') { char* e = NULL; long v = strtol(s, &e, 10); if (e != s) max_bytes = v; } }
        int dir_group = get_env_int("ANALYZER_DIR_GROUPING", 0);

        
        for (int r = 0; r < read_count && prefetch_cnt < MAX_RECORDS; r++) {
            double ts = read_records[r].timestamp;
            if (ts <= trig_ts) continue;
            if (ts > t_max2) break;
            const char* p = read_records[r].file_path;
            if (!p || p[0] != '/' || !path_monitorable(p)) continue;
            if (dir_group && !same_dir(p, trig_path)) continue;
            if (strncmp(p, "/proc/", 6) == 0 || strncmp(p, "/sys/", 5) == 0 || strncmp(p, "/dev/", 5) == 0) continue;
            int off = read_records[r].offset, len = read_records[r].req_len;
            if (strcmp(p, trig_path) == 0 && off == trig_off && len == trig_len) continue;
            if (max_items > 0 && out_items >= (int)max_items) break;
            if (max_bytes > 0 && out_bytes + len > (size_t)max_bytes) break;
            if (seen_prefetch(prefetches, prefetch_cnt, p, off, len)) continue;
            strcpy(prefetches[prefetch_cnt].file_path, p);
            prefetches[prefetch_cnt].offset = off;
            prefetches[prefetch_cnt].length = len;
            prefetch_cnt++;
            out_items++;
            out_bytes += len;
        }
        for (int m = 0; m < mmap_count && prefetch_cnt < MAX_RECORDS; m++) {
            double ts = mmap_records[m].timestamp;
            if (ts < trig_ts || ts > t_max2) continue;
            const char* p = mmap_records[m].file_path;
            if (!p || p[0] != '/' || !path_monitorable(p)) continue;
            if (dir_group && !same_dir(p, trig_path)) continue;
            if (strncmp(p, "/proc/", 6) == 0 || strncmp(p, "/sys/", 5) == 0 || strncmp(p, "/dev/", 5) == 0) continue;
            int off = mmap_records[m].file_offset;
            int len = mmap_records[m].size;
            if (len <= 0) continue;
            if (strcmp(p, trig_path) == 0 && off == trig_off && len == trig_len) continue;
            if (max_items > 0 && out_items >= (int)max_items) break;
            if (max_bytes > 0 && out_bytes + len > (size_t)max_bytes) break;
            if (seen_prefetch(prefetches, prefetch_cnt, p, off, len)) continue;
            strcpy(prefetches[prefetch_cnt].file_path, p);
            prefetches[prefetch_cnt].offset = off;
            prefetches[prefetch_cnt].length = len;
            prefetch_cnt++;
            out_items++;
            out_bytes += len;
        }

        if (prefetch_cnt > 0) {
            fprintf(trigger_fp, "%s,%d,%d\n", trig_path, trig_off, trig_len);
            fprintf(prefetch_fp, "===TRIGGER===\n");
            fprintf(prefetch_fp, "%s,%d,%d\n", trig_path, trig_off, trig_len);
            const char* no_merge = getenv("IFETCHER_NO_MERGE");
            if (no_merge && no_merge[0] && strcmp(no_merge, "0") != 0) {
                for (int m = 0; m < prefetch_cnt; m++) {
                    fprintf(prefetch_fp, "%s,%d,%d\n", prefetches[m].file_path, prefetches[m].offset, prefetches[m].length);
                }
            } else {
                PrefetchReq merged[MAX_RECORDS];
                int merged_cnt = 0;
                merge_prefetch_requests(prefetches, prefetch_cnt, merged, &merged_cnt);
                for (int m = 0; m < merged_cnt; m++) {
                    fprintf(prefetch_fp, "%s,%d,%d\n", merged[m].file_path, merged[m].offset, merged[m].length);
                }
            }
        }
    }

    

    fclose(trigger_fp);
    fclose(prefetch_fp);
}