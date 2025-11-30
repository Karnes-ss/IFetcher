#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include "profiler_common.h"

// 函数指针：指向 libc 原始的 read() 和 fread()
typedef ssize_t (*read_func_t)(int fd, void* buf, size_t count);
typedef size_t (*fread_func_t)(void* ptr, size_t size, size_t nmemb, FILE* stream);

static read_func_t original_read = NULL;
static fread_func_t original_fread = NULL;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static const char* gate_file = NULL;
static int gate_on = 0;

// 初始化：获取原始函数地址
static void init() {
    // 获取 libc 中的原始 read()
    original_read = (read_func_t)dlsym(RTLD_NEXT, "read");
    if (original_read == NULL) {
        fprintf(stderr, "libwrapper: Failed to get original read(): %s\n", dlerror());
        exit(EXIT_FAILURE);
    }

    // 获取 libc 中的原始 fread()
    original_fread = (fread_func_t)dlsym(RTLD_NEXT, "fread");
    if (original_fread == NULL) {
        fprintf(stderr, "libwrapper: Failed to get original fread(): %s\n", dlerror());
        exit(EXIT_FAILURE);
    }

    // 初始化日志
    profiler_log_init();
    gate_file = getenv("IFETCHER_GATE_FILE");
    if (!gate_file) {
        gate_on = 1; // Default to ON if no gate file specified
    }
}

static inline int logging_enabled() {
    if (gate_on) return 1;
    if (gate_file && access(gate_file, F_OK) == 0) { gate_on = 1; }
    return gate_on;
}

// 拦截 read() 系统调用
ssize_t read(int fd, void* buf, size_t count) {
    pthread_once(&init_once, init);

    // 获取当前文件偏移量
    off_t offset = lseek(fd, 0, SEEK_CUR);
    if (offset == -1) {
        offset = 0; // 无法获取时设为 0
    }

    // 调用原始 read()
    ssize_t ret = original_read(fd, buf, count);

    // 记录日志（仅当读取成功且读取大小 > 0 且已开启网关）
    if (logging_enabled()) {
        ProfilerLogEntry entry = {
            .pid = getpid(),
            .op_type = OP_READ,
            .filename = get_filename_from_fd(fd),
            .offset = offset,
            .size = ret>0?ret:0,
            .fd = fd,
            .timestamp = time(NULL),
            .addr_start = 0,
            .addr_end = 0,
            .file_offset = 0,
            .status = (ret<0)?1:0,
            .err_no = (ret<0)?errno:0
        };
        profiler_log(&entry);
    }

    return ret;
}

// 拦截 fread() 库函数
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    pthread_once(&init_once, init);

    // 从 FILE* 获取文件描述符
    int fd = fileno(stream);
    if (fd == -1) {
        // 无法获取 FD 时直接调用原始函数
        return original_fread(ptr, size, nmemb, stream);
    }

    // 获取当前文件偏移量
    off_t offset = ftell(stream);
    if (offset == -1) {
        offset = 0;
    }

    // 调用原始 fread()
    size_t ret = original_fread(ptr, size, nmemb, stream);
    size_t total_size = ret * size;

    // 记录日志（仅当读取成功且读取大小 > 0 且已开启网关）
    if (logging_enabled()) {
        ProfilerLogEntry entry = {
            .pid = getpid(),
            .op_type = OP_FREAD,
            .filename = get_filename_from_fd(fd),
            .offset = offset,
            .size = total_size,
            .fd = fd,
            .timestamp = time(NULL),
            .addr_start = 0,
            .addr_end = 0,
            .file_offset = 0,
            .status = 0,
            .err_no = 0
        };
        if (ret==0 && ferror(stream)) { entry.status=1; entry.err_no=errno; }
        profiler_log(&entry);
    }

    return ret;
}
