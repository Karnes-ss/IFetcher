#include "profiler_common.h"
#include <pthread.h>

static FILE* read_log_file = NULL;
static FILE* mmap_log_file = NULL;
static FILE* stat_log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char* app_cmdline = NULL;
void profiler_log_set_app(const char* cmd){ if (cmd) { app_cmdline = strdup(cmd);} }

// 初始化日志文件（分别打开三个）
void profiler_log_init() {
    if (read_log_file == NULL) {
        read_log_file = fopen(READ_LOG_FILE, "a");
        if (read_log_file == NULL) {
            perror("Failed to open read_log");
            exit(EXIT_FAILURE);
        }
        fprintf(read_log_file, "===== READ/FREAD Log Started at %s =====\n", get_timestamp());
        fflush(read_log_file);
    }
    if (mmap_log_file == NULL) {
        mmap_log_file = fopen(MMAP_LOG_FILE, "a");
        if (mmap_log_file == NULL) {
            perror("Failed to open mmap_log");
            exit(EXIT_FAILURE);
        }
        fprintf(mmap_log_file, "===== MMAP Log Started at %s =====\n", get_timestamp());
        fflush(mmap_log_file);
    }
    if (stat_log_file == NULL) {
        stat_log_file = fopen(STAT_LOG_FILE, "a");
        if (stat_log_file == NULL) {
            perror("Failed to open stat_log");
            exit(EXIT_FAILURE);
        }
        fprintf(stat_log_file, "===== Disk Stats Log Started at %s =====\n", get_timestamp());
        fflush(stat_log_file);
    }
    if (app_cmdline) {
        /* 将被监控应用完整命令行写入三份 /tmp 日志的首部，便于后续 analyzer/预取器自动识别 APP */
        fprintf(read_log_file, "APP=%s\n", app_cmdline);
        fflush(read_log_file);
        fprintf(mmap_log_file, "APP=%s\n", app_cmdline);
        fflush(mmap_log_file);
        fprintf(stat_log_file, "APP=%s\n", app_cmdline);
        fflush(stat_log_file);
    }
}

// 写入读取/映射日志，按类型选择文件
void profiler_log(ProfilerLogEntry* entry) {
    profiler_log_init();
    pthread_mutex_lock(&log_mutex);

    const char* op_type_str[] = {"READ", "FREAD", "MMAP"};
    FILE* target = (entry->op_type == OP_MMAP) ? mmap_log_file : read_log_file;

    fprintf(target, "[%s] PID:%d | Type:%s | ",
            get_timestamp(), entry->pid, op_type_str[entry->op_type]);

    if (entry->op_type == OP_MMAP) {
        fprintf(target, "File:%s | AddrStart:%lld | AddrEnd:%lld | FileOffset:%lld | Size:%zu\n",
                entry->filename,
                (long long)entry->addr_start,
                (long long)entry->addr_end,
                (long long)entry->file_offset,
                (size_t)entry->size);
    } else {
        fprintf(target, "FD:%d | File:%s | Offset:%lld | Size:%zu\n",
                entry->fd, entry->filename, (long long)entry->offset, (size_t)entry->size);
    }
    fflush(target);
    pthread_mutex_unlock(&log_mutex);
}

// 新增：写入磁盘采样日志
void profiler_log_diskstat(const char* dev_name,
                           unsigned long long reads_delta,
                           unsigned long long sectors_read_delta,
                           unsigned long long read_time_ms_delta,
                           unsigned long long writes_delta,
                           unsigned long long sectors_written_delta,
                           unsigned long long write_time_ms_delta,
                           unsigned long long io_time_ms_delta,
                           unsigned long long in_flight) {
    profiler_log_init();
    pthread_mutex_lock(&log_mutex);

    fprintf(stat_log_file,
            "[%s] Device:%s | reads:%llu | sectors_read:%llu | read_time_ms:%llu | "
            "writes:%llu | sectors_written:%llu | write_time_ms:%llu | "
            "io_time_ms:%llu | in_flight:%llu\n",
            get_timestamp(), dev_name,
            reads_delta, sectors_read_delta, read_time_ms_delta,
            writes_delta, sectors_written_delta, write_time_ms_delta,
            io_time_ms_delta, in_flight);

    fflush(stat_log_file);
    pthread_mutex_unlock(&log_mutex);
}

// 获取当前时间戳字符串
const char* get_timestamp() {
    static char buf[64];
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}

// 从文件描述符获取文件路径
char* get_filename_from_fd(int fd) {
    // 使用线程本地缓冲，避免多线程覆盖同一个静态数组
    static __thread char filename[256];

    // 优先走 /proc/self/fd/<fd>，无需依赖 pid
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);

    ssize_t len = readlink(proc_path, filename, sizeof(filename) - 1);
    if (len == -1) {
        // readlink 失败：可能是 FD 非文件、已关闭、权限/沙箱限制等
        // 用 fstat 识别 FD 类型，给出更有意义的名字
        struct stat st;
        if (fstat(fd, &st) == 0) {
            if (S_ISREG(st.st_mode)) {
                strcpy(filename, "regular-file");
            } else if (S_ISDIR(st.st_mode)) {
                strcpy(filename, "directory");
            } else if (S_ISCHR(st.st_mode)) {
                strcpy(filename, "char-device");
            } else if (S_ISBLK(st.st_mode)) {
                strcpy(filename, "block-device");
            } else if (S_ISFIFO(st.st_mode)) {
                strcpy(filename, "pipe");
            } else if (S_ISSOCK(st.st_mode)) {
                strcpy(filename, "socket");
            } else {
                strcpy(filename, "unknown");
            }
        } else {
            // FD 已失效或不可访问
            strcpy(filename, "unknown");
        }
        return filename;
    }

    filename[len] = '\0';
    return filename;
}