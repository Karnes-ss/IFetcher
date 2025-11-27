// d:\OS_lab\IFecther\prefetcher\include\event_loop.h
#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H
#include "types.h"
#include <sys/types.h>
int event_loop_run(PrefetcherConfig* config, pid_t app_pid);
#endif