#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "virtual_buffer.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FILE_CHUNK VIRTUAL_BUFFER_CHUNK_SIZE

typedef struct FileSource {
    FILE *file;
    size_t references;
    size_t length;
    size_t *lines;
    size_t indexed;
    size_t index_capacity;
    size_t inspected;
    size_t inspected_lines;
    int temporary;
    unsigned char *cache;
    size_t cache_offset;
    size_t cache_length;
} FileSource;

typedef struct FilePiece {
    FileSource *source;
    size_t offset;
    size_t length;
} FilePiece;

struct VirtualBuffer {
    FilePiece *pieces;
    size_t count;
    size_t length;
    FileSource *changes;
    int failed;
};

static int seek_file(FILE *file, size_t offset)
{
#ifdef _MSC_VER
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return offset <= LONG_MAX && fseek(file, (long)offset, SEEK_SET) == 0;
#endif
}

static void release_source(FileSource *source)
{
    if (source && !--source->references) {
        fclose(source->file);
        free(source->lines);
        free(source->cache);
        free(source);
    }
}

static FileSource *create_source(FILE *file, size_t length)
{
    FileSource *source = calloc(1, sizeof(*source));
    if (!source)
        return NULL;
    source->file = file;
    source->length = length;
    source->references = 1;
    return source;
}

static int source_cache(FileSource *source, size_t offset)
{
    if (offset >= source->length)
        return 0;
    if (source->cache_length && offset >= source->cache_offset &&
        offset - source->cache_offset < source->cache_length)
        return 1;
    if (!source->cache) {
        source->cache = malloc(FILE_CHUNK);
        if (!source->cache)
            return 0;
    }
    source->cache_length = 0;
    size_t start = offset / FILE_CHUNK * FILE_CHUNK;
    size_t length = source->length - start;
    if (length > FILE_CHUNK)
        length = FILE_CHUNK;
    if (!seek_file(source->file, start) || fread(source->cache, 1, length, source->file) != length)
        return 0;
    source->cache_offset = start;
    source->cache_length = length;
    return 1;
}

static int source_read(FileSource *source, size_t offset, void *data, size_t length)
{
    unsigned char *output = data;
    if (offset > source->length || length > source->length - offset)
        return 0;
    while (length) {
        if (!source_cache(source, offset))
            return 0;
        size_t local = offset - source->cache_offset;
        size_t count = source->cache_length - local;
        if (count > length)
            count = length;
        memcpy(output, source->cache + local, count);
        output += count;
        offset += count;
        length -= count;
    }
    return 1;
}

static size_t count_newlines(const unsigned char *text, size_t length)
{
    size_t count = 0;
    while (length) {
        const unsigned char *newline = memchr(text, '\n', length);
        if (!newline)
            break;
        size_t consumed = (size_t)(newline - text) + 1;
        length -= consumed;
        text += consumed;
        ++count;
    }
    return count;
}

static int source_lines(FileSource *source, size_t offset, size_t *count)
{
    if (offset > source->length)
        return 0;
    size_t blocks = offset / FILE_CHUNK;
    if (blocks + 1 > source->index_capacity) {
        size_t step = FILE_CHUNK / sizeof(*source->lines);
        size_t capacity = (blocks / step + 1) * step;
        if (capacity > SIZE_MAX / sizeof(*source->lines))
            return 0;
        size_t *lines = realloc(source->lines, capacity * sizeof(*lines));
        if (!lines)
            return 0;
        source->lines = lines;
        source->index_capacity = capacity;
        source->lines[0] = 0;
    }
    while (source->indexed < blocks) {
        size_t position = source->indexed * FILE_CHUNK;
        if (!source_cache(source, position) || source->cache_length < FILE_CHUNK)
            return 0;
        source->lines[source->indexed + 1] = source->lines[source->indexed] +
            count_newlines(source->cache, FILE_CHUNK);
        ++source->indexed;
    }
    *count = source->lines[blocks];
    size_t tail = offset % FILE_CHUNK;
    if (tail) {
        if (!source_cache(source, blocks * FILE_CHUNK))
            return 0;
        *count += count_newlines(source->cache, tail);
    }
    if (offset >= source->inspected) {
        source->inspected = offset;
        source->inspected_lines = *count;
    }
    return 1;
}

