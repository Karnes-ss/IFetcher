// d:\OS_lab\IFecther\prefetcher\src\core\app_config.c
#include "app_config.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_app_from_first_line(const char* log_path, char*** out_argv, int* out_argc, char** out_buf) {
    FILE* fp = fopen(log_path, "r");
    if (fp == NULL) return 0;
    char line[512];
    if (fgets(line, sizeof(line), fp) == NULL) { fclose(fp); return 0; }
    fclose(fp);
    char* p = line;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "APP=", 4) != 0) return 0;
    char* spec = p + 4;
    size_t len = strlen(spec);
    while (len > 0 && (spec[len-1] == '\n' || spec[len-1] == '\r' || spec[len-1] == ' ')) spec[--len] = '\0';
    char* buf = strdup(spec);
    if (!buf) return -1;
    char* scan = strdup(spec);
    if (!scan) { free(buf); return -1; }
    int cnt = 0;
    for (char* t = strtok(scan, " "); t != NULL; t = strtok(NULL, " ")) cnt++;
    free(scan);
    if (cnt == 0) { free(buf); return 0; }
    char** argvv = (char**)malloc((cnt + 1) * sizeof(char*));
    if (!argvv) { free(buf); return -1; }
    int idx = 0;
    for (char* t = strtok(buf, " "); t != NULL; t = strtok(NULL, " ")) argvv[idx++] = t;
    argvv[cnt] = NULL;
    *out_argc = cnt;
    *out_argv = argvv;
    *out_buf = buf;
    return 1;
}

void show_usage(const char* program_name) {
    printf("Usage: %s [options] [--] [application [args...]]\n", program_name);
    printf("Options:\n");
    printf("  -h, --help       Show this help message\n");
    printf("  -a, --app PATH   Specify target application explicitly\n");
    printf("  --               Separator between options and application args\n");
    printf("\nExamples:\n");
    printf("  %s /bin/ls -la\n", program_name);
    printf("  %s --app /usr/bin/firefox -- --new-window\n", program_name);
    printf("\nEnv fallback:\n");
    printf("  APP=/bin/ls APP_ARGS=\"-la\"\n");
    printf("\n");
}

int app_config_build(int argc, char* argv[], AppConfig* out) {
    memset(out, 0, sizeof(*out));
    out->no_spawn = 0;
    int arg_index = 1;
    int app_set = 0;

    while (arg_index < argc) {
        if ((strcmp(argv[arg_index], "-h") == 0) || (strcmp(argv[arg_index], "--help") == 0)) {
            show_usage(argv[0]);
            return 1;
        } else if ((strcmp(argv[arg_index], "--trigger-log") == 0) || (strcmp(argv[arg_index], "--prefetch-log") == 0)) {
            if (arg_index + 1 >= argc) {
                fprintf(stderr, "[APP CONFIG ERROR] %s requires a path argument\n", argv[arg_index]);
                return -1;
            }
            arg_index += 2;
            continue;
        } else if ((strcmp(argv[arg_index], "-a") == 0) || (strcmp(argv[arg_index], "--app") == 0)) {
            if (arg_index + 1 >= argc) {
                fprintf(stderr, "[APP CONFIG ERROR] --app requires a path argument\n");
                return -1;
            }
            out->app_path = argv[arg_index + 1];
            app_set = 1;
            arg_index += 2;
            continue;
        } else if (strcmp(argv[arg_index], "--no-spawn") == 0) {
            out->no_spawn = 1;
            arg_index++;
            continue;
        } else if (strcmp(argv[arg_index], "--") == 0) {
            arg_index++;
            break;
        } else if (argv[arg_index][0] != '-') {
            break;
        }
        arg_index++;
    }

    if (app_set) {
        int extra = (arg_index < argc) ? (argc - arg_index) : 0;
        out->argc = 1 + extra;
        out->argv = (char**)malloc((out->argc + 1) * sizeof(char*));
        if (!out->argv) return -1;
        out->argv[0] = out->app_path;
        for (int i = 0; i < extra; i++) out->argv[1 + i] = argv[arg_index + i];
        out->argv[out->argc] = NULL;
        return 0;
    }

    if (arg_index < argc) {
        out->app_path = argv[arg_index];
        out->argc = argc - arg_index;
        out->argv = (char**)malloc((out->argc + 1) * sizeof(char*));
        if (!out->argv) return -1;
        for (int i = 0; i < out->argc; i++) out->argv[i] = argv[arg_index + i];
        out->argv[out->argc] = NULL;
        return 0;
    }

    char** log_argv = NULL;
    int log_argc = 0;
    char* log_buf = NULL;
    int ok = parse_app_from_first_line(TRIGGER_LOG_PATH, &log_argv, &log_argc, &log_buf);
    if (ok == 0) ok = parse_app_from_first_line(PREFETCH_LOG_PATH, &log_argv, &log_argc, &log_buf);
    if (ok == 1) {
        out->app_path = log_argv[0];
        out->argc = log_argc;
        out->argv = log_argv;
        out->storage_buf = log_buf;
        return 0;
    } else if (ok == -1) {
        fprintf(stderr, "[APP CONFIG ERROR] Failed to parse APP from logs\n");
        return -1;
    }

    const char* env_app = getenv("APP");
    if (env_app && env_app[0] != '\0') {
        out->app_path = (char*)env_app;
        const char* env_args = getenv("APP_ARGS");
        if (env_args && env_args[0] != '\0') {
            char* buf = strdup(env_args);
            char* scan = strdup(env_args);
            if (!buf || !scan) { free(buf); free(scan); return -1; }
            int extra = 0;
            for (char* t = strtok(scan, " "); t != NULL; t = strtok(NULL, " ")) extra++;
            free(scan);
            out->argc = 1 + extra;
            out->argv = (char**)malloc((out->argc + 1) * sizeof(char*));
            if (!out->argv) { free(buf); return -1; }
            out->argv[0] = out->app_path;
            int idx = 1;
            for (char* t = strtok(buf, " "); t != NULL; t = strtok(NULL, " ")) out->argv[idx++] = t;
            out->argv[out->argc] = NULL;
            out->storage_buf = buf;
            return 0;
        } else {
            out->argc = 1;
            out->argv = (char**)malloc(2 * sizeof(char*));
            if (!out->argv) return -1;
            out->argv[0] = out->app_path;
            out->argv[1] = NULL;
            return 0;
        }
    }

    fprintf(stderr, "[APP CONFIG ERROR] No application specified. Use --app or APP/APP_ARGS or put APP=... on first line of logs.\n");
    show_usage(argv[0]);
    return -1;
}

void app_config_free(AppConfig* cfg) {
    if (!cfg) return;
    if (cfg->argv) free(cfg->argv);
    if (cfg->storage_buf) free(cfg->storage_buf);
}