#ifndef VIRTUAL_BUFFER_H
#define VIRTUAL_BUFFER_H

#include <stddef.h>
#include <stdio.h>

#define VIRTUAL_BUFFER_CHUNK_SIZE 65536u

typedef struct VirtualBuffer VirtualBuffer;

VirtualBuffer *virtual_buffer_create(void);
VirtualBuffer *virtual_buffer_open(const char *path);
VirtualBuffer *virtual_buffer_clone(const VirtualBuffer *buffer);
void virtual_buffer_destroy(VirtualBuffer *buffer);
size_t virtual_buffer_length(const VirtualBuffer *buffer);
int virtual_buffer_read(VirtualBuffer *buffer, size_t offset, void *data, size_t length);
unsigned char virtual_buffer_byte(VirtualBuffer *buffer, size_t offset);
int virtual_buffer_replace(VirtualBuffer *buffer, size_t offset, size_t removed,
    const void *data, size_t length);
int virtual_buffer_replace_stream(VirtualBuffer *buffer, size_t offset, size_t removed,
    FILE *stream, size_t length);
int virtual_buffer_lines(VirtualBuffer *buffer, size_t offset, size_t *count);
int virtual_buffer_known_lines(VirtualBuffer *buffer, size_t *count, int *complete);
int virtual_buffer_line(VirtualBuffer *buffer, size_t line, size_t *start, size_t *end);
int virtual_buffer_line_start(VirtualBuffer *buffer, size_t line, size_t *start);
int virtual_buffer_line_prefix(VirtualBuffer *buffer, size_t line, char *data,
    size_t capacity, size_t *length, int *truncated);
int virtual_buffer_write(VirtualBuffer *buffer, FILE *stream, size_t offset, size_t length);
int virtual_buffer_detach(VirtualBuffer *buffer);
int virtual_buffer_failed(const VirtualBuffer *buffer);

#endif