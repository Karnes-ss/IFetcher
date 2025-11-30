// 顶部头文件区域
#include "profiler_common.h"
#include "maps_monitor.h"
#include "diskstats.h"
#include <pthread.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <errno.h>

#define MONITOR_INTERVAL 1

static int verbose() { const char* v = getenv("IFETCHER_VERBOSE"); return (v == NULL || strcmp(v, "0") != 0); }

static pthread_t monitor_thread;
static volatile int monitor_running = 1;
static const char* gate_file = NULL;
static int gate_on = 0;
static double start_ts_env = -1.0;
// 为 main 使用的函数提供前置声明，消除隐式声明警告
int start_proc_monitor(pid_t target_pid);

static int mmap_logging_allowed() {
    if (!gate_on && gate_file && access(gate_file, F_OK) == 0) gate_on = 1;
    if (gate_on) return 1;
    if (start_ts_env > 0) {
        double now = (double)time(NULL);
        if (now >= start_ts_env) return 1;
    }
    return 0;
}

// 监控线程主函数
static void* monitor_thread_func(void* arg) {
    pid_t target_pid = *(pid_t*)arg;
    free(arg);
    profiler_log_init();

    gate_file = getenv("IFETCHER_GATE_FILE");
    const char* sts = getenv("IFETCHER_START_TS");
    if (sts && *sts) start_ts_env = atof(sts);

    int interval_ms = 0; const char* ims = getenv("IFETCHER_MONITOR_INTERVAL_MS"); if (ims && *ims) { interval_ms = atoi(ims); if (interval_ms < 1) interval_ms = 1; }
    if (verbose()) printf("Proc monitor started. Target PID: %d, Interval: %s\n", target_pid, interval_ms>0?"ms":"1s");
    maps_set_start_time(time(NULL));

    // 初始化快照，并在需要时写入当前映射（启动期）
    MmapEntry initial_entries[MAPS_MAX_ENTRY];
    int init_count = maps_init_snapshot(target_pid, initial_entries, MAPS_MAX_ENTRY);
    if (init_count < 0) init_count = 0;
    const char* skip_init = getenv("IFETCHER_SKIP_INIT_SNAPSHOT");
    if (!(skip_init && strcmp(skip_init, "1") == 0)) {
        for (int i = 0; i < init_count; i++) {
            size_t file_size = 0;
            int fd = open(initial_entries[i].filename, O_RDONLY);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0) {
                    file_size = (size_t)st.st_size;
                }
                close(fd);
            }
            ProfilerLogEntry entry = {
                .pid = target_pid,
                .op_type = OP_MMAP,
                .filename = initial_entries[i].filename,
                .offset = 0,
                .size = file_size,
                .fd = -1,
                .timestamp = time(NULL),
                .addr_start = initial_entries[i].start,
                .addr_end = initial_entries[i].end,
                .file_offset = (off_t)initial_entries[i].file_offset,
                .status = 0,
                .err_no = 0
            };
            profiler_log(&entry);
        }
    }

    while (monitor_running) {
        if (kill(target_pid, 0) != 0 && errno != EPERM) {
            break;
        }
        check_mmap_changes(target_pid);
        monitor_disk_stats();
        if (interval_ms > 0) { struct timespec ts; ts.tv_sec = interval_ms / 1000; ts.tv_nsec = (long)((interval_ms % 1000) * 1000000L); nanosleep(&ts, NULL); } else { sleep(MONITOR_INTERVAL); }
    }

    // 退出前统一写入启动窗口内新增的映射（可跳过）
    if (!(skip_init && strcmp(skip_init, "1") == 0)) {
        flush_startup_mmaps(target_pid);
    }

    if (verbose()) printf("Proc monitor stopped\n");
    return NULL;
}

// 停止 proc 监控线程
void stop_proc_monitor() {
    monitor_running = 0;
    pthread_join(monitor_thread, NULL);
}

// 进程是否存在（不改变进程状态）
// 删除不必要的前置声明：
// int start_proc_monitor(pid_t target_pid);
// 主函数（用于独立运行监控）
static pid_t spawn_target(int argc, char* argv[]) {
    /* 以子进程启动目标应用；记录完整命令行为 APP（用于日志首部）；并可通过 LD_PRELOAD 预加载 libwrapper.so 拦截读 */
    size_t len = 0; for (int i = 2; i < argc; i++) len += strlen(argv[i]) + 1; char* buf = (char*)malloc(len + 1); if (buf) { buf[0] = '\0'; for (int i = 2; i < argc; i++) { strcat(buf, argv[i]); if (i + 1 < argc) strcat(buf, " "); } profiler_log_set_app(buf); free(buf); }
    pid_t child = fork();
    if (child == 0) {
        // 可选：启用 read/fread 拦截，使用绝对路径防止 chdir 失效
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            char libpath[1100];
            // 1. Try current directory
            snprintf(libpath, sizeof(libpath), "%s/libwrapper.so", cwd);
            if (access(libpath, F_OK) != 0) {
                // 2. Try profiler subdirectory (common when running from root)
                snprintf(libpath, sizeof(libpath), "%s/profiler/libwrapper.so", cwd);
            }
            
            if (access(libpath, F_OK) == 0) {
                setenv("LD_PRELOAD", libpath, 1);
            } else {
                fprintf(stderr, "[ProcMonitor] Warning: libwrapper.so not found in %s\n", cwd);
            }
        } else {
            setenv("LD_PRELOAD", "./libwrapper.so", 1);
        }
        // 执行目标命令（argv[2] 起为目标命令及其参数）
        execvp(argv[2], &argv[2]);
        perror("execvp failed");
        _exit(127);
    }
    return child;
}

int main(int argc, char* argv[]) {
    if (argc >= 3 && strcmp(argv[1], "--spawn") == 0) {
        pid_t target_pid = spawn_target(argc, argv);
        if (start_proc_monitor(target_pid) != 0) {
            perror("Failed to start proc monitor (spawn)");
            return EXIT_FAILURE;
        }
        printf("Press Enter to stop monitoring...\n");
        getchar();
        stop_proc_monitor();
        return EXIT_SUCCESS;
    }

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <target_pid>\n", argv[0]);
        fprintf(stderr, "       %s --spawn <cmd> [args...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    pid_t target_pid = atoi(argv[1]);
    if (start_proc_monitor(target_pid) != 0) {
        perror("Failed to start proc monitor");
        return EXIT_FAILURE;
    }

    // 等待用户输入退出
    printf("Press Enter to stop monitoring...\n");
    getchar();

    stop_proc_monitor();
    return EXIT_SUCCESS;
}


int start_proc_monitor(pid_t target_pid) {
    pid_t* pid_arg = (pid_t*)malloc(sizeof(pid_t));
    if (pid_arg == NULL) {
        perror("malloc for pid_arg failed");
        return -1;
    }
    *pid_arg = target_pid;
    return pthread_create(&monitor_thread, NULL, monitor_thread_func, pid_arg);
}
