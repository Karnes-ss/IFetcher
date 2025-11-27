// d:\OS_lab\IFecther\prefetcher\include\app_config.h
#ifndef APP_CONFIG_H
#define APP_CONFIG_H
#include "types.h"
typedef struct AppConfig {
    char* app_path;
    char** argv;
    int argc;
    int no_spawn;
    char* storage_buf;
} AppConfig;
void show_usage(const char* program_name);
int app_config_build(int argc, char* argv[], AppConfig* out);
void app_config_free(AppConfig* cfg);
#endif