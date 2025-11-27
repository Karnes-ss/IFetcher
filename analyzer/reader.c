#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "reader.h"
#define LINE_MAX 256

// 解析方括号时间戳为 epoch 秒，例如 "[2025-11-12 21:54:13]"
static double parse_bracket_ts(const char *line) {
    const char *start = strchr(line, '[');
    if (!start) return 0.0;
    const char *end = strchr(start, ']');
    if (!end) return 0.0;
    char buf[32];
    size_t n = (size_t)(end - start - 1);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, start + 1, n);
    buf[n] = '\0';
    int year, mon, day, hour, min, sec;
    if (sscanf(buf, "%d-%d-%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec) != 6) return 0.0;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = min;
    tmv.tm_sec = sec;
    time_t t = mktime(&tmv);
    return (double)t;
}

// 读取 stat_log 文件内容到 StatRecord 数组（解析 io_time_ms，并累计 total_io）
int load_stat_log(const char *filename, StatRecord *records) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return 0;
    int count = 0;
    char line[LINE_MAX];
    double cum_io_ms = 0.0;
    while (fgets(line, LINE_MAX, fp)) {
        if (strstr(line, "Device:") == NULL) continue;
        double ts = parse_bracket_ts(line);
        const char *p = strstr(line, "io_time_ms:");
        if (!p) continue;
        long long io_ms = 0;
        if (sscanf(p, "io_time_ms:%lld", &io_ms) != 1) continue;
        records[count].timestamp = ts;
        records[count].delta_io = (double)io_ms;  // 该周期的 I/O 活动强度
        cum_io_ms += (double)io_ms;
        records[count].total_io = cum_io_ms;      // 累计总和，供参考
        count++;
        if (count >= MAX_RECORDS) break;
    }
    fclose(fp);
    return count;
}

// 读取 read_log 文件内容到 ReadRecord 数组（过滤非磁盘路径）
int load_read_log(const char *filename, ReadRecord *records) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return 0;
    int count = 0;
    char line[LINE_MAX];
    while (fgets(line, LINE_MAX, fp)) {
        if (!strstr(line, "Type:READ") && !strstr(line, "Type:FREAD")) continue;
        double ts = parse_bracket_ts(line);

        // File 路径
        const char *pf = strstr(line, "File:");
        if (!pf) continue;
        pf += strlen("File:");
        const char *pf_end = strstr(pf, " | ");
        int lfile = pf_end ? (int)(pf_end - pf) : (int)strcspn(pf, "\n");
        if (lfile <= 0) continue;
        char file_path[128];
        if (lfile > 127) lfile = 127;
        memcpy(file_path, pf, lfile);
        file_path[lfile] = '\0';
        // 仅保留真实磁盘路径，忽略 pipe:/anon_inode: 等
        if (file_path[0] != '/') continue;

        // 偏移与大小
        int offset = 0, size = 0;
        const char *po = strstr(line, "Offset:");
        const char *ps = strstr(line, "Size:");
        if (!po || !ps) continue;
        if (sscanf(po, "Offset:%d", &offset) != 1) continue;
        if (sscanf(ps, "Size:%d", &size) != 1) continue;

        records[count].timestamp = ts;
        strcpy(records[count].file_path, file_path);
        records[count].offset = offset;
        records[count].req_len = size;
        records[count].read_len = size;   // 未提供实际读长度，默认等同请求长度
        records[count].io_time = 0.0;     // 未提供 I/O 耗时，设为 0
        count++;
        if (count >= MAX_RECORDS) break;
    }
    fclose(fp);
    return count;
}

// 读取 mmap_log 文件内容到 MmapRecord 数组
int load_mmap_log(const char *filename, MmapRecord *records) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return 0;
    int count = 0;
    char line[LINE_MAX];
    while (fgets(line, LINE_MAX, fp)) {
        if (!strstr(line, "Type:MMAP")) continue;
        double ts = parse_bracket_ts(line);

        const char *pf = strstr(line, "File:");
        const char *ps = strstr(line, "AddrStart:");
        const char *pe = strstr(line, "AddrEnd:");
        const char *po = strstr(line, "FileOffset:");
        const char *pz = strstr(line, "Size:");
        if (!pf || !ps || !pe || !po || !pz) continue;

        pf += strlen("File:");
        const char *pf_end = strstr(pf, " | ");
        int lfile = pf_end ? (int)(pf_end - pf) : (int)strcspn(pf, "\n");
        if (lfile <= 0) continue;
        char file_path[128];
        if (lfile > 127) lfile = 127;
        memcpy(file_path, pf, lfile);
        file_path[lfile] = '\0';

        long long addr_start = 0, addr_end = 0, file_off = 0, sz = 0;
        if (sscanf(ps, "AddrStart:%lld", &addr_start) != 1) continue;
        if (sscanf(pe, "AddrEnd:%lld", &addr_end) != 1) continue;
        if (sscanf(po, "FileOffset:%lld", &file_off) != 1) continue;
        if (sscanf(pz, "Size:%lld", &sz) != 1) continue;

        records[count].timestamp = ts;
        snprintf(records[count].start_addr, sizeof(records[count].start_addr), "%lld", addr_start);
        snprintf(records[count].end_addr, sizeof(records[count].end_addr), "%lld", addr_end);
        strcpy(records[count].file_path, file_path);
        records[count].file_offset = (int)file_off;
        records[count].size = (int)sz;
        count++;
        if (count >= MAX_RECORDS) break;
    }
    fclose(fp);
    return count;
}