#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "editor_core.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ReplaceEdit {
    size_t position;
    size_t original_length;
    size_t inserted_length;
    char *original;
} ReplaceEdit;

typedef struct HistoryEntry {
    char *text;
    size_t length;
    size_t cursor;
    size_t revision;
    struct HistoryEntry *next;
} HistoryEntry;

struct EditorCore {
    char *text;
    size_t length;
    size_t capacity;
    size_t cursor;
    EditorMode mode;
    EditorStatus status;
    EditorSettings settings;
    size_t anchor;
    size_t count;
    size_t operator_count;
    int operator_has_count;
    unsigned long pending;
    char *register_text;
    size_t register_length;
    int register_linewise;
    int modified;
    size_t block_anchor_column;
    size_t block_column;
    size_t block_first;
    size_t block_last;
    size_t block_insert_column;
    size_t block_insert_start;
    int block_insert;
    ReplaceEdit *replace_edits;
    size_t replace_count;
    size_t replace_capacity;
    HistoryEntry *undo;
    HistoryEntry *redo;
    size_t revision;
    size_t next_revision;
    size_t saved_revision;
    size_t revision_cursor;
    unsigned int dispatch_depth;
    int history_enabled;
    int history_open;
    int history_changed;
};

static void clear_replace_history(EditorCore *core)
{
    while (core->replace_count)
        free(core->replace_edits[--core->replace_count].original);
}

static void free_history(HistoryEntry *entry)
{
    while (entry) {
        HistoryEntry *next = entry->next;
        free(entry->text);
        free(entry);
        entry = next;
    }
}

static int prepare_edit(EditorCore *core)
{
    if (!core->history_enabled)
        return 1;
    if (!core->history_open) {
        if (core->next_revision == SIZE_MAX)
            return 0;
        HistoryEntry *entry = malloc(sizeof(*entry));
        if (!entry)
            return 0;
        entry->text = malloc(core->length + 1);
        if (!entry->text) {
            free(entry);
            return 0;
        }
        memcpy(entry->text, core->text, core->length + 1);
        entry->length = core->length;
        entry->cursor = core->cursor;
        entry->revision = core->revision;
        entry->next = core->undo;
        core->undo = entry;
        free_history(core->redo);
        core->redo = NULL;
        core->revision = ++core->next_revision;
        core->history_open = 1;
    }
    core->history_changed = 1;
    return 1;
}

