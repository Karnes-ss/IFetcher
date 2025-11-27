#ifndef READER_H
#define READER_H
#define MAX_RECORDS 10000

// StatRecord：用于存储 stat_log 的每条磁盘状态记录
typedef struct {
    double timestamp;   // 时间戳
    double delta_io;    // 本周期 I/O 时间
    double total_io;    // 总 I/O 时间
} StatRecord;

// ReadRecord：用于存储 read_log 的每条读操作记录
typedef struct {
    double timestamp;       // 时间戳
    char file_path[128];    // 文件路径
    int offset, req_len, read_len; // 偏移量、请求长度、实际读取长度
    double io_time;         // I/O 操作耗时
} ReadRecord;

// MmapRecord：用于存储 mmap_log 的每条映射记录
typedef struct {
    double timestamp;
    char start_addr[32];
    char end_addr[32];
    char file_path[128];
    int file_offset;
    int size;
} MmapRecord;

// 读取 stat_log 文件，返回记录数
int load_stat_log(const char *filename, StatRecord *records);
// 读取 read_log 文件，返回记录数
int load_read_log(const char *filename, ReadRecord *records);
// 读取 mmap_log 文件，返回记录数
int load_mmap_log(const char *filename, MmapRecord *records);

#endif