VirtualBuffer *virtual_buffer_create(void)
{
    return calloc(1, sizeof(VirtualBuffer));
}

VirtualBuffer *virtual_buffer_open(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
#ifdef _MSC_VER
    int positioned = _fseeki64(file, 0, SEEK_END) == 0;
    __int64 size = positioned ? _ftelli64(file) : -1;
#else
    int positioned = fseek(file, 0, SEEK_END) == 0;
    long size = positioned ? ftell(file) : -1;
#endif
    if (size < 0 || (uintmax_t)size >= SIZE_MAX) {
        fclose(file);
        return NULL;
    }
    VirtualBuffer *buffer = calloc(1, sizeof(*buffer));
    FileSource *source = create_source(file, (size_t)size);
    FilePiece *piece = malloc(sizeof(*piece));
    if (!buffer || !source || !piece) {
        if (source)
            release_source(source);
        else
            fclose(file);
        free(piece);
        free(buffer);
        return NULL;
    }
    piece->source = source;
    piece->offset = 0;
    piece->length = (size_t)size;
    buffer->pieces = piece;
    buffer->count = 1;
    buffer->length = (size_t)size;
    return buffer;
}

void virtual_buffer_destroy(VirtualBuffer *buffer)
{
    if (!buffer)
        return;
    for (size_t index = 0; index < buffer->count; ++index)
        release_source(buffer->pieces[index].source);
    release_source(buffer->changes);
    free(buffer->pieces);
    free(buffer);
}

VirtualBuffer *virtual_buffer_clone(const VirtualBuffer *buffer)
{
    VirtualBuffer *copy = calloc(1, sizeof(*copy));
    if (!copy)
        return NULL;
    copy->pieces = malloc((buffer->count ? buffer->count : 1) * sizeof(*copy->pieces));
    if (!copy->pieces) {
        free(copy);
        return NULL;
    }
    if (buffer->count)
        memcpy(copy->pieces, buffer->pieces, buffer->count * sizeof(*copy->pieces));
    copy->count = buffer->count;
    copy->length = buffer->length;
    copy->changes = buffer->changes;
    if (copy->changes)
        ++copy->changes->references;
    for (size_t index = 0; index < copy->count; ++index)
        ++copy->pieces[index].source->references;
    return copy;
}

size_t virtual_buffer_length(const VirtualBuffer *buffer)
{
    return buffer->length;
}

int virtual_buffer_read(VirtualBuffer *buffer, size_t offset, void *data, size_t length)
{
    if (offset > buffer->length || length > buffer->length - offset || (length && !data))
        return 0;
    unsigned char *output = data;
    for (size_t index = 0; index < buffer->count && length; ++index) {
        FilePiece piece = buffer->pieces[index];
        if (offset >= piece.length) {
            offset -= piece.length;
            continue;
        }
        size_t count = piece.length - offset;
        if (count > length)
            count = length;
        if (!source_read(piece.source, piece.offset + offset, output, count)) {
            buffer->failed = 1;
            return 0;
        }
        output += count;
        length -= count;
        offset = 0;
    }
    return length == 0;
}

unsigned char virtual_buffer_byte(VirtualBuffer *buffer, size_t offset)
{
    unsigned char byte = 0;
    virtual_buffer_read(buffer, offset, &byte, 1);
    return byte;
}

static void append_piece(FilePiece *pieces, size_t *count, FileSource *source,
    size_t offset, size_t length)
{
    if (!length)
        return;
    if (*count && pieces[*count - 1].source == source &&
        pieces[*count - 1].offset + pieces[*count - 1].length == offset) {
        pieces[*count - 1].length += length;
        return;
    }
    pieces[*count].source = source;
    pieces[*count].offset = offset;
    pieces[*count].length = length;
    ++source->references;
    ++*count;
}

