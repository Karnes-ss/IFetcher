#include "list.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

FileNode* list_create_node(const char* path) {
    if (path == NULL || strlen(path) == 0) {
        fprintf(stderr, "[LIST ERROR] Empty file path\n");
        return NULL;
    }

    FileNode* node = (FileNode*)malloc(sizeof(FileNode));
    if (node == NULL) {
        perror("[LIST ERROR] malloc node");
        return NULL;
    }

    node->path = strdup(path);
    if (node->path == NULL) {
        perror("[LIST ERROR] strdup path");
        free(node);
        return NULL;
    }

    node->offset = 0;
    node->length = 0;
    node->next = NULL;
    return node;
}

int list_add_node(FileNode** head, const char* path) {
    if (head == NULL) {
        fprintf(stderr, "[LIST ERROR] Invalid head pointer\n");
        return -1;
    }

    FileNode* new_node = list_create_node(path);
    if (new_node == NULL) {
        return -1;
    }

    if (*head == NULL) {
        *head = new_node; // List is empty, new node becomes head
        return 0;
    }

    // Traverse to the end of the list
    FileNode* current = *head;
    while (current->next != NULL) {
        current = current->next;
    }
    current->next = new_node;
    return 0;
}

size_t list_get_length(const FileNode* head) {
    size_t len = 0;
    const FileNode* current = head;
    while (current != NULL) {
        len++;
        current = current->next;
    }
    return len;
}

void list_free(FileNode* head) {
    FileNode* temp = NULL;
    while (head != NULL) {
        temp = head;
        head = head->next;
        free(temp->path);
        free(temp);
    }
}

FileNode* list_clone(const FileNode* head) {
    FileNode* copy = NULL;
    const FileNode* curr = head;
    while (curr != NULL) {
        FileNode* new_node = list_create_node(curr->path);
        if (!new_node) {
            list_free(copy);
            return NULL;
        }
        new_node->offset = curr->offset;
        new_node->length = curr->length;
        if (copy == NULL) {
            copy = new_node;
        } else {
            FileNode* tail = copy;
            while (tail->next) tail = tail->next;
            tail->next = new_node;
        }
        curr = curr->next;
    }
    return copy;
}

int list_add_node_ex(FileNode** head, const char* path, off_t offset, size_t length) {
    if (head == NULL) {
        fprintf(stderr, "[LIST ERROR] Invalid head pointer\n");
        return -1;
    }
    FileNode* new_node = list_create_node(path);
    if (new_node == NULL) return -1;
    new_node->offset = offset;
    new_node->length = length;
    if (*head == NULL) { *head = new_node; return 0; }
    FileNode* current = *head;
    while (current->next != NULL) current = current->next;
    current->next = new_node;
    return 0;
}