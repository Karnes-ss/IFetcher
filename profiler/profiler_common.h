#ifndef PROFILER_COMMON_H
#define PROFILER_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

// 日志文件路径（绝对路径）
// 将日志写到 VM 本地文件系统，避免 HGFS/FUSE 的 inotify 与 I/O 问题
#define READ_LOG_FILE "/tmp/read_log"
#define MMAP_LOG_FILE "/tmp/mmap_log"
#define STAT_LOG_FILE "/tmp/stat_log"

// 日志级别
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

// 读取操作类型
typedef enum {
    OP_READ,    // read() 系统调用
    OP_FREAD,   // fread() 库函数
    OP_MMAP     // mmap() 映射读取
} OpType;

// 日志结构体
typedef struct {
    pid_t pid;             // 进程ID
    OpType op_type;        // 操作类型
    const char* filename;  // 文件路径（mmap时有效）
    off_t offset;          // 读取偏移量（read/fread）
    size_t size;           // 读取大小（字节）
    int fd;                // 文件描述符（read/fread时有效）
    time_t timestamp;      // 时间戳
    off_t addr_start;      // mmap: 内存映射起始地址
    off_t addr_end;        // mmap: 内存映射结束地址
    off_t file_offset;     // mmap: 文件偏移（/proc/<pid>/maps 第三列）
} ProfilerLogEntry;

// 日志初始化
void profiler_log_init();

// 写入日志
void profiler_log(ProfilerLogEntry* entry);

// 获取当前时间戳（字符串格式）
const char* get_timestamp();

// 从文件描述符获取文件路径（通过 /proc/[pid]/fd/[fd]）
char* get_filename_from_fd(int fd);

// 新增：写入磁盘采样日志（记录相邻采样的增量：读/写次数、读/写扇区、读/写耗时、总IO耗时；以及瞬时 in_flight）
void profiler_log_diskstat(const char* dev_name,
                           unsigned long long reads_delta,
                           unsigned long long sectors_read_delta,
                           unsigned long long read_time_ms_delta,
                           unsigned long long writes_delta,
                           unsigned long long sectors_written_delta,
                           unsigned long long write_time_ms_delta,
                           unsigned long long io_time_ms_delta,
                           unsigned long long in_flight);

void profiler_log_set_app(const char* cmdline);

#endif // PROFILER_COMMON_H