static int replace(VirtualBuffer *buffer, size_t offset, size_t removed,
    const void *data, FILE *stream, size_t length)
{
    if (buffer->failed || offset > buffer->length || removed > buffer->length - offset ||
        length >= SIZE_MAX - (buffer->length - removed) || buffer->count > SIZE_MAX / sizeof(FilePiece) - 3)
        return 0;
    FilePiece *pieces = malloc((buffer->count + 3) * sizeof(*pieces));
    if (!pieces)
        return 0;
    if (length && !buffer->changes) {
        FILE *file = tmpfile();
        if (file) {
            buffer->changes = create_source(file, 0);
            if (buffer->changes)
                buffer->changes->temporary = 1;
            else
                fclose(file);
        }
    }
    size_t added = buffer->changes ? buffer->changes->length : 0;
    if (length) {
        FileSource *changes = buffer->changes;
        if (!changes || length > SIZE_MAX - added || !seek_file(changes->file, added)) {
            free(pieces);
            return 0;
        }
        size_t left = length;
        unsigned char chunk[FILE_CHUNK];
        const unsigned char *input = data;
        while (left) {
            size_t count = left > FILE_CHUNK ? FILE_CHUNK : left;
            if (stream) {
                if (fread(chunk, 1, count, stream) != count)
                    break;
                input = chunk;
            }
            if (!input || fwrite(input, 1, count, changes->file) != count)
                break;
            if (!stream)
                input += count;
            left -= count;
        }
        if (left || fflush(changes->file)) {
            clearerr(changes->file);
            free(pieces);
            return 0;
        }
        changes->length += length;
        changes->cache_length = 0;
    }
    size_t count = 0;
    size_t position = 0;
    int inserted = 0;
    for (size_t index = 0; index < buffer->count; ++index) {
        FilePiece piece = buffer->pieces[index];
        size_t end = position + piece.length;
        if (position < offset) {
            size_t prefix = end <= offset ? piece.length : offset - position;
            append_piece(pieces, &count, piece.source, piece.offset, prefix);
        }
        if (!inserted && end >= offset) {
            append_piece(pieces, &count, buffer->changes, added, length);
            inserted = 1;
        }
        if (end > offset + removed) {
            size_t skipped = position >= offset + removed ? 0 : offset + removed - position;
            append_piece(pieces, &count, piece.source, piece.offset + skipped, piece.length - skipped);
        }
        position = end;
    }
    if (!inserted)
        append_piece(pieces, &count, buffer->changes, added, length);
    for (size_t index = 0; index < buffer->count; ++index)
        release_source(buffer->pieces[index].source);
    free(buffer->pieces);
    buffer->pieces = pieces;
    buffer->count = count;
    buffer->length = buffer->length - removed + length;
    return 1;
}

int virtual_buffer_replace(VirtualBuffer *buffer, size_t offset, size_t removed,
    const void *data, size_t length)
{
    return replace(buffer, offset, removed, data, NULL, length);
}

int virtual_buffer_replace_stream(VirtualBuffer *buffer, size_t offset, size_t removed,
    FILE *stream, size_t length)
{
    return stream && replace(buffer, offset, removed, NULL, stream, length);
}

int virtual_buffer_lines(VirtualBuffer *buffer, size_t offset, size_t *count)
{
    if (offset > buffer->length)
        return 0;
    *count = 0;
    for (size_t index = 0; index < buffer->count && offset; ++index) {
        FilePiece piece = buffer->pieces[index];
        size_t length = offset < piece.length ? offset : piece.length;
        size_t first, last;
        if (!source_lines(piece.source, piece.offset, &first) ||
            !source_lines(piece.source, piece.offset + length, &last)) {
            buffer->failed = 1;
            return 0;
        }
        *count += last - first;
        offset -= length;
    }
    return 1;
}

int virtual_buffer_known_lines(VirtualBuffer *buffer, size_t *count, int *complete)
{
    *count = 0;
    *complete = 1;
    for (size_t index = 0; index < buffer->count; ++index) {
        FilePiece piece = buffer->pieces[index];
        size_t available = piece.source->inspected > piece.offset ?
            piece.source->inspected - piece.offset : 0;
        if (available > piece.length)
            available = piece.length;
        size_t first = 0, last = 0;
        if (available && (!source_lines(piece.source, piece.offset, &first) ||
            !source_lines(piece.source, piece.offset + available, &last)))
            return 0;
        *count += last - first;
        if (available != piece.length) {
            *complete = 0;
            break;
        }
    }
    return 1;
}

