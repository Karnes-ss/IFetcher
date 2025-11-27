#ifndef LOG_PARSER_H
#define LOG_PARSER_H

#include "types.h"

/**
 * @brief Parse log files and build trigger-prefetch mapping list
 * @param config Global configuration structure (contains log paths and inotify_fd)
 * @return Returns 0 on success, -1 on failure
 */
int log_parser_load(PrefetcherConfig* config);

/**
 * @brief Free mapping list (including list nodes)
 * @param config Global configuration structure (contains mapping list head pointer)
 */
void log_parser_free_map(PrefetcherConfig* config);

#endif // LOG_PARSER_H