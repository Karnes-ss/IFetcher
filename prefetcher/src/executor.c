// d:\OS_lab\IFecther\prefetcher\src\core\executor.c
#include "executor.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

pid_t executor_spawn(const PrefetcherConfig* config, const AppConfig* appcfg) {
    pid_t app_pid = fork();
    if (app_pid == -1) {
        perror("[EXECUTOR ERROR] fork");
        return -1;
    } else if (app_pid == 0) {
        int n = appcfg->argc + 7;
        char** time_argv = (char**)malloc(n * sizeof(char*));
        if (!time_argv) _exit(EXIT_FAILURE);
        time_argv[0] = "/usr/bin/time";
        time_argv[1] = "-v";
        time_argv[2] = "-o";
        time_argv[3] = "time_summary.log";
        time_argv[4] = "/usr/bin/timeout";
        time_argv[5] = "60";
        time_argv[6] = appcfg->app_path;
        for (int i = 1; i < appcfg->argc; i++) time_argv[6 + i] = appcfg->argv[i];
        time_argv[n - 1] = NULL;
        execv(time_argv[0], time_argv);
        perror("[EXECUTOR ERROR] execv app");
        _exit(EXIT_FAILURE);
    } else {
        printf("[MAIN] Created app process (PID: %d)\n", app_pid);
        return app_pid;
    }
}