static size_t encode_character(unsigned long value, char text[5])
{
    if (!value || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        return 0;
    if (value < 0x80) {
        text[0] = (char)value;
        text[1] = 0;
        return 1;
    }
    if (value < 0x800) {
        text[0] = (char)(0xC0 | (value >> 6));
        text[1] = (char)(0x80 | (value & 0x3F));
        text[2] = 0;
        return 2;
    }
    if (value < 0x10000) {
        text[0] = (char)(0xE0 | (value >> 12));
        text[1] = (char)(0x80 | ((value >> 6) & 0x3F));
        text[2] = (char)(0x80 | (value & 0x3F));
        text[3] = 0;
        return 3;
    }
    text[0] = (char)(0xF0 | (value >> 18));
    text[1] = (char)(0x80 | ((value >> 12) & 0x3F));
    text[2] = (char)(0x80 | ((value >> 6) & 0x3F));
    text[3] = (char)(0x80 | (value & 0x3F));
    text[4] = 0;
    return 4;
}

static size_t decode_character(const char *text, size_t length, unsigned long *value)
{
    if (!length)
        return 0;
    unsigned char first = (unsigned char)text[0];
    size_t count;
    unsigned long minimum;
    if (first < 0x80) {
        *value = first;
        return 1;
    } else if (first >= 0xC2 && first <= 0xDF) {
        count = 2;
        minimum = 0x80;
        *value = first & 0x1F;
    } else if (first >= 0xE0 && first <= 0xEF) {
        count = 3;
        minimum = 0x800;
        *value = first & 0x0F;
    } else if (first >= 0xF0 && first <= 0xF4) {
        count = 4;
        minimum = 0x10000;
        *value = first & 0x07;
    } else {
        return 0;
    }
    if (length < count)
        return 0;
    for (size_t index = 1; index < count; ++index) {
        unsigned char byte = (unsigned char)text[index];
        if ((byte & 0xC0) != 0x80)
            return 0;
        *value = (*value << 6) | (byte & 0x3F);
    }
    if (*value < minimum || *value > 0x10FFFF || (*value >= 0xD800 && *value <= 0xDFFF))
        return 0;
    return count;
}

static int reserve_text(EditorCore *core, size_t length)
{
    size_t limit = SIZE_MAX;
    if (length >= limit)
        return 0;
    size_t capacity = length + 1;
    if (capacity <= core->capacity)
        return 1;
    if (capacity <= limit / 2)
        capacity *= 2;
    char *replacement = realloc(core->text, capacity);
    if (!replacement)
        return 0;
    core->text = replacement;
    core->capacity = capacity;
    return 1;
}

static int insert_text(EditorCore *core, const char *text)
{
    if (!text)
        return 0;
    size_t length = strlen(text);
    if (length > SIZE_MAX - core->length)
        return 0;
    char *normalized = malloc(length + 1);
    if (!normalized)
        return 0;
    size_t count = 0;
    for (size_t index = 0; index < length;) {
        unsigned long value;
        size_t width = decode_character(text + index, length - index, &value);
        if (!width) {
            free(normalized);
            return 0;
        }
        if (value == '\r') {
            normalized[count++] = '\n';
            index += text[index + 1] == '\n' ? 2 : 1;
        } else {
            memcpy(normalized + count, text + index, width);
            count += width;
            index += width;
        }
    }
    if (!reserve_text(core, core->length + count)) {
        free(normalized);
        return 0;
    }
    if (count && !prepare_edit(core)) {
        free(normalized);
        return 0;
    }
    memmove(core->text + core->cursor + count, core->text + core->cursor,
        core->length - core->cursor + 1);
    memcpy(core->text + core->cursor, normalized, count);
    core->length += count;
    core->cursor += count;
    if (count)
        core->modified = 1;
    if (core->block_insert && memchr(normalized, '\n', count))
        core->block_insert = 0;
    if (core->mode == EDITOR_MODE_VISUAL || core->mode == EDITOR_MODE_VISUAL_LINE ||
        core->mode == EDITOR_MODE_VISUAL_BLOCK)
        core->mode = EDITOR_MODE_COMMAND;
    free(normalized);
    return 1;
}

static size_t line_start(const EditorCore *core, size_t position)
{
    while (position > 0 && core->text[position - 1] != '\n')
        --position;
    return position;
}

static size_t line_end(const EditorCore *core, size_t position)
{
    while (position < core->length && core->text[position] != '\n')
        ++position;
    return position;
}

static int find_line(const EditorCore *core, size_t index, size_t *start, size_t *end)
{
    *start = 0;
    while (index > 0) {
        size_t next = line_end(core, *start);
        if (next == core->length)
            return 0;
        *start = next + 1;
        --index;
    }
    *end = line_end(core, *start);
    return 1;
}

static size_t previous_character(const EditorCore *core, size_t position)
{
    if (position == 0)
        return 0;
    size_t start = position - 1;
    while (start > 0 && position - start < 4 &&
        ((unsigned char)core->text[start] & 0xC0) == 0x80)
        --start;
    unsigned long value;
    size_t length = decode_character(core->text + start, position - start, &value);
    return length && start + length == position ? start : position - 1;
}

static size_t next_character(const EditorCore *core, size_t position)
{
    if (position == core->length)
        return position;
    unsigned long value;
    size_t length = decode_character(core->text + position, core->length - position, &value);
    return position + (length ? length : 1);
}

static int erase_text(EditorCore *core, size_t start, size_t end)
{
    if (start != end && !prepare_edit(core))
        return 0;
    if (core->block_insert && (start < core->block_insert_start || end > core->cursor))
        core->block_insert = 0;
    if (start != end)
        core->modified = 1;
    memmove(core->text + start, core->text + end,
        core->length - end + 1);
    core->length -= end - start;
    if (core->cursor > end)
        core->cursor -= end - start;
    else if (core->cursor > start)
        core->cursor = start;
    if (core->mode == EDITOR_MODE_VISUAL || core->mode == EDITOR_MODE_VISUAL_LINE ||
        core->mode == EDITOR_MODE_VISUAL_BLOCK)
        core->mode = EDITOR_MODE_COMMAND;
    return 1;
}

static size_t clamp_column(const EditorCore *core, size_t start, size_t end, size_t column)
{
    size_t position = start + (column < end - start ? column : end - start);
    size_t candidate = position;
    while (candidate > start && position - candidate < 3 && candidate < end &&
        ((unsigned char)core->text[candidate] & 0xC0) == 0x80)
        --candidate;
    unsigned long value;
    size_t length = decode_character(core->text + candidate, end - candidate, &value);
    if (length && candidate + length > position)
        position = candidate;
    return position;
}

EditorCore *editor_core_create(const char *initial_text)
{
    EditorCore *core = calloc(1, sizeof(*core));
    if (!core)
        return NULL;
    core->settings.maxcol = 80;
    core->settings.tabwidth = 8;
    core->settings.expandtabs = 1;
    if (!insert_text(core, initial_text ? initial_text : "")) {
        editor_core_destroy(core);
        return NULL;
    }
    core->cursor = 0;
    core->modified = 0;
    core->history_enabled = 1;
    return core;
}

void editor_core_destroy(EditorCore *core)
{
    if (core) {
        clear_replace_history(core);
        free_history(core->undo);
        free_history(core->redo);
        free(core->replace_edits);
        free(core->register_text);
        free(core->text);
        free(core);
    }
}

typedef struct TextRange {
    size_t start;
    size_t end;
    int linewise;
} TextRange;

static int is_visual(const EditorCore *core)
{
    return core->mode == EDITOR_MODE_VISUAL || core->mode == EDITOR_MODE_VISUAL_LINE ||
        core->mode == EDITOR_MODE_VISUAL_BLOCK;
}

static size_t character_column(const EditorCore *core, size_t position)
{
    size_t column = 0;
    for (size_t offset = line_start(core, position); offset < position;
        offset = next_character(core, offset))
        ++column;
    return column;
}

static size_t advance_columns(const EditorCore *core, size_t column, size_t start, size_t end)
{
    while (start < end) {
        size_t width = core->text[start] == '\t' ? core->settings.tabwidth - column % core->settings.tabwidth : 1;
        if (width > SIZE_MAX - column)
            return SIZE_MAX;
        column += width;
        start = next_character(core, start);
    }
    return column;
}

static size_t column_position(const EditorCore *core, size_t start, size_t end, size_t column)
{
    while (column-- && start < end)
        start = next_character(core, start);
    return start;
}

static size_t position_line(const EditorCore *core, size_t position)
{
    size_t line = 0;
    for (size_t offset = 0; offset < position; ++offset)
        if (core->text[offset] == '\n')
            ++line;
    return line;
}

static void block_bounds(const EditorCore *core, size_t *first, size_t *last,
    size_t *left, size_t *right)
{
    size_t anchor = position_line(core, core->anchor);
    size_t cursor = position_line(core, core->cursor);
    *first = anchor < cursor ? anchor : cursor;
    *last = anchor > cursor ? anchor : cursor;
    *left = core->block_anchor_column < core->block_column ? core->block_anchor_column : core->block_column;
    *right = core->block_anchor_column > core->block_column ? core->block_anchor_column : core->block_column;
}

static int finish_block_insert(EditorCore *core)
{
    if (!core->block_insert)
        return 1;
    size_t length = core->cursor - core->block_insert_start;
    size_t copies = core->block_last - core->block_first;
    if (length && copies > (SIZE_MAX - 1 - core->length) / length)
        return 0;
    char *text = length ? malloc(length) : NULL;
    if (length && !text)
        return 0;
    if (length)
        memcpy(text, core->text + core->block_insert_start, length);
    if (!reserve_text(core, core->length + copies * length)) {
        free(text);
        return 0;
    }
    if (length) {
        for (size_t line = core->block_last; line > core->block_first; --line) {
            size_t start, end;
            if (!find_line(core, line, &start, &end))
                continue;
            size_t position = column_position(core, start, end, core->block_insert_column);
            if (character_column(core, position) < core->block_insert_column)
                continue;
            if (!prepare_edit(core)) {
                free(text);
                return 0;
            }
            memmove(core->text + position + length, core->text + position,
                core->length - position + 1);
            memcpy(core->text + position, text, length);
            core->length += length;
            core->modified = 1;
        }
        core->cursor = previous_character(core, core->cursor);
    }
    free(text);
    core->block_insert = 0;
    return 1;
}

static int use_block(EditorCore *core, int remove)
{
    size_t first, last, left, right;
    block_bounds(core, &first, &last, &left, &right);
    size_t rows = last - first;
    if (rows > SIZE_MAX - 1 - core->length)
        return 0;
    char *text = malloc(core->length + rows + 1);
    if (!text)
        return 0;
    size_t length = 0;
    for (size_t line = first;; ++line) {
        size_t start, end;
        find_line(core, line, &start, &end);
        start = column_position(core, start, end, left);
        end = column_position(core, start, end, right - left + 1);
        memcpy(text + length, core->text + start, end - start);
        length += end - start;
        if (line == last)
            break;
        text[length++] = '\n';
    }
    text[length] = 0;
    free(core->register_text);
    core->register_text = text;
    core->register_length = length;
    core->register_linewise = 0;
    if (remove) {
        for (size_t line = last;; --line) {
            size_t start, end;
            find_line(core, line, &start, &end);
            start = column_position(core, start, end, left);
            end = column_position(core, start, end, right - left + 1);
            if (!erase_text(core, start, end))
                return 0;
            if (line == first)
                break;
        }
    }
    size_t start, end;
    find_line(core, first, &start, &end);
    core->cursor = column_position(core, start, end, left);
    core->mode = EDITOR_MODE_COMMAND;
    return 1;
}

static void reset_command(EditorCore *core)
{
    core->count = 0;
    core->operator_count = 0;
    core->operator_has_count = 0;
    core->pending = 0;
}

static int step_history(EditorCore *core, size_t count, int redo)
{
    int changed = 0;
    HistoryEntry **source = redo ? &core->redo : &core->undo;
    HistoryEntry **destination = redo ? &core->undo : &core->redo;
    while (count-- && *source) {
        HistoryEntry *entry = *source;
        *source = entry->next;
        char *text = core->text;
        size_t length = core->length;
        size_t cursor = core->revision_cursor;
        size_t revision = core->revision;
        core->text = entry->text;
        core->length = entry->length;
        core->capacity = entry->length + 1;
        core->cursor = core->revision_cursor = entry->cursor;
        core->revision = entry->revision;
        entry->text = text;
        entry->length = length;
        entry->cursor = cursor;
        entry->revision = revision;
        entry->next = *destination;
        *destination = entry;
        core->modified = core->revision != core->saved_revision;
        changed = 1;
    }
    core->status = changed ? EDITOR_STATUS_NONE : EDITOR_STATUS_NO_CHANGE;
    core->history_open = 0;
    core->block_insert = 0;
    clear_replace_history(core);
    core->anchor = core->cursor;
    core->mode = EDITOR_MODE_COMMAND;
    reset_command(core);
    return 1;
}

static void reset_document(EditorCore *core)
{
    free_history(core->undo);
    free_history(core->redo);
    core->undo = core->redo = NULL;
    core->revision = core->next_revision = core->saved_revision = core->revision_cursor = 0;
    core->history_open = core->history_changed = 0;
    clear_replace_history(core);
    core->block_insert = 0;
    core->cursor = 0;
    core->anchor = 0;
    core->mode = EDITOR_MODE_COMMAND;
    core->modified = 0;
    reset_command(core);
    core->status = EDITOR_STATUS_NONE;
}

static int set_buffer(EditorCore *core, const EditorBuffer *buffer)
{
    if (!buffer || (buffer->length && !buffer->data) || buffer->length == SIZE_MAX)
        return 0;
    char *text = malloc(buffer->length + 1);
    if (!text)
        return 0;
    if (buffer->length)
        memcpy(text, buffer->data, buffer->length);
    text[buffer->length] = 0;
    free(core->text);
    core->text = text;
    core->length = buffer->length;
    core->capacity = buffer->length + 1;
    reset_document(core);
    return 1;
}

static int load_file(EditorCore *core, const char *path)
{
    if (!path || !*path)
        return 0;
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    EditorCore loaded = { 0 };
    int success = reserve_text(&loaded, 0);
    char chunk[4096];
    while (success) {
        size_t count = fread(chunk, 1, sizeof(chunk), file);
        if (count > SIZE_MAX - loaded.length || !reserve_text(&loaded, loaded.length + count)) {
            success = 0;
            break;
        }
        memcpy(loaded.text + loaded.length, chunk, count);
        loaded.length += count;
        if (count < sizeof(chunk)) {
            success = !ferror(file);
            break;
        }
    }
    if (fclose(file) != 0)
        success = 0;
    if (!success) {
        free(loaded.text);
        return 0;
    }
    loaded.text[loaded.length] = 0;
    free(core->text);
    core->text = loaded.text;
    core->length = loaded.length;
    core->capacity = loaded.capacity;
    reset_document(core);
    return 1;
}

static int save_file(EditorCore *core, const char *path)
{
    if (!path || !*path)
        return 0;
    FILE *file = fopen(path, "wb");
    if (!file)
        return 0;
    int success = fwrite(core->text, 1, core->length, file) == core->length;
    if (fclose(file) != 0)
        success = 0;
    if (success) {
        core->modified = 0;
        core->saved_revision = core->revision;
        core->history_open = 0;
    }
    return success;
}

static TextRange selected_range(const EditorCore *core)
{
    TextRange range;
    range.start = core->anchor < core->cursor ? core->anchor : core->cursor;
    range.end = core->anchor > core->cursor ? core->anchor : core->cursor;
    range.linewise = core->mode == EDITOR_MODE_VISUAL_LINE;
    if (range.linewise) {
        range.start = line_start(core, range.start);
        range.end = line_end(core, range.end);
        if (range.end < core->length)
            ++range.end;
    } else {
        range.end = next_character(core, range.end);
    }
    return range;
}

static int use_range(EditorCore *core, TextRange range, int remove)
{
    size_t length = range.end - range.start;
    if (!length && !range.linewise) {
        core->mode = EDITOR_MODE_COMMAND;
        return 1;
    }
    int append_newline = range.linewise && (!length || core->text[range.end - 1] != '\n');
    if (length > SIZE_MAX - 1 - append_newline)
        return 0;
    char *text = malloc(length + append_newline + 1);
    if (!text)
        return 0;
    memcpy(text, core->text + range.start, length);
    if (append_newline)
        text[length++] = '\n';
    text[length] = 0;
    free(core->register_text);
    core->register_text = text;
    core->register_length = length;
    core->register_linewise = range.linewise;
    if (remove) {
        if (range.linewise && range.end == core->length && range.start > 0)
            --range.start;
        if (!erase_text(core, range.start, range.end))
            return 0;
        core->cursor = range.start;
        if (range.linewise)
            core->cursor = line_start(core, core->cursor);
    } else if (is_visual(core)) {
        core->cursor = range.start;
    }
    core->mode = EDITOR_MODE_COMMAND;
    return 1;
}

static TextRange counted_lines(const EditorCore *core, size_t count)
{
    TextRange range = { line_start(core, core->cursor), core->cursor, 1 };
    while (count--) {
        range.end = line_end(core, range.end);
        if (range.end == core->length)
            break;
        ++range.end;
    }
    return range;
}

static int word_class(const EditorCore *core, size_t position)
{
    unsigned char byte = (unsigned char)core->text[position];
    if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
        return 0;
    return byte >= 0x80 || (byte >= 'a' && byte <= 'z') ||
        (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '_' ? 1 : 2;
}

static size_t word_forward(const EditorCore *core, size_t position)
{
    if (position < core->length) {
        int category = word_class(core, position);
        do {
            position = next_character(core, position);
        } while (position < core->length && word_class(core, position) == category);
    }
    while (position < core->length && !word_class(core, position))
        position = next_character(core, position);
    return position;
}

static size_t word_backward(const EditorCore *core, size_t position)
{
    if (!position)
        return 0;
    position = previous_character(core, position);
    while (position && !word_class(core, position))
        position = previous_character(core, position);
    int category = word_class(core, position);
    while (position && word_class(core, previous_character(core, position)) == category)
        position = previous_character(core, position);
    return position;
}

static void goto_line(EditorCore *core, size_t number)
{
    size_t position = 0;
    while (number > 1) {
        size_t end = line_end(core, position);
        if (end == core->length)
            break;
        position = end + 1;
        --number;
    }
    core->cursor = position;
}

static int move_command(EditorCore *core, unsigned long key, size_t count, int explicit_count)
{
    if (core->mode == EDITOR_MODE_VISUAL_BLOCK &&
        (key == 'h' || key == 'l' || key == EDITOR_KEY_LEFT || key == EDITOR_KEY_RIGHT)) {
        size_t start = line_start(core, core->cursor);
        size_t end = line_end(core, core->cursor);
        while (count--) {
            size_t previous = core->cursor;
            if ((key == 'h' || key == EDITOR_KEY_LEFT) && core->cursor > start)
                core->cursor = previous_character(core, core->cursor);
            else if ((key == 'l' || key == EDITOR_KEY_RIGHT) && core->cursor < end)
                core->cursor = next_character(core, core->cursor);
            if (previous == core->cursor)
                break;
        }
        return 1;
    }
    if (core->mode == EDITOR_MODE_VISUAL_BLOCK &&
        (key == 'j' || key == 'k' || key == EDITOR_KEY_UP || key == EDITOR_KEY_DOWN)) {
        size_t line = position_line(core, core->cursor);
        while (count--) {
            size_t target = key == 'j' || key == EDITOR_KEY_DOWN ? line + 1 : (line ? line - 1 : 0);
            size_t start, end;
            if (target == line || !find_line(core, target, &start, &end))
                break;
            core->cursor = column_position(core, start, end, core->block_column);
            line = target;
        }
        return 1;
    }
    EditorMessage message;
    switch (key) {
    case 'h': case EDITOR_KEY_LEFT: message = EDITOR_MOVE_LEFT; break;
    case 'l': case EDITOR_KEY_RIGHT: message = EDITOR_MOVE_RIGHT; break;
    case 'j': case EDITOR_KEY_DOWN: message = EDITOR_MOVE_DOWN; break;
    case 'k': case EDITOR_KEY_UP: message = EDITOR_MOVE_UP; break;
    case '0': case EDITOR_KEY_HOME: message = EDITOR_MOVE_HOME; break;
    case '$': case EDITOR_KEY_END:
        if (count > 1)
            move_command(core, 'j', count - 1, 1);
        core->cursor = line_end(core, core->cursor);
        return 1;
    case 'G':
        goto_line(core, explicit_count ? count : SIZE_MAX);
        if (core->mode == EDITOR_MODE_VISUAL_BLOCK) {
            size_t start = line_start(core, core->cursor);
            core->cursor = column_position(core, start, line_end(core, start), core->block_column);
        }
        return 1;
    case 'w': case 'b':
        while (count--) {
            size_t previous = core->cursor;
            core->cursor = key == 'w' ? word_forward(core, previous) : word_backward(core, previous);
            if (previous == core->cursor)
                break;
        }
        return 1;
    default: return 0;
    }
    while (count--) {
        size_t previous = core->cursor;
        editor_core_send(core, message, NULL);
        if (previous == core->cursor)
            break;
    }
    return 1;
}

static int paste_register(EditorCore *core, size_t count, int before)
{
    if (!core->register_length)
        return 1;
    TextRange range = { core->cursor, core->cursor, 0 };
    int visual = is_visual(core);
    if (visual)
        range = selected_range(core);
    else if (core->register_linewise) {
        range.start = before ? line_start(core, core->cursor) : line_end(core, core->cursor);
        if (!before && range.start < core->length)
            ++range.start;
        range.end = range.start;
    } else {
        if (!before && range.start < line_end(core, range.start))
            range.start = next_character(core, range.start);
        range.end = range.start;
    }
    int prefix = core->register_linewise && !visual && !before && range.start == core->length && core->length;
    int trim = core->register_linewise && !visual && (!before || !core->length) && range.start == core->length;
    int suffix = visual && range.linewise && !core->register_linewise &&
        core->register_text[core->register_length - 1] != '\n';
    if (count > (SIZE_MAX - 2) / core->register_length)
        return 0;
    size_t length = core->register_length * count + prefix - trim + suffix;
    if (length > SIZE_MAX - 1 - (core->length - (range.end - range.start)))
        return 0;
    char *text = malloc(length + 1);
    if (!text)
        return 0;
    size_t offset = 0;
    if (prefix)
        text[offset++] = '\n';
    for (size_t repeat = 0; repeat < count; ++repeat) {
        size_t part = core->register_length - (trim && repeat == count - 1);
        memcpy(text + offset, core->register_text, part);
        offset += part;
    }
    if (suffix)
        text[offset++] = '\n';
    size_t new_length = core->length - (range.end - range.start) + length;
    if (!reserve_text(core, new_length)) {
        free(text);
        return 0;
    }
    int changed = length != range.end - range.start ||
        (length && memcmp(core->text + range.start, text, length) != 0);
    if (changed && !prepare_edit(core)) {
        free(text);
        return 0;
    }
    memmove(core->text + range.start + length, core->text + range.end,
        core->length - range.end + 1);
    memcpy(core->text + range.start, text, length);
    core->length = new_length;
    if (changed)
        core->modified = 1;
    core->cursor = core->register_linewise || range.linewise ? range.start + prefix :
        previous_character(core, range.start + length);
    core->mode = EDITOR_MODE_COMMAND;
    free(text);
    return 1;
}

static int write_span(EditorCore *core, size_t start, size_t end, const char *text, size_t length)
{
    size_t remaining = core->length - (end - start);
    if (length > SIZE_MAX - 1 - remaining || !reserve_text(core, remaining + length))
        return 0;
    if (length != end - start || (length && memcmp(core->text + start, text, length) != 0)) {
        if (!prepare_edit(core))
            return 0;
        core->modified = 1;
    }
    memmove(core->text + start + length, core->text + end, core->length - end + 1);
    if (length)
        memcpy(core->text + start, text, length);
    core->length = remaining + length;
    return 1;
}

static size_t tab_spaces(const EditorCore *core)
{
    size_t column = advance_columns(core, 0, line_start(core, core->cursor), core->cursor);
    return core->settings.tabwidth - column % core->settings.tabwidth;
}

static int insert_tab(EditorCore *core)
{
    if (!core->settings.expandtabs)
        return insert_text(core, "\t");
    size_t length = tab_spaces(core);
    char *text = malloc(length + 1);
    if (!text)
        return 0;
    memset(text, ' ', length);
    text[length] = 0;
    int success = insert_text(core, text);
    free(text);
    return success;
}

static int replace_input(EditorCore *core, unsigned long key)
{
    char encoded[5];
    char *expanded = NULL;
    const char *text = encoded;
    size_t length;
    if (key == '\t' && core->settings.expandtabs) {
        length = tab_spaces(core);
        expanded = malloc(length);
        if (!expanded)
            return 0;
        memset(expanded, ' ', length);
        text = expanded;
    } else {
        length = encode_character(key == '\r' ? '\n' : key, encoded);
        if (!length)
            return 0;
    }
    size_t end = core->cursor;
    size_t limit = line_end(core, end);
    size_t count = expanded ? length : 1;
    if (key != '\r' && key != '\n')
        while (count-- && end < limit)
            end = next_character(core, end);
    if (core->replace_count == core->replace_capacity) {
        if (core->replace_capacity > SIZE_MAX / 2 / sizeof(ReplaceEdit)) {
            free(expanded);
            return 0;
        }
        size_t capacity = core->replace_capacity ? core->replace_capacity * 2 : 16;
        ReplaceEdit *edits = realloc(core->replace_edits, capacity * sizeof(*edits));
        if (!edits) {
            free(expanded);
            return 0;
        }
        core->replace_edits = edits;
        core->replace_capacity = capacity;
    }
    ReplaceEdit edit;
    edit.position = core->cursor;
    edit.original_length = end - core->cursor;
    edit.inserted_length = length;
    edit.original = malloc(edit.original_length ? edit.original_length : 1);
    if (!edit.original) {
        free(expanded);
        return 0;
    }
    memcpy(edit.original, core->text + core->cursor, edit.original_length);
    if (!write_span(core, core->cursor, end, text, length)) {
        free(edit.original);
        free(expanded);
        return 0;
    }
    free(expanded);
    core->replace_edits[core->replace_count++] = edit;
    core->cursor += length;
    return 1;
}

static int replace_characters(EditorCore *core, unsigned long key, size_t count)
{
    char encoded[5];
    size_t width = encode_character(key == '\r' ? '\n' : key, encoded);
    if (!width || (key < ' ' && key != '\t' && key != '\r' && key != '\n') || key == 127)
        return 1;
    size_t start = core->cursor, end = start;
    size_t first = 0, last = 0, left = 0, right = 0;
    int block = core->mode == EDITOR_MODE_VISUAL_BLOCK;
    int visual = is_visual(core);
    if (block) {
        block_bounds(core, &first, &last, &left, &right);
        find_line(core, first, &start, &end);
        size_t ignored;
        find_line(core, last, &ignored, &end);
    } else if (visual) {
        TextRange range = selected_range(core);
        start = range.start;
        end = range.end;
    } else {
        size_t limit = line_end(core, start);
        while (count--) {
            if (end == limit)
                return 1;
            end = next_character(core, end);
        }
    }
    size_t characters = 0, line = first, column = 0;
    for (size_t position = start; position < end; position = next_character(core, position)) {
        if (core->text[position] == '\n') {
            ++line;
            column = 0;
        } else {
            if (!block || (line <= last && column >= left && column <= right))
                ++characters;
            ++column;
        }
    }
    if (characters > (SIZE_MAX - 1 - core->length) / width)
        return 0;
    size_t capacity = end - start + characters * width;
    char *text = malloc(capacity ? capacity : 1);
    if (!text)
        return 0;
    size_t length = 0, cursor = start;
    int positioned = 0;
    line = first;
    column = 0;
    for (size_t position = start; position < end;) {
        size_t next = next_character(core, position);
        int newline = core->text[position] == '\n';
        int replace = !newline && (!block || (line <= last && column >= left && column <= right));
        if (replace) {
            if (!positioned) {
                cursor = start + length;
                positioned = 1;
            }
            memcpy(text + length, encoded, width);
            length += width;
        } else {
            memcpy(text + length, core->text + position, next - position);
            length += next - position;
        }
        if (newline) {
            ++line;
            column = 0;
        } else {
            ++column;
        }
        position = next;
    }
    int success = write_span(core, start, end, text, length);
    if (success) {
        core->cursor = visual ? cursor : previous_character(core, start + length);
        core->mode = EDITOR_MODE_COMMAND;
    }
    free(text);
    return success;
}

static int handle_key(EditorCore *core, unsigned long key)
{
    core->status = EDITOR_STATUS_NONE;
    if (key == 27) {
        if (!finish_block_insert(core))
            return 0;
        if (core->mode == EDITOR_MODE_REPLACE && core->replace_count &&
            core->cursor > line_start(core, core->cursor))
            core->cursor = previous_character(core, core->cursor);
        clear_replace_history(core);
        core->mode = EDITOR_MODE_COMMAND;
        reset_command(core);
        return 1;
    }
    if (!key || key > EDITOR_KEY_END ||
        (key > 0x10FFFF && key < EDITOR_KEY_DELETE) || (key >= 0xD800 && key <= 0xDFFF))
        return 0;
    if (core->mode == EDITOR_MODE_REPLACE) {
        if (key == '\b') {
            if (!core->replace_count)
                return 1;
            ReplaceEdit *edit = &core->replace_edits[core->replace_count - 1];
            if (!write_span(core, edit->position, edit->position + edit->inserted_length,
                edit->original, edit->original_length))
                return 0;
            core->cursor = edit->position;
            free(edit->original);
            --core->replace_count;
            return 1;
        }
        if (key >= EDITOR_KEY_DELETE) {
            clear_replace_history(core);
            return key == EDITOR_KEY_DELETE ? editor_core_send(core, EDITOR_DELETE_CHAR, NULL) :
                move_command(core, key, 1, 0);
        }
        if ((key < ' ' && key != '\t' && key != '\r' && key != '\n') || key == 127)
            return 1;
        return replace_input(core, key);
    }
    if (core->pending == 'r') {
        size_t count = core->operator_count;
        reset_command(core);
        return replace_characters(core, key, count);
    }
    if (core->mode == EDITOR_MODE_INSERT) {
        if (core->block_insert) {
            if (key == '\b' && core->cursor == core->block_insert_start)
                return 1;
            if (key == EDITOR_KEY_DELETE || key >= EDITOR_KEY_LEFT || key == '\r' || key == '\n')
                core->block_insert = 0;
        }
        if (key == EDITOR_KEY_DELETE)
            return editor_core_send(core, EDITOR_DELETE_CHAR, NULL);
        if (key >= EDITOR_KEY_LEFT)
            return move_command(core, key, 1, 0);
        switch (key) {
        case '\b': return editor_core_send(core, EDITOR_BACKSPACE, NULL);
        case '\t': return insert_tab(core);
        case '\r':
        case '\n': return editor_core_send(core, EDITOR_ADD_CHAR, "\n");
        default:
            if (key < ' ' || key == 127)
                return 1;
            return editor_core_send(core, EDITOR_ADD_CODEPOINT, &key);
        }
    }
    if (key >= '0' && key <= '9' && (key != '0' || core->count)) {
        size_t digit = key - '0';
        if (core->count > (SIZE_MAX - digit) / 10)
            return 0;
        core->count = core->count * 10 + digit;
        return 1;
    }
    int explicit_count = core->count != 0;
    size_t count = explicit_count ? core->count : 1;
    unsigned long pending = core->pending;
    if (pending) {
        int motion_counted = explicit_count || core->operator_has_count;
        if (core->operator_count && count > SIZE_MAX / core->operator_count) {
            reset_command(core);
            return 0;
        }
        count *= core->operator_count ? core->operator_count : 1;
        reset_command(core);
        if (pending == 'g') {
            if (key == 'g')
                goto_line(core, count);
            if (core->mode == EDITOR_MODE_VISUAL_BLOCK) {
                size_t start = line_start(core, core->cursor);
                core->cursor = column_position(core, start, line_end(core, start), core->block_column);
            }
            return 1;
        }
        if (key == pending)
            return use_range(core, counted_lines(core, count), pending == 'd');
        size_t original = core->cursor;
        if (!move_command(core, key, count, motion_counted))
            return 1;
        TextRange range = { original < core->cursor ? original : core->cursor,
            original > core->cursor ? original : core->cursor, 0 };
        if (key == 'j' || key == 'k' || key == 'G' || key == EDITOR_KEY_UP || key == EDITOR_KEY_DOWN) {
            range.linewise = 1;
            range.start = line_start(core, range.start);
            range.end = line_end(core, range.end);
            if (range.end < core->length)
                ++range.end;
        }
        core->cursor = original;
        return use_range(core, range, pending == 'd');
    }
    core->count = 0;
    if (key == 'u' || key == 18)
        return step_history(core, count, key == 18);
    if (key == 'r') {
        core->pending = key;
        core->operator_count = count;
        return 1;
    }
    if (key == 'R' && !is_visual(core)) {
        clear_replace_history(core);
        core->mode = EDITOR_MODE_REPLACE;
        return 1;
    }
    if (key == 'v' || key == 'V' || key == 22) {
        EditorMode mode = key == 22 ? EDITOR_MODE_VISUAL_BLOCK :
            (key == 'v' ? EDITOR_MODE_VISUAL : EDITOR_MODE_VISUAL_LINE);
        if (core->mode == mode)
            core->mode = EDITOR_MODE_COMMAND;
        else {
            if (!is_visual(core))
                core->anchor = core->cursor;
            core->mode = mode;
            if (mode == EDITOR_MODE_VISUAL_BLOCK) {
                core->block_anchor_column = character_column(core, core->anchor);
                core->block_column = character_column(core, core->cursor);
            }
        }
        return 1;
    }
    if (core->mode == EDITOR_MODE_VISUAL_BLOCK) {
        if (key == 'I') {
            size_t right;
            block_bounds(core, &core->block_first, &core->block_last, &core->block_insert_column, &right);
            size_t start, end;
            find_line(core, core->block_first, &start, &end);
            core->cursor = column_position(core, start, end, core->block_insert_column);
            core->block_insert_column = character_column(core, core->cursor);
            core->block_insert_start = core->cursor;
            core->block_insert = 1;
            core->mode = EDITOR_MODE_INSERT;
            return 1;
        }
        if (key == 'y' || key == 'd' || key == 'x' || key == EDITOR_KEY_DELETE)
            return use_block(core, key != 'y');
        if (key == 'p' || key == 'P')
            return 1;
    }
    if (is_visual(core) && (key == 'y' || key == 'd' || key == 'x' || key == EDITOR_KEY_DELETE))
        return use_range(core, selected_range(core), key != 'y');
    if (key == 'p' || key == 'P')
        return paste_register(core, count, key == 'P');
    if (key == 'g' || (!is_visual(core) && (key == 'd' || key == 'y'))) {
        core->pending = key;
        core->operator_count = count;
        core->operator_has_count = explicit_count;
        return 1;
    }
    if (key == 'i' && !is_visual(core)) {
        core->mode = EDITOR_MODE_INSERT;
        return 1;
    }
    if (key == 'x' || key == EDITOR_KEY_DELETE) {
        TextRange range = { core->cursor, core->cursor, 0 };
        size_t end = line_end(core, core->cursor);
        while (count-- && range.end < end)
            range.end = next_character(core, range.end);
        return use_range(core, range, 1);
    }
    move_command(core, key, count, explicit_count);
    if (core->mode == EDITOR_MODE_VISUAL_BLOCK && key != 'j' && key != 'k' &&
        key != EDITOR_KEY_UP && key != EDITOR_KEY_DOWN && key != 'G')
        core->block_column = character_column(core, core->cursor);
    return 1;
}

static int dispatch_message(EditorCore *core, EditorMessage message, void *data)
{
    if (!core)
        return 0;
    switch (message) {
    case EDITOR_ADD_CHAR: case EDITOR_ADD_CODEPOINT: case EDITOR_INSERT_TEXT:
    case EDITOR_BACKSPACE: case EDITOR_DELETE_CHAR: case EDITOR_DELETE_LINE:
    case EDITOR_SET_CARET: case EDITOR_MOVE_LEFT: case EDITOR_MOVE_RIGHT:
    case EDITOR_MOVE_UP: case EDITOR_MOVE_DOWN: case EDITOR_MOVE_HOME: case EDITOR_MOVE_END:
        clear_replace_history(core);
        core->status = EDITOR_STATUS_NONE;
        break;
    default: break;
    }
    switch (message) {
    case EDITOR_GET_SETTINGS:
        if (!data)
            return 0;
        *(EditorSettings *)data = core->settings;
        return 1;
    case EDITOR_SET_SETTINGS: {
        const EditorSettings *settings = data;
        if (!settings || !settings->tabwidth || settings->tabwidth == SIZE_MAX)
            return 0;
        core->settings = *settings;
        core->settings.expandtabs = settings->expandtabs != 0;
        return 1;
    }
    case EDITOR_GET_STATUS:
        if (!data)
            return 0;
        *(EditorStatus *)data = core->status;
        return 1;
    case EDITOR_UNDO:
        return step_history(core, 1, 0);
    case EDITOR_REDO:
        return step_history(core, 1, 1);
    case EDITOR_SET_BUFFER:
        return set_buffer(core, data);
    case EDITOR_GET_BUFFER: {
        EditorBuffer *buffer = data;
        if (!buffer)
            return 0;
        buffer->data = (const unsigned char *)core->text;
        buffer->length = core->length;
        return 1;
    }
    case EDITOR_LOAD_FILE:
        return load_file(core, data);
    case EDITOR_SAVE_FILE:
        return save_file(core, data);
    case EDITOR_GET_MODIFIED:
        if (!data)
            return 0;
        *(int *)data = core->modified;
        return 1;
    case EDITOR_GET_REGISTER: {
        EditorRegister *value = data;
        if (!value)
            return 0;
        value->text = core->register_text ? core->register_text : "";
        value->length = core->register_length;
        value->linewise = core->register_linewise;
        return 1;
    }
    case EDITOR_GET_LINE_CARET: {
        EditorLineCaret *caret = data;
        size_t start, end;
        if (!caret || !find_line(core, caret->line, &start, &end))
            return 0;
        caret->active = 0;
        caret->column = caret->advance = 0;
        size_t first, last, left, right;
        if (core->mode == EDITOR_MODE_VISUAL_BLOCK) {
            block_bounds(core, &first, &last, &left, &right);
            if (caret->line >= first && caret->line <= last) {
                caret->active = 1;
                caret->column = column_position(core, start, end, core->block_column) - start;
            }
        } else if (core->block_insert && caret->line >= core->block_first && caret->line <= core->block_last) {
            size_t position = column_position(core, start, end, core->block_insert_column);
            if (character_column(core, position) == core->block_insert_column) {
                caret->active = 1;
                caret->column = position - start;
                size_t column = advance_columns(core, 0, start, position);
                caret->advance = advance_columns(core, column, core->block_insert_start, core->cursor) - column;
                if (caret->line == core->block_first) {
                    caret->column = core->cursor - start;
                    caret->advance = 0;
                }
            }
        }
        return 1;
    }
    case EDITOR_GET_SELECTION: {
        EditorSelection *selection = data;
        size_t start, end;
        if (!selection || !find_line(core, selection->line, &start, &end))
            return 0;
        selection->start = selection->end = 0;
        selection->active = selection->includes_newline = 0;
        if (core->mode == EDITOR_MODE_VISUAL_BLOCK) {
            size_t first, last, left, right;
            block_bounds(core, &first, &last, &left, &right);
            selection->active = selection->line >= first && selection->line <= last;
            if (selection->active) {
                selection->start = column_position(core, start, end, left) - start;
                selection->end = column_position(core, start + selection->start, end, right - left + 1) - start;
            }
        } else if (is_visual(core)) {
            TextRange range = selected_range(core);
            selection->active = range.linewise ?
                start <= line_start(core, core->anchor > core->cursor ? core->anchor : core->cursor) && end >= range.start :
                range.start <= end && range.end > start;
            if (selection->active) {
                selection->start = range.start > start ? range.start - start : 0;
                selection->end = range.end < end ? range.end - start : end - start;
                selection->includes_newline = range.linewise || range.end > end;
            }
        }
        return 1;
    }
    case EDITOR_HANDLE_KEY:
        return data ? handle_key(core, *(const unsigned long *)data) : 0;
    case EDITOR_GET_MODE:
        if (!data)
            return 0;
        *(EditorMode *)data = core->mode;
        return 1;
    case EDITOR_ADD_CHAR: {
        if (!data)
            return 0;
        const char *text = data;
        size_t length = strlen(text);
        unsigned long value;
        if (!length || decode_character(text, length, &value) != length)
            return 0;
        return insert_text(core, text);
    }
    case EDITOR_ADD_CODEPOINT: {
        if (!data)
            return 0;
        unsigned long value = *(const unsigned long *)data;
        char text[5] = { 0 };
        if (!encode_character(value, text))
            return 0;
        return insert_text(core, text);
    }
    case EDITOR_INSERT_TEXT:
        return insert_text(core, data);
    case EDITOR_BACKSPACE:
        return erase_text(core, previous_character(core, core->cursor), core->cursor);
    case EDITOR_DELETE_CHAR:
        return erase_text(core, core->cursor, next_character(core, core->cursor));
    case EDITOR_GET_LINE: {
        EditorLine *line = data;
        size_t start, end;
        if (!line || !find_line(core, line->index, &start, &end))
            return 0;
        line->text = core->text + start;
        line->length = end - start;
        return 1;
    }
    case EDITOR_GET_CHARACTER: {
        EditorCharacter *character = data;
        size_t start, end;
        if (!character || !find_line(core, character->line, &start, &end) ||
            character->column >= end - start)
            return 0;
        character->length = decode_character(core->text + start + character->column,
            end - start - character->column, &character->value);
        if (!character->length) {
            character->value = 0xFFFD;
            character->length = 1;
        }
        return 1;
    }
    case EDITOR_DELETE_LINE: {
        size_t start, end;
        if (!data || !find_line(core, *(const size_t *)data, &start, &end))
            return 0;
        if (end < core->length)
            ++end;
        else if (start > 0)
            --start;
        return erase_text(core, start, end);
    }
    case EDITOR_GET_LINE_COUNT: {
        if (!data)
            return 0;
        size_t count = 1;
        for (size_t index = 0; index < core->length; ++index)
            if (core->text[index] == '\n')
                ++count;
        *(size_t *)data = count;
        return 1;
    }
    case EDITOR_GET_CARET: {
        EditorCaret *caret = data;
        if (!caret)
            return 0;
        caret->line = 0;
        for (size_t index = 0; index < core->cursor; ++index)
            if (core->text[index] == '\n')
                ++caret->line;
        caret->column = core->cursor - line_start(core, core->cursor);
        return 1;
    }
    case EDITOR_SET_CARET: {
        const EditorCaret *caret = data;
        size_t start, end;
        if (!caret || !find_line(core, caret->line, &start, &end))
            return 0;
        core->block_insert = 0;
        core->cursor = clamp_column(core, start, end, caret->column);
        if (core->mode == EDITOR_MODE_VISUAL_BLOCK)
            core->block_column = character_column(core, core->cursor);
        return 1;
    }
    case EDITOR_MOVE_LEFT:
        core->block_insert = 0;
        core->cursor = previous_character(core, core->cursor);
        return 1;
    case EDITOR_MOVE_RIGHT:
        core->block_insert = 0;
        core->cursor = next_character(core, core->cursor);
        return 1;
    case EDITOR_MOVE_HOME:
        core->block_insert = 0;
        core->cursor = line_start(core, core->cursor);
        return 1;
    case EDITOR_MOVE_END:
        core->block_insert = 0;
        core->cursor = line_end(core, core->cursor);
        return 1;
    case EDITOR_MOVE_UP:
    case EDITOR_MOVE_DOWN: {
        core->block_insert = 0;
        size_t start = line_start(core, core->cursor);
        size_t column = core->cursor - start;
        size_t end;
        if (message == EDITOR_MOVE_UP) {
            if (!start)
                return 1;
            end = start - 1;
            start = line_start(core, end);
        } else {
            end = line_end(core, core->cursor);
            if (end == core->length)
                return 1;
            start = end + 1;
            end = line_end(core, start);
        }
        core->cursor = clamp_column(core, start, end, column);
        return 1;
    }
    default:
        return 0;
    }
}

int editor_core_send(EditorCore *core, EditorMessage message, void *data)
{
    if (!core)
        return 0;
    int outer = core->dispatch_depth == 0;
    int independent = 0;
    if (outer) {
        core->history_changed = 0;
        switch (message) {
        case EDITOR_ADD_CHAR: case EDITOR_ADD_CODEPOINT: case EDITOR_INSERT_TEXT:
        case EDITOR_BACKSPACE: case EDITOR_DELETE_CHAR: case EDITOR_DELETE_LINE:
        case EDITOR_SET_CARET: case EDITOR_MOVE_LEFT: case EDITOR_MOVE_RIGHT:
        case EDITOR_MOVE_UP: case EDITOR_MOVE_DOWN: case EDITOR_MOVE_HOME: case EDITOR_MOVE_END:
            core->history_open = 0;
            independent = 1;
            break;
        case EDITOR_HANDLE_KEY:
            if (data && *(const unsigned long *)data >= EDITOR_KEY_LEFT &&
                *(const unsigned long *)data <= EDITOR_KEY_END)
                core->history_open = 0;
            break;
        default: break;
        }
    }
    ++core->dispatch_depth;
    int success = dispatch_message(core, message, data);
    --core->dispatch_depth;
    if (outer) {
        if (core->history_changed || (core->history_open && core->mode == EDITOR_MODE_COMMAND))
            core->revision_cursor = core->cursor;
        if (independent || (message == EDITOR_HANDLE_KEY &&
            core->mode != EDITOR_MODE_INSERT && core->mode != EDITOR_MODE_REPLACE))
            core->history_open = 0;
    }
    return success;
}