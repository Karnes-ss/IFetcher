#include "types.h"
#include "list.h"
#include "log_parser.h"
#include "inotify_wrapper.h"
#include "prefetch.h"
#include "app_config.h"
#include "executor.h"
#include "event_loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/inotify.h>

// Configuration parameters
#include "config.h"

// 显示使用帮助

static int verbose() { const char* v = getenv("IFETCHER_VERBOSE"); return (v == NULL || strcmp(v, "0") != 0); }

int main(int argc, char* argv[]) {
    /* 主流程：解析应用 → 初始化 inotify → 加载触发-预取映射 → 可选 spawn 应用 → 事件循环触发预取 */
    if (verbose()) printf("=== Prefetcher System Started ===\n");

    AppConfig appcfg;
    int rc = app_config_build(argc, argv, &appcfg);
    if (rc == 1) {
        return EXIT_SUCCESS;
    }
    if (rc != 0) {
        fprintf(stderr, "[MAIN ERROR] Failed to build app config\n");
        return EXIT_FAILURE;
    }


    const char* cli_trigger = NULL;
    const char* cli_prefetch = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--trigger-log") == 0) { cli_trigger = argv[i + 1]; i++; continue; }
        if (strcmp(argv[i], "--prefetch-log") == 0) { cli_prefetch = argv[i + 1]; i++; continue; }
    }

    const char* env_trigger = getenv("TRIGGER_LOG_PATH");
    const char* env_prefetch = getenv("PREFETCH_LOG_PATH");

    PrefetcherConfig config = {
        .trigger_log_path = cli_trigger ? cli_trigger : (env_trigger ? env_trigger : TRIGGER_LOG_PATH),
        .prefetch_log_path = cli_prefetch ? cli_prefetch : (env_prefetch ? env_prefetch : PREFETCH_LOG_PATH),
        .app_path = appcfg.app_path,
        .inotify_fd = -1,
        .watch_map_head = NULL
    };

    // 2. Initialize inotify
    config.inotify_fd = inotify_init_wrapper();
    if (config.inotify_fd == -1) {
        fprintf(stderr, "[MAIN ERROR] Failed to initialize inotify\n");
        return EXIT_FAILURE;
    }

    // 3. Parse logs and build trigger-prefetch mapping
    if (log_parser_load(&config) != 0) {
        fprintf(stderr, "[MAIN ERROR] Failed to load log files\n");
        close(config.inotify_fd);
        return EXIT_FAILURE;
    }
    if (verbose()) {
        size_t cnt = 0; WatchMap* cur = config.watch_map_head; while (cur) { cnt++; cur = cur->next; }
        printf("[MAIN] Watch map entries: %zu\n", cnt);
    }

    // 4. Optionally create child process to execute target application
    pid_t app_pid = -1;
    if (!appcfg.no_spawn) {
        app_pid = executor_spawn(&config, &appcfg);
        if (app_pid == -1) {
            log_parser_free_map(&config);
            close(config.inotify_fd);
            app_config_free(&appcfg);
            return EXIT_FAILURE;
        }
    }

    event_loop_run(&config, app_pid);

    // 6. Clean up resources
    if (verbose()) printf("\n=== Cleaning up resources ===\n");
    log_parser_free_map(&config);
    close(config.inotify_fd);
    app_config_free(&appcfg);

    if (verbose()) printf("=== Prefetcher System Exited ===\n");
    return EXIT_SUCCESS;
}
