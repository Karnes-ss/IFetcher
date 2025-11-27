#ifndef INOTIFY_WRAPPER_H
#define INOTIFY_WRAPPER_H

#include <sys/inotify.h>
#include <stddef.h>
#include <unistd.h>

/**
 * @brief Initialize inotify instance
 * @return Returns inotify file descriptor on success, -1 on failure
 */
int inotify_init_wrapper(void);

/**
 * @brief Add file monitoring to inotify (monitor access events IN_ACCESS)
 * @param inotify_fd inotify instance file descriptor
 * @param path Path of the file to monitor
 * @return Returns WD (watch descriptor) on success, -1 on failure
 */
int inotify_add_watch_wrapper(int inotify_fd, const char* path);

/**
 * @brief Remove inotify file monitoring
 * @param inotify_fd inotify instance file descriptor
 * @param wd Watch descriptor
 * @return Returns 0 on success, -1 on failure
 */
int inotify_rm_watch_wrapper(int inotify_fd, int wd);

/**
 * @brief Read inotify events (blocking mode)
 * @param inotify_fd inotify instance file descriptor
 * @param buf Event buffer
 * @param buf_size Buffer size
 * @return Returns number of bytes read on success, -1 on failure
 */
ssize_t inotify_read_events(int inotify_fd, char* buf, size_t buf_size);

#endif // INOTIFY_WRAPPER_H