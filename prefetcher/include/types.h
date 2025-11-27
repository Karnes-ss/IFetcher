#ifndef TYPES_H
#define TYPES_H

#include <stddef.h>
#include <sys/types.h>

// Prefetch file linked list node
typedef struct FileNode {
    char* path;                // File path (absolute/relative)
    off_t offset;              // Suggested prefetch start offset
    size_t length;             // Suggested prefetch length
    struct FileNode* next;     // Next node pointer
} FileNode;

// Trigger file-prefetch list mapping item (links inotify WD with prefetch list)
typedef struct WatchMap {
    int wd;                    // inotify watch descriptor (unique identifier)
    FileNode* prefetch_list;   // Corresponding prefetch file list
    struct WatchMap* next;     // Next mapping item pointer
} WatchMap;

// Global configuration structure (stores core system configuration)
typedef struct PrefetcherConfig {
    const char* trigger_log_path;  // Path to trigger_log
    const char* prefetch_log_path; // Path to prefetch_log
    const char* app_path;          // Target application path
    int inotify_fd;                // inotify instance file descriptor
    WatchMap* watch_map_head;      // Trigger-prefetch mapping list head pointer
} PrefetcherConfig;

#endif // TYPES_H