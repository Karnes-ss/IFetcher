// d:\OS_lab\IFecther\prefetcher\include\executor.h
#ifndef EXECUTOR_H
#define EXECUTOR_H
#include "types.h"
#include "app_config.h"
#include <sys/types.h>
pid_t executor_spawn(const PrefetcherConfig* config, const AppConfig* appcfg);
#endif