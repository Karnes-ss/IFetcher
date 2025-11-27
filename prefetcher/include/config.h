// d:\OS_lab\IFecther\prefetcher\include\config.h
#ifndef CONFIG_H
#define CONFIG_H
#include <sys/inotify.h>
#define TRIGGER_LOG_PATH "../analyzer/trigger_log.txt"
#define PREFETCH_LOG_PATH "../analyzer/prefetch_log.txt"
#define DEFAULT_APP_PATH "./app/test_app"
#define MAX_EVENTS 1024
#define EVENT_BUF_SIZE (MAX_EVENTS * sizeof(struct inotify_event))
#endif