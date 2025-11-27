#ifndef PREFETCH_H
#define PREFETCH_H

#include "types.h"
#include <pthread.h>

/**
 * @brief Prefetch thread function (for pthread_create to call)
 * @param arg Prefetch file list head pointer (FileNode*)
 * @return No return value (pthread_exit(NULL))
 */
void* prefetch_thread(void* arg);

/**
 * @brief Create prefetch thread
 * @param prefetch_list Prefetch file list head pointer
 * @return Returns 0 on success, -1 on failure
 */
int prefetch_create_thread(FileNode* prefetch_list);

#endif // PREFETCH_H