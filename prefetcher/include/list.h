#ifndef LIST_H
#define LIST_H

#include "types.h"

/**
 * @brief Create a single file node
 * @param path File path
 * @return Returns node pointer on success, NULL on failure
 */
FileNode* list_create_node(const char* path);

/**
 * @brief Add a file node to the end of the linked list
 * @param head List head pointer (modifiable, needs pointer to pointer)
 * @param path File path to add
 * @return Returns 0 on success, -1 on failure
 */
int list_add_node(FileNode** head, const char* path);
int list_add_node_ex(FileNode** head, const char* path, off_t offset, size_t length);

/**
 * @brief Get the length of the linked list
 * @param head List head pointer
 * @return Number of nodes in the list
 */
size_t list_get_length(const FileNode* head);

/**
 * @brief Free all nodes in the list (including path memory)
 * @param head List head pointer
 */
void list_free(FileNode* head);

FileNode* list_clone(const FileNode* head);

#endif // LIST_H