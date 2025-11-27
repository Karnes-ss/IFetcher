#ifndef DISKSTATS_H
#define DISKSTATS_H

// 每次调用采样一次 /proc/diskstats 并写入增量日志
void monitor_disk_stats(void);

#endif