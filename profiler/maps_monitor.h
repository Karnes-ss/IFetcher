#ifndef MAPS_MONITOR_H
#define MAPS_MONITOR_H

#include "profiler_common.h"
#include <sys/types.h>
#include <time.h>

#define MAPS_MAX_ENTRY 1024
#define STARTUP_WINDOW_SEC 15

typedef struct {
    char filename[256];
    off_t start;
    off_t end;
    unsigned long long file_offset;
} MmapEntry;

// 设置监控启动时间（用于判断启动窗口）
void maps_set_start_time(time_t t);

// 初始化快照，并将当前映射填入 out_entries；返回条目数（>=0）
int maps_init_snapshot(pid_t pid, MmapEntry* out_entries, int max_out);

// 检查增量变化：发现新增映射立即写日志，并在启动窗口内暂存以便退出聚合写出
void check_mmap_changes(pid_t pid);

// 退出前统一写出启动窗口内新增的映射
void flush_startup_mmaps(pid_t pid);

#endif