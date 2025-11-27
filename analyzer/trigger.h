#ifndef TRIGGER_H
#define TRIGGER_H
#include "reader.h"

// 分析主流程，负责区间识别、触发器选择和预取目标输出
void analyzer_main(const StatRecord *stat_records, int stat_count,
                   const ReadRecord *read_records, int read_count,
                   const MmapRecord *mmap_records, int mmap_count);

#endif