static int newline_offset(VirtualBuffer *buffer, size_t number, size_t *offset)
{
    size_t logical = 0;
    for (size_t index = 0; index < buffer->count; ++index) {
        FilePiece piece = buffer->pieces[index];
        size_t position = piece.offset;
        size_t limit = position + piece.length;
        size_t before;
        if (!source_lines(piece.source, position, &before))
            goto failure;
        while (position < limit) {
            size_t count = FILE_CHUNK - position % FILE_CHUNK;
            if (count > limit - position)
                count = limit - position;
            size_t after;
            if (!source_lines(piece.source, position + count, &after))
                goto failure;
            if (after - before >= number) {
                if (!source_cache(piece.source, position))
                    goto failure;
                const unsigned char *text = piece.source->cache + position - piece.source->cache_offset;
                size_t consumed = 0;
                while (number--) {
                    const unsigned char *newline = memchr(text + consumed, '\n', count - consumed);
                    if (!newline)
                        goto failure;
                    consumed = (size_t)(newline - text) + 1;
                }
                *offset = logical + position - piece.offset + consumed - 1;
                return 1;
            }
            number -= after - before;
            before = after;
            position += count;
        }
        logical += piece.length;
    }
    return 0;
failure:
    buffer->failed = 1;
    return 0;
}

int virtual_buffer_line_start(VirtualBuffer *buffer, size_t line, size_t *start)
{
    *start = 0;
    if (line) {
        if (!newline_offset(buffer, line, start))
            return 0;
        ++*start;
    }
    return !buffer->failed;
}

int virtual_buffer_line_prefix(VirtualBuffer *buffer, size_t line, char *data,
    size_t capacity, size_t *length, int *truncated)
{
    size_t start;
    if (!virtual_buffer_line_start(buffer, line, &start))
        return 0;
    size_t count = buffer->length - start;
    if (count > capacity)
        count = capacity;
    if (!virtual_buffer_read(buffer, start, data, count))
        return 0;
    const char *newline = memchr(data, '\n', count);
    *length = newline ? (size_t)(newline - data) : count;
    *truncated = !newline && start + count < buffer->length &&
        virtual_buffer_byte(buffer, start + count) != '\n';
    return 1;
}

int virtual_buffer_line(VirtualBuffer *buffer, size_t line, size_t *start, size_t *end)
{
    if (!virtual_buffer_line_start(buffer, line, start))
        return 0;
    if (!newline_offset(buffer, line + 1, end))
        *end = buffer->length;
    return !buffer->failed;
}

int virtual_buffer_write(VirtualBuffer *buffer, FILE *stream, size_t offset, size_t length)
{
    unsigned char chunk[FILE_CHUNK];
    while (length) {
        size_t count = length > sizeof(chunk) ? sizeof(chunk) : length;
        if (!virtual_buffer_read(buffer, offset, chunk, count) || fwrite(chunk, 1, count, stream) != count)
            return 0;
        offset += count;
        length -= count;
    }
    return 1;
}

int virtual_buffer_detach(VirtualBuffer *buffer)
{
    for (size_t index = 0; index < buffer->count; ++index) {
        FileSource *source = buffer->pieces[index].source;
        if (source->temporary)
            continue;
        FILE *copy = tmpfile();
        if (!copy)
            return 0;
        unsigned char chunk[FILE_CHUNK];
        size_t offset = 0;
        int success = 1;
        while (offset < source->length && success) {
            size_t count = source->length - offset;
            if (count > sizeof(chunk))
                count = sizeof(chunk);
            success = source_read(source, offset, chunk, count) && fwrite(chunk, 1, count, copy) == count;
            offset += count;
        }
        if (fflush(copy))
            success = 0;
        if (!success) {
            fclose(copy);
            return 0;
        }
        fclose(source->file);
        source->file = copy;
        source->temporary = 1;
    }
    return 1;
}

int virtual_buffer_failed(const VirtualBuffer *buffer)
{
    return buffer->failed;
}