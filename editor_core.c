#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "editor_core.h"
#include "lexer_c_cpp.h"
#include "lexer_intel_asm.h"
#include "lexer_makefile.h"
#include "lexer_batch.h"
#include "lexer_pascal.h"
#include "lexer_basic.h"
#include "lexer_html.h"
#include "lexer_ini.h"
#include "lexer_config_sys.h"
#include "virtual_buffer.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#define PCRE2_STATIC
#include "third_party/pcre2/src/pcre2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ReplaceEdit {
    size_t position;
    size_t cursor;
    size_t original_length;
    size_t inserted_length;
    char *original;
} ReplaceEdit;

typedef struct HistoryEntry {
    char *text;
    VirtualBuffer *file;
    size_t length;
    size_t cursor;
    size_t revision;
    EditorLineEnding line_ending;
    int literal_bytes;
    struct HistoryEntry *next;
} HistoryEntry;

struct EditorCore {
    FILE *output_stream;
    EditorDirectoryFunctions directory_functions;
    EditorDirectoryEntry *directory_entries;
    size_t directory_count;
    char *directory_path;
    int directory_active;
    int directory_changed;
    const Lexer **lexers;
    size_t lexer_count;
    const Lexer *lexer;
    char *text;
    VirtualBuffer *file;
    char *line_text;
    size_t line_capacity;
    FILE *stream_output;
    size_t length;
    size_t capacity;
    EditorLineEnding line_ending;
    int literal_bytes;
    char *serialized_text;
    size_t serialized_capacity;
    size_t cursor;
    EditorMode mode;
    EditorStatus status;
    EditorSettings settings;
    size_t page_lines;
    char command_prefix;
    int command_has_range;
    size_t command_range_start;
    size_t command_range_end;
    char *search_text;
    size_t search_length;
    int search_whole_word;
    EditorBufferSwitch buffer_switch;
    EditorChange document_change;
    unsigned long *change_keys;
    size_t change_length;
    size_t change_capacity;
    int change_dirty;
    unsigned long *last_change;
    size_t last_change_length;
    size_t last_change_capacity;
    size_t repeat_count;
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

static int write_span(EditorCore *core, size_t start, size_t end, const char *text, size_t length);

static unsigned char text_byte(const EditorCore *core, size_t position)
{
    return core->file ? virtual_buffer_byte(core->file, position) :
        (unsigned char)*(core->text + position);
}

static int read_text(const EditorCore *core, size_t offset, void *data, size_t length)
{
    if (offset > core->length || length > core->length - offset)
        return 0;
    if (core->file)
        return virtual_buffer_read(core->file, offset, data, length);
    if (length)
        memcpy(data, core->text + offset, length);
    return 1;
}

static int text_equal(const EditorCore *core, size_t offset, const char *text, size_t length)
{
    unsigned char chunk[65536];
    while (length) {
        size_t count = length > sizeof(chunk) ? sizeof(chunk) : length;
        if (!read_text(core, offset, chunk, count) || memcmp(chunk, text, count))
            return 0;
        offset += count;
        text += count;
        length -= count;
    }
    return 1;
}

static void clear_directory(EditorCore *core)
{
    for (size_t index = 0; index < core->directory_count; ++index) {
        free((void *)core->directory_entries[index].name);
        free((void *)core->directory_entries[index].path);
    }
    free(core->directory_entries);
    free(core->directory_path);
    core->directory_entries = NULL;
    core->directory_count = 0;
    core->directory_path = NULL;
    core->directory_active = 0;
    core->directory_changed = 0;
}

static char *copy_string(const char *text)
{
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy)
        memcpy(copy, text, length + 1);
    return copy;
}

static int register_lexer(EditorCore *core, const Lexer *lexer)
{
    if (!lexer || !lexer->name || !*lexer->name || !lexer->supports_extension ||
        !lexer->lex_line || !lexer->states_equal || !lexer->free_line)
        return 0;
    for (size_t index = 0; index < core->lexer_count; ++index) {
        if (core->lexers[index] == lexer)
            return 1;
        if (!strcmp(core->lexers[index]->name, lexer->name))
            return 0;
    }
    if (core->lexer_count >= SIZE_MAX / sizeof(*core->lexers))
        return 0;
    const Lexer **lexers = realloc((void *)core->lexers, (core->lexer_count + 1) * sizeof(*lexers));
    if (!lexers)
        return 0;
    core->lexers = lexers;
    core->lexers[core->lexer_count++] = lexer;
    return 1;
}

static int select_lexer(EditorCore *core, const char *filename)
{
    if (!filename)
        return 0;
    const char *basename = filename;
    const char *extension = NULL;
    for (const char *position = filename; *position; ++position) {
        if (*position == '/' || *position == '\\') {
            basename = position + 1;
            extension = NULL;
        }
        else if (*position == '.')
            extension = position;
    }
    core->lexer = NULL;
    for (size_t index = 0; index < core->lexer_count; ++index) {
        const Lexer *lexer = core->lexers[index];
        if ((lexer->supports_filename && lexer->supports_filename(basename)) ||
            (extension && lexer->supports_extension(extension))) {
            core->lexer = lexer;
            break;
        }
    }
    return 1;
}

static void clear_replace_history(EditorCore *core)
{
    while (core->replace_count)
        free(core->replace_edits[--core->replace_count].original);
}

static void free_history(HistoryEntry *entry)
{
    while (entry) {
        HistoryEntry *next = entry->next;
        virtual_buffer_destroy(entry->file);
        free(entry->text);
        free(entry);
        entry = next;
    }
}

static void note_changed_lines(EditorCore *core, size_t first, size_t last)
{
    ++core->document_change.revision;
    if (first < core->document_change.first_line)
        core->document_change.first_line = first;
    if (last > core->document_change.last_line)
        core->document_change.last_line = last;
}

static void note_change(EditorCore *core, size_t start, size_t end, const char *text, size_t length)
{
    if (core->file) {
        note_changed_lines(core, 0, SIZE_MAX);
        return;
    }
    size_t first = 0;
    size_t removed_lines = 0;
    size_t added_lines = 0;
    for (size_t index = 0; index < start; ++index)
        if (text_byte(core, index) == '\n')
            ++first;
    for (size_t index = start; index < end; ++index)
        if (text_byte(core, index) == '\n')
            ++removed_lines;
    for (size_t index = 0; index < length; ++index)
        if (text[index] == '\n')
            ++added_lines;
    note_changed_lines(core, first, removed_lines == added_lines ? first + added_lines : SIZE_MAX);
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
        entry->file = core->file ? virtual_buffer_clone(core->file) : NULL;
        entry->text = core->file ? NULL : malloc(core->length + 1);
        if (core->file ? !entry->file : !entry->text) {
            free(entry);
            return 0;
        }
        if (!core->file)
            memcpy(entry->text, core->text, core->length + 1);
        entry->length = core->length;
        entry->cursor = core->cursor;
        entry->revision = core->revision;
        entry->line_ending = core->line_ending;
        entry->literal_bytes = core->literal_bytes;
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
    if (core->file)
        return length < SIZE_MAX;
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
    int crlf = core->file && core->line_ending == EDITOR_LINE_ENDING_CRLF;
    if (crlf && length > (SIZE_MAX - 1) / 2)
        return 0;
    char *normalized = malloc((crlf ? length * 2 : length) + 1);
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
        if (value == '\r' || value == '\n') {
            if (crlf)
                normalized[count++] = '\r';
            normalized[count++] = '\n';
            index += value == '\r' && text[index + 1] == '\n' ? 2 : 1;
        } else {
            memcpy(normalized + count, text + index, width);
            count += width;
            index += width;
        }
    }
    if (count > SIZE_MAX - 1 - core->length || !reserve_text(core, core->length + count)) {
        free(normalized);
        return 0;
    }
    if (count && !prepare_edit(core)) {
        free(normalized);
        return 0;
    }
    if (count)
        note_change(core, core->cursor, core->cursor, normalized, count);
    if (core->file) {
        int success = virtual_buffer_replace(core->file, core->cursor, 0, normalized, count);
        if (success && core->block_insert && memchr(normalized, '\n', count))
            core->block_insert = 0;
        free(normalized);
        if (success) {
            core->cursor += count;
            core->length += count;
            core->modified |= count != 0;
            if (core->mode == EDITOR_MODE_VISUAL || core->mode == EDITOR_MODE_VISUAL_LINE ||
                core->mode == EDITOR_MODE_VISUAL_BLOCK)
                core->mode = EDITOR_MODE_COMMAND;
        }
        return success;
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
    if (core->file) {
        size_t line, start;
        return virtual_buffer_lines(core->file, position, &line) &&
            virtual_buffer_line_start(core->file, line, &start) ? start : position;
    }
    while (position > 0 && text_byte(core, position - 1) != '\n')
        --position;
    return position;
}

static size_t line_end(const EditorCore *core, size_t position)
{
    if (core->file) {
        size_t line, start, end;
        return virtual_buffer_lines(core->file, position, &line) &&
            virtual_buffer_line(core->file, line, &start, &end) ? end : position;
    }
    while (position < core->length && text_byte(core, position) != '\n')
        ++position;
    return position;
}

static int find_line(const EditorCore *core, size_t index, size_t *start, size_t *end)
{
    if (core->file)
        return virtual_buffer_line(core->file, index, start, end);
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

static size_t decode_text(const EditorCore *core, size_t position, size_t length, unsigned long *value)
{
    char bytes[4];
    if (length > sizeof(bytes))
        length = sizeof(bytes);
    return read_text(core, position, bytes, length) ? decode_character(bytes, length, value) : 0;
}

static const char *text_span(EditorCore *core, size_t start, size_t length)
{
    if (length >= 65536)
        return NULL;
    if (!core->line_text) {
        core->line_text = malloc(65536);
        if (!core->line_text)
            return NULL;
        core->line_capacity = 65536;
    }
    if (!read_text(core, start, core->line_text, length))
        return NULL;
    core->line_text[length] = 0;
    return core->line_text;
}

static size_t previous_character(const EditorCore *core, size_t position)
{
    if (position == 0)
        return 0;
    size_t start = position - 1;
    while (start > 0 && position - start < 4 &&
        ((unsigned char)text_byte(core, start) & 0xC0) == 0x80)
        --start;
    unsigned long value;
    size_t length = decode_text(core, start, position - start, &value);
    return length && start + length == position ? start : position - 1;
}

static size_t next_character(const EditorCore *core, size_t position)
{
    if (position == core->length)
        return position;
    unsigned long value;
    size_t length = decode_text(core, position, core->length - position, &value);
    return position + (length ? length : 1);
}

static int erase_text(EditorCore *core, size_t start, size_t end)
{
    if (start != end && !prepare_edit(core))
        return 0;
    if (start != end)
        note_change(core, start, end, NULL, 0);
    if (core->block_insert && (start < core->block_insert_start || end > core->cursor))
        core->block_insert = 0;
    if (start != end)
        core->modified = 1;
    if (core->file) {
        if (!virtual_buffer_replace(core->file, start, end - start, NULL, 0))
            return 0;
    } else {
    memmove(core->text + start, core->text + end,
        core->length - end + 1);
    }
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
        ((unsigned char)text_byte(core, candidate) & 0xC0) == 0x80)
        --candidate;
    unsigned long value;
    size_t length = decode_text(core, candidate, end - candidate, &value);
    if (length && candidate + length > position)
        position = candidate;
    return position;
}

EditorCore *editor_core_create(const char *initial_text)
{
    EditorCore *core = calloc(1, sizeof(*core));
    if (!core)
        return NULL;
    static const Lexer *const available_lexers[] = {
        &lexer_c_cpp, &lexer_intel_asm, &lexer_makefile, &lexer_batch, &lexer_pascal,
        &lexer_basic, &lexer_html, &lexer_ini, &lexer_config_sys
    };
    for (size_t index = 0; index < sizeof(available_lexers) / sizeof(available_lexers[0]); ++index) {
        if (!register_lexer(core, available_lexers[index])) {
            editor_core_destroy(core);
            return NULL;
        }
    }
    core->settings.maxcol = 80;
    core->settings.tabwidth = 8;
    core->settings.expandtabs = 1;
    core->page_lines = 20;
    core->command_prefix = ':';
    core->document_change.first_line = SIZE_MAX;
    core->file = virtual_buffer_create();
    if (!core->file) {
        editor_core_destroy(core);
        return NULL;
    }
    core->literal_bytes = 1;
    core->line_ending = initial_text && strstr(initial_text, "\r\n")
        ? EDITOR_LINE_ENDING_CRLF : EDITOR_LINE_ENDING_LF;
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
    if (!core) {
        return;
    }
    clear_directory(core);
    free(core->serialized_text);
    free((void *)core->lexers);
    clear_replace_history(core);
    free(core->search_text);
    free(core->change_keys);
    free(core->last_change);
    free_history(core->undo);
    free_history(core->redo);
    free(core->replace_edits);
    free(core->register_text);
    free(core->text);
    virtual_buffer_destroy(core->file);
    free(core->line_text);
    free(core);
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
        size_t width = text_byte(core, start) == '\t' ? core->settings.tabwidth - column % core->settings.tabwidth : 1;
        if (text_byte(core, start) == '\r' && start + 1 < core->length && text_byte(core, start + 1) == '\n')
            width = 0;
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
    if (core->file) {
        size_t line = 0;
        virtual_buffer_lines(core->file, position, &line);
        return line;
    }
    size_t line = 0;
    for (size_t offset = 0; offset < position; ++offset)
        if (text_byte(core, offset) == '\n')
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
    if (length && !read_text(core, core->block_insert_start, text, length)) {
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
            if (!write_span(core, position, position, text, length)) {
                free(text);
                return 0;
            }
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
    size_t capacity = last - first;
    for (size_t line = first;; ++line) {
        size_t start, end;
        if (!find_line(core, line, &start, &end))
            return 0;
        start = column_position(core, start, end, left);
        end = column_position(core, start, end, right - left + 1);
        if (end - start > SIZE_MAX - 1 - capacity)
            return 0;
        capacity += end - start;
        if (line == last)
            break;
    }
    char *text = malloc(capacity + 1);
    if (!text)
        return 0;
    size_t length = 0;
    for (size_t line = first;; ++line) {
        size_t start, end;
        find_line(core, line, &start, &end);
        start = column_position(core, start, end, left);
        end = column_position(core, start, end, right - left + 1);
        if (!read_text(core, start, text + length, end - start)) {
            free(text);
            return 0;
        }
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
    core->command_has_range = 0;
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
        VirtualBuffer *file = core->file;
        size_t length = core->length;
        size_t cursor = core->revision_cursor;
        size_t revision = core->revision;
        EditorLineEnding line_ending = core->line_ending;
        int literal_bytes = core->literal_bytes;
        core->text = entry->text;
        core->file = entry->file;
        core->length = entry->length;
        core->capacity = core->file ? 0 : entry->length + 1;
        core->cursor = core->revision_cursor = entry->cursor;
        core->revision = entry->revision;
        core->line_ending = entry->line_ending;
        core->literal_bytes = entry->literal_bytes;
        entry->text = text;
        entry->file = file;
        entry->length = length;
        entry->cursor = cursor;
        entry->revision = revision;
        entry->line_ending = line_ending;
        entry->literal_bytes = literal_bytes;
        entry->next = *destination;
        *destination = entry;
        core->modified = core->revision != core->saved_revision;
        changed = 1;
    }
    if (changed)
        note_changed_lines(core, 0, SIZE_MAX);
    core->status = changed ? EDITOR_STATUS_NONE : EDITOR_STATUS_NO_CHANGE;
    core->history_open = 0;
    core->block_insert = 0;
    clear_replace_history(core);
    core->anchor = core->cursor;
    core->mode = EDITOR_MODE_COMMAND;
    reset_command(core);
    return 1;
}

static EditorLineEnding inspect_line_endings(const char *text, size_t length, int *literal)
{
    int pairs = 0;
    int bare = 0;
    for (size_t index = 0; index < length; ++index) {
        if (text[index] == '\n') {
            if (index && text[index - 1] == '\r')
                pairs = 1;
            else
                bare = 1;
        }
    }
    *literal = pairs && bare;
    return pairs ? EDITOR_LINE_ENDING_CRLF : EDITOR_LINE_ENDING_LF;
}

static size_t normalize_line_endings(char *text, size_t length, size_t *cursor)
{
    size_t count = 0;
    size_t removed = 0;
    for (size_t index = 0; index < length; ++index) {
        if (text[index] == '\r' && index + 1 < length && text[index + 1] == '\n') {
            if (index < *cursor)
                ++removed;
            continue;
        }
        text[count++] = text[index];
    }
    text[count] = 0;
    *cursor -= removed;
    return count;
}

static void reset_document(EditorCore *core)
{
    if (core->file) {
        core->literal_bytes = 1;
        core->line_ending = EDITOR_LINE_ENDING_LF;
        unsigned char bytes[65536];
        size_t count = core->length > sizeof(bytes) ? sizeof(bytes) : core->length;
        if (virtual_buffer_read(core->file, 0, bytes, count)) {
            for (size_t index = 1; index < count; ++index)
                if (bytes[index - 1] == '\r' && bytes[index] == '\n') {
                    core->line_ending = EDITOR_LINE_ENDING_CRLF;
                    break;
                }
        }
    } else {
    core->line_ending = inspect_line_endings(core->text, core->length, &core->literal_bytes);
    if (!core->literal_bytes) {
        size_t cursor = 0;
        core->length = normalize_line_endings(core->text, core->length, &cursor);
    }
    }
    note_changed_lines(core, 0, SIZE_MAX);
    free_history(core->undo);
    free_history(core->redo);
    core->undo = core->redo = NULL;
    core->revision = core->next_revision = core->saved_revision = core->revision_cursor = 0;
    core->history_open = core->history_changed = 0;
    core->change_length = 0;
    core->change_dirty = 0;
    core->buffer_switch.count = 0;
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
    VirtualBuffer *paged = virtual_buffer_create();
    if (!paged)
        return 0;
    if (!virtual_buffer_replace(paged, 0, 0, buffer->data, buffer->length)) {
        virtual_buffer_destroy(paged);
        return 0;
    }
    virtual_buffer_destroy(core->file);
    core->file = paged;
    free(core->text);
    core->text = NULL;
    core->length = buffer->length;
    core->capacity = 0;
    clear_directory(core);
    reset_document(core);
    return 1;
}

static int load_file(EditorCore *core, const char *path)
{
    if (!path || !*path)
        return 0;
    VirtualBuffer *paged = virtual_buffer_open(path);
    if (!paged)
        return 0;
    size_t file_length = virtual_buffer_length(paged);
    virtual_buffer_destroy(core->file);
    core->file = paged;
    free(core->text);
    core->text = NULL;
    core->length = file_length;
    core->capacity = 0;
    clear_directory(core);
    reset_document(core);
    return 1;
}

typedef struct DirectoryListing {
    EditorDirectoryEntry *entries;
    size_t count;
    size_t capacity;
    int failed;
} DirectoryListing;

static int collect_directory_entry(void *context, const EditorDirectoryEntry *entry)
{
    DirectoryListing *listing = context;
    if (listing->failed)
        return 0;
    if (!entry || !entry->name || !*entry->name || !entry->path || !*entry->path ||
        strchr(entry->name, '\n') || strchr(entry->name, '\r')) {
        listing->failed = 1;
        return 0;
    }
    if (listing->count == listing->capacity) {
        if (listing->capacity > SIZE_MAX / sizeof(*listing->entries) / 2) {
            listing->failed = 1;
            return 0;
        }
        size_t capacity = listing->capacity ? listing->capacity * 2 : 32;
        EditorDirectoryEntry *entries = realloc(listing->entries, capacity * sizeof(*entries));
        if (!entries) {
            listing->failed = 1;
            return 0;
        }
        listing->entries = entries;
        listing->capacity = capacity;
    }
    EditorDirectoryEntry copied = { 0 };
    copied.name = copy_string(entry->name);
    copied.path = copy_string(entry->path);
    copied.is_directory = entry->is_directory != 0;
    if (!copied.name || !copied.path) {
        free((void *)copied.name);
        free((void *)copied.path);
        listing->failed = 1;
        return 0;
    }
    listing->entries[listing->count++] = copied;
    return 1;
}

static int compare_directory_entries(const void *left, const void *right)
{
    const EditorDirectoryEntry *left_entry = left;
    const EditorDirectoryEntry *right_entry = right;
    int left_parent = !strcmp(left_entry->name, "..");
    int right_parent = !strcmp(right_entry->name, "..");
    if (left_parent != right_parent)
        return right_parent - left_parent;
    if (left_entry->is_directory != right_entry->is_directory)
        return right_entry->is_directory - left_entry->is_directory;
    return strcmp(left_entry->name, right_entry->name);
}

static int open_directory(EditorCore *core, const char *path)
{
    if (!path || core->modified || !core->directory_functions.list || !core->directory_functions.change)
        return 0;
    char *resolved = NULL;
    if (!core->directory_functions.change(core->directory_functions.context,
        core->directory_active ? core->directory_path : NULL, path, &resolved) ||
        !resolved || !*resolved) {
        free(resolved);
        return 0;
    }
    DirectoryListing listing = { 0 };
    EditorCore document = { 0 };
    int success = core->directory_functions.list(core->directory_functions.context,
        resolved, collect_directory_entry, &listing) && !listing.failed;
    if (success && listing.count > 1)
        qsort(listing.entries, listing.count, sizeof(*listing.entries), compare_directory_entries);
    for (size_t index = 0; success && index < listing.count; ++index) {
        const EditorDirectoryEntry *entry = &listing.entries[index];
        size_t length = strlen(entry->name);
        size_t suffix = entry->is_directory ? 1 : 0;
        size_t separator = index + 1 < listing.count ? 1 : 0;
        if (length > SIZE_MAX - suffix - separator - 1 ||
            document.length > SIZE_MAX - length - suffix - separator - 1 ||
            !reserve_text(&document, document.length + length + suffix + separator)) {
            success = 0;
            break;
        }
        memcpy(document.text + document.length, entry->name, length);
        document.length += length;
        if (suffix)
            document.text[document.length++] = '/';
        if (separator)
            document.text[document.length++] = '\n';
    }
    EditorBuffer buffer = { (const unsigned char *)document.text, document.length };
    if (success)
        success = set_buffer(core, &buffer);
    free(document.text);
    if (!success) {
        for (size_t index = 0; index < listing.count; ++index) {
            free((void *)listing.entries[index].name);
            free((void *)listing.entries[index].path);
        }
        free(listing.entries);
        free(resolved);
        return 0;
    }
    core->directory_entries = listing.entries;
    core->directory_count = listing.count;
    core->directory_path = resolved;
    core->directory_active = 1;
    core->directory_changed = 1;
    core->lexer = NULL;
    return 1;
}

static int activate_directory_entry(EditorCore *core)
{
    if (!core->directory_active)
        return 0;
    size_t line = position_line(core, core->cursor);
    if (line >= core->directory_count)
        return 1;
    const EditorDirectoryEntry *entry = &core->directory_entries[line];
    if (entry->is_directory)
        return open_directory(core, entry->path);
    char *path = copy_string(entry->path);
    if (!path)
        return 0;
    if (!load_file(core, path)) {
        free(path);
        return 0;
    }
    core->directory_path = path;
    core->directory_changed = 1;
    return select_lexer(core, path);
}

static int serialize_buffer(EditorCore *core, EditorBuffer *buffer)
{
    if (core->file) {
        buffer->data = NULL;
        buffer->length = 0;
        if (core->length >= 65536)
            return 0;
        const char *text = text_span(core, 0, core->length);
        if (!text)
            return 0;
        buffer->data = (const unsigned char *)text;
        buffer->length = core->length;
        return 1;
    }
    buffer->data = (const unsigned char *)core->text;
    buffer->length = core->length;
    if (core->literal_bytes || core->line_ending == EDITOR_LINE_ENDING_LF)
        return 1;
    size_t newlines = 0;
    for (size_t index = 0; index < core->length; ++index)
        if (text_byte(core, index) == '\n')
            ++newlines;
    if (newlines >= SIZE_MAX - core->length)
        return 0;
    size_t length = core->length + newlines;
    if (core->serialized_capacity < length + 1) {
        char *text = realloc(core->serialized_text, length + 1);
        if (!text)
            return 0;
        core->serialized_text = text;
        core->serialized_capacity = length + 1;
    }
    size_t position = 0;
    for (size_t index = 0; index < core->length; ++index) {
        if (text_byte(core, index) == '\n')
            core->serialized_text[position++] = '\r';
        core->serialized_text[position++] = text_byte(core, index);
    }
    core->serialized_text[position] = 0;
    buffer->data = (const unsigned char *)core->serialized_text;
    buffer->length = length;
    return 1;
}

static int save_file(EditorCore *core, const char *path)
{
    if (!path || !*path)
        return 0;
    if (core->file) {
        if (!virtual_buffer_detach(core->file))
            return 0;
        for (HistoryEntry *entry = core->undo; entry; entry = entry->next)
            if (!virtual_buffer_detach(entry->file))
                return 0;
        for (HistoryEntry *entry = core->redo; entry; entry = entry->next)
            if (!virtual_buffer_detach(entry->file))
                return 0;
        FILE *stream = fopen(path, "wb");
        if (!stream)
            return 0;
        int success = virtual_buffer_write(core->file, stream, 0, core->length);
        if (fclose(stream))
            success = 0;
        if (success) {
            core->modified = 0;
            core->saved_revision = core->revision;
            core->history_open = 0;
        }
        return success;
    }
    EditorBuffer buffer;
    if (!serialize_buffer(core, &buffer))
        return 0;
    FILE *file = fopen(path, "wb");
    if (!file)
        return 0;
    int success = fwrite(buffer.data, 1, buffer.length, file) == buffer.length;
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
    int append_newline = range.linewise && (!length || text_byte(core, range.end - 1) != '\n');
    if (length > SIZE_MAX - 1 - append_newline)
        return 0;
    char *text = malloc(length + append_newline + 1);
    if (!text)
        return 0;
    if (!read_text(core, range.start, text, length)) {
        free(text);
        return 0;
    }
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
    unsigned char byte = (unsigned char)text_byte(core, position);
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

static size_t word_end(const EditorCore *core, size_t position)
{
    size_t original = position;
    if (position == core->length)
        return position;
    position = next_character(core, position);
    while (position < core->length && !word_class(core, position))
        position = next_character(core, position);
    if (position == core->length)
        return original;
    int category = word_class(core, position);
    while (next_character(core, position) < core->length &&
        word_class(core, next_character(core, position)) == category)
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

static int search_matches(const EditorCore *core, size_t position)
{
    if (core->search_length > core->length - position ||
        !text_equal(core, position, core->search_text, core->search_length))
        return 0;
    if (!core->search_whole_word)
        return 1;
    size_t end = position + core->search_length;
    int category = word_class(core, position);
    return (!position || word_class(core, previous_character(core, position)) != category) &&
        (end == core->length || word_class(core, end) != category);
}

static int repeat_search(EditorCore *core, size_t count, int backwards)
{
    if (!core->search_length) {
        core->status = EDITOR_STATUS_NO_PREVIOUS_SEARCH;
        return 0;
    }
    if (count == 1) {
        size_t position = core->cursor;
        if (backwards) {
            while (position) {
                position = previous_character(core, position);
                if (search_matches(core, position))
                    goto located;
            }
            position = core->length;
            while (position > core->cursor) {
                position = previous_character(core, position);
                if (position < core->cursor)
                    break;
                if (search_matches(core, position))
                    goto located;
            }
        } else {
            for (position = next_character(core, core->cursor); position < core->length;
                position = next_character(core, position))
                if (search_matches(core, position))
                    goto located;
            for (position = 0; position <= core->cursor && position < core->length;
                position = next_character(core, position))
                if (search_matches(core, position))
                    goto located;
        }
        core->status = EDITOR_STATUS_PATTERN_NOT_FOUND;
        return 0;
located:
        core->cursor = position;
        core->status = EDITOR_STATUS_NONE;
        return 1;
    }
    size_t matches = 0;
    size_t first = SIZE_MAX;
    for (size_t position = 0; position < core->length; position = next_character(core, position)) {
        if (search_matches(core, position)) {
            if ((backwards && position < core->cursor) ||
                (!backwards && first == SIZE_MAX && position > core->cursor))
                first = matches;
            ++matches;
        }
    }
    if (!matches) {
        core->status = EDITOR_STATUS_PATTERN_NOT_FOUND;
        return 0;
    }
    if (first == SIZE_MAX)
        first = backwards ? matches - 1 : 0;
    size_t offset = (count - 1) % matches;
    size_t target = backwards ? (offset > first ? matches - (offset - first) : first - offset) :
        (offset >= matches - first ? offset - (matches - first) : first + offset);
    for (size_t position = 0; position < core->length; position = next_character(core, position)) {
        if (search_matches(core, position)) {
            if (!target--) {
                core->cursor = position;
                core->status = EDITOR_STATUS_NONE;
                return 1;
            }
        }
    }
    return 0;
}

static int start_search(EditorCore *core, const char *text)
{
    size_t length = strlen(text);
    if (length) {
        char *copy = malloc(length + 1);
        if (!copy)
            return 0;
        memcpy(copy, text, length + 1);
        free(core->search_text);
        core->search_text = copy;
        core->search_length = length;
        core->search_whole_word = 0;
    }
    return repeat_search(core, 1, 0);
}

static int search_word(EditorCore *core, size_t count)
{
    size_t start = core->cursor;
    size_t end = line_end(core, start);
    while (start < end && !word_class(core, start))
        start = next_character(core, start);
    if (start == end)
        return 1;
    int category = word_class(core, start);
    size_t line = line_start(core, start);
    while (start > line &&
        word_class(core, previous_character(core, start)) == category)
        start = previous_character(core, start);
    end = start;
    while (end < core->length && word_class(core, end) == category)
        end = next_character(core, end);
    size_t length = end - start;
    char *text = malloc(length + 1);
    if (!text)
        return 0;
    if (!read_text(core, start, text, length)) {
        free(text);
        return 0;
    }
    text[length] = 0;
    free(core->search_text);
    core->search_text = text;
    core->search_length = length;
    core->search_whole_word = 1;
    core->cursor = start;
    return repeat_search(core, count, 0);
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
    case '*': return search_word(core, count);
    case 'n': case 'N': return repeat_search(core, count, key == 'N');
    case 'h': case EDITOR_KEY_LEFT: message = EDITOR_MOVE_LEFT; break;
    case 'l': case EDITOR_KEY_RIGHT: message = EDITOR_MOVE_RIGHT; break;
    case 'j': case EDITOR_KEY_DOWN: message = EDITOR_MOVE_DOWN; break;
    case 'k': case EDITOR_KEY_UP: message = EDITOR_MOVE_UP; break;
    case '0': case EDITOR_KEY_HOME: message = EDITOR_MOVE_HOME; break;
    case '^': {
        core->cursor = line_start(core, core->cursor);
        size_t end = line_end(core, core->cursor);
        while (core->cursor < end &&
            (text_byte(core, core->cursor) == ' ' || text_byte(core, core->cursor) == '\t'))
            core->cursor = next_character(core, core->cursor);
        return 1;
    }
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
    case 'w': case 'b': case 'e':
        while (count--) {
            size_t previous = core->cursor;
            core->cursor = key == 'w' ? word_forward(core, previous) :
                (key == 'b' ? word_backward(core, previous) : word_end(core, previous));
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
    if (!write_span(core, range.start, range.end, text, length)) {
        free(text);
        return 0;
    }
    core->cursor = core->register_linewise || range.linewise ? range.start + prefix :
        previous_character(core, range.start + length);
    core->mode = EDITOR_MODE_COMMAND;
    free(text);
    return 1;
}

static int write_span(EditorCore *core, size_t start, size_t end, const char *text, size_t length)
{
    if (core->file) {
        if (start > end || end > core->length)
            return 0;
        if (length == end - start && text_equal(core, start, text, length))
            return 1;
        if (!prepare_edit(core) || !virtual_buffer_replace(core->file, start, end - start, text, length))
            return 0;
        core->length = virtual_buffer_length(core->file);
        core->modified = 1;
        note_changed_lines(core, 0, SIZE_MAX);
        return 1;
    }
    size_t remaining = core->length - (end - start);
    if (length > SIZE_MAX - 1 - remaining || !reserve_text(core, remaining + length))
        return 0;
    if (length != end - start || (length && memcmp(core->text + start, text, length) != 0)) {
        if (!prepare_edit(core))
            return 0;
        note_change(core, start, end, text, length);
        core->modified = 1;
    }
    memmove(core->text + start + length, core->text + end, core->length - end + 1);
    if (length)
        memcpy(core->text + start, text, length);
    core->length = remaining + length;
    return 1;
}

static int replace_bytes(EditorCore *core, const EditorByteEdit *edit)
{
    if (core->file) {
        if (!edit || (edit->length && !edit->data) || edit->offset > core->length ||
            edit->remove_length > core->length - edit->offset ||
            !write_span(core, edit->offset, edit->offset + edit->remove_length,
                (const char *)edit->data, edit->length))
            return 0;
        core->cursor = edit->offset;
        core->anchor = core->cursor;
        core->mode = EDITOR_MODE_COMMAND;
        reset_command(core);
        return 1;
    }
    EditorBuffer buffer;
    if (!edit || (edit->length && !edit->data) || !serialize_buffer(core, &buffer) ||
        edit->offset > buffer.length || edit->remove_length > buffer.length - edit->offset)
        return 0;
    size_t remaining = buffer.length - edit->remove_length;
    if (edit->length > SIZE_MAX - 1 - remaining)
        return 0;
    if (edit->length == edit->remove_length &&
        (!edit->length || !memcmp(buffer.data + edit->offset, edit->data, edit->length)))
        return 1;
    size_t length = remaining + edit->length;
    char *text = malloc(length + 1);
    if (!text)
        return 0;
    memcpy(text, buffer.data, edit->offset);
    if (edit->length)
        memcpy(text + edit->offset, edit->data, edit->length);
    memcpy(text + edit->offset + edit->length, buffer.data + edit->offset + edit->remove_length,
        buffer.length - edit->offset - edit->remove_length);
    text[length] = 0;
    int literal;
    EditorLineEnding ending = inspect_line_endings(text, length, &literal);
    size_t cursor = edit->offset;
    if (!literal)
        length = normalize_line_endings(text, length, &cursor);
    int success = reserve_text(core, length) && prepare_edit(core) &&
        write_span(core, 0, core->length, text, length);
    free(text);
    if (!success)
        return 0;
    core->literal_bytes = literal;
    core->line_ending = ending;
    core->modified = 1;
    core->cursor = cursor;
    core->anchor = core->cursor;
    core->mode = EDITOR_MODE_COMMAND;
    core->status = EDITOR_STATUS_NONE;
    clear_replace_history(core);
    reset_command(core);
    return 1;
}

static size_t tab_spaces(const EditorCore *core)
{
    size_t column = advance_columns(core, 0, line_start(core, core->cursor), core->cursor);
    return core->settings.tabwidth - column % core->settings.tabwidth;
}

typedef struct CIndentFrame {
    char opener;
    size_t base;
    size_t body;
    int aligned;
    int switch_block;
    int case_active;
} CIndentFrame;

static int c_indent_identifier(unsigned char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || value == '_' || value >= 128;
}

static size_t c_indent_write(char *text, size_t column, const EditorSettings *settings)
{
    size_t tabs = settings->expandtabs ? 0 : column / settings->tabwidth;
    size_t spaces = settings->expandtabs ? column : column % settings->tabwidth;
    memset(text, '\t', tabs);
    memset(text + tabs, ' ', spaces);
    return tabs + spaces;
}

static TextRange whole_lines(const EditorCore *core, TextRange range)
{
    size_t last = range.end > range.start ? range.end - 1 : range.start;
    range.start = line_start(core, range.start);
    range.end = line_end(core, last);
    if (range.end < core->length)
        ++range.end;
    range.linewise = 1;
    return range;
}

static int blank_text_line(const EditorCore *core, size_t start)
{
    size_t end = line_end(core, start);
    while (start < end && (text_byte(core, start) == ' ' || text_byte(core, start) == '\t'))
        ++start;
    return start == end;
}

static TextRange paragraph_lines(const EditorCore *core, size_t count, int around)
{
    size_t start = line_start(core, core->cursor);
    int blank = blank_text_line(core, start);
    while (start && blank_text_line(core, line_start(core, start - 1)) == blank)
        start = line_start(core, start - 1);
    size_t end = start;
    if (blank) {
        while (end < core->length && blank_text_line(core, end)) {
            size_t next = line_end(core, end);
            end = next < core->length ? next + 1 : next;
        }
        if (!around) {
            TextRange range = { start, end, 1 };
            return range;
        }
    }
    while (count-- && end < core->length) {
        while (end < core->length && !blank_text_line(core, end)) {
            size_t next = line_end(core, end);
            end = next < core->length ? next + 1 : next;
        }
        if (count || around) {
            while (end < core->length && blank_text_line(core, end)) {
                size_t next = line_end(core, end);
                end = next < core->length ? next + 1 : next;
            }
        }
    }
    TextRange range = { start, end, 1 };
    return range;
}

static int write_stream_span(EditorCore *core, size_t start, size_t end, FILE *stream, size_t length)
{
    if (start > end || end > core->length || fflush(stream) || fseek(stream, 0, SEEK_SET))
        return 0;
    int equal = length == end - start;
    unsigned char bytes[65536];
    for (size_t offset = 0; equal && offset < length;) {
        size_t count = length - offset;
        if (count > sizeof(bytes))
            count = sizeof(bytes);
        if (fread(bytes, 1, count, stream) != count)
            return 0;
        equal = text_equal(core, start + offset, (const char *)bytes, count);
        offset += count;
    }
    if (equal)
        return 1;
    if (fseek(stream, 0, SEEK_SET) || !prepare_edit(core) ||
        !virtual_buffer_replace_stream(core->file, start, end - start, stream, length))
        return 0;
    core->length = virtual_buffer_length(core->file);
    core->modified = 1;
    note_changed_lines(core, 0, SIZE_MAX);
    return 1;
}

static int append_edit_text(EditorCore *output, const char *text, size_t length)
{
    if (output->output_stream) {
        if (length > SIZE_MAX - 1 - output->length)
            return 0;
        while (length) {
            size_t count = length > 65536 ? 65536 : length;
            if (fwrite(text, 1, count, output->output_stream) != count)
                return 0;
            output->length += count;
            text += count;
            length -= count;
        }
        return 1;
    }
    if (length > SIZE_MAX - 1 - output->length || !reserve_text(output, output->length + length))
        return 0;
    if (length)
        memcpy(output->text + output->length, text, length);
    output->length += length;
    output->text[output->length] = 0;
    return 1;
}

static int append_core_text(EditorCore *output, const EditorCore *core, size_t start, size_t length)
{
    char bytes[65536];
    while (length) {
        size_t count = length > sizeof(bytes) ? sizeof(bytes) : length;
        if (!read_text(core, start, bytes, count) || !append_edit_text(output, bytes, count))
            return 0;
        start += count;
        length -= count;
    }
    return 1;
}

static int same_ranges(const EditorCore *core, size_t first, size_t second, size_t length)
{
    for (size_t offset = 0; offset < length; ++offset)
        if (text_byte(core, first + offset) != text_byte(core, second + offset))
            return 0;
    return 1;
}

static int apply_line_edit(EditorCore *core, TextRange range, EditorCore *output)
{
    int success = output->output_stream ?
        write_stream_span(core, range.start, range.end, output->output_stream, output->length) :
        write_span(core, range.start, range.end, output->text, output->length);
    if (output->output_stream)
        fclose(output->output_stream);
    free(output->text);
    if (!success)
        return 0;
    core->cursor = range.start;
    while (core->cursor < core->length &&
        (text_byte(core, core->cursor) == ' ' || text_byte(core, core->cursor) == '\t'))
        ++core->cursor;
    core->anchor = core->cursor;
    core->block_insert = 0;
    core->mode = EDITOR_MODE_COMMAND;
    return 1;
}

static int shift_lines(EditorCore *core, TextRange range, int right, size_t steps)
{
    range = whole_lines(core, range);
    if (steps > SIZE_MAX / core->settings.tabwidth)
        return 0;
    size_t distance = steps * core->settings.tabwidth;
    EditorCore output = { 0 };
    output.output_stream = tmpfile();
    if (!output.output_stream)
        return 0;
    for (size_t start = range.start; start < range.end;) {
        size_t end = line_end(core, start);
        size_t first = start;
        while (first < end && (text_byte(core, first) == ' ' || text_byte(core, first) == '\t'))
            ++first;
        size_t next = end < range.end ? end + 1 : end;
        if (first < end) {
            size_t column = advance_columns(core, 0, start, first);
            if (right && distance > SIZE_MAX - column)
                goto failure;
            column = right ? column + distance : distance > column ? 0 : column - distance;
            size_t bytes = core->settings.expandtabs ? column :
                column / core->settings.tabwidth + column % core->settings.tabwidth;
            for (size_t index = 0; index < bytes; ++index) {
                char value = !core->settings.expandtabs && index < column / core->settings.tabwidth ? '\t' : ' ';
                if (!append_edit_text(&output, &value, 1))
                    goto failure;
            }
            if (!append_core_text(&output, core, first, next - first))
                goto failure;
        } else if (!append_core_text(&output, core, start, next - start)) {
            goto failure;
        }
        start = next;
    }
    return apply_line_edit(core, range, &output);
failure:
    fclose(output.output_stream);
    free(output.text);
    return 0;
}

static int format_lines(EditorCore *core, TextRange range)
{
    if (!core->settings.maxcol) {
        core->status = EDITOR_STATUS_NO_CHANGE;
        core->mode = EDITOR_MODE_COMMAND;
        return 1;
    }
    range = whole_lines(core, range);
    EditorCore output = { 0 };
    output.output_stream = tmpfile();
    if (!output.output_stream)
        return 0;
    int paragraph = 0, has_word = 0;
    size_t prefix = 0, prefix_length = 0, column = 0;
    size_t leader = 0, leader_length = 0;
    for (size_t start = range.start; start < range.end;) {
        size_t end = line_end(core, start);
        size_t first = start;
        while (first < end && (text_byte(core, first) == ' ' || text_byte(core, first) == '\t'))
            ++first;
        size_t body = first;
        size_t current_leader = 0;
        if (core->lexer == &lexer_c_cpp && end - first >= 2 &&
            text_byte(core, first) == '/' && text_byte(core, first + 1) == '/') {
            while (body < end && text_byte(core, body) == '/')
                ++body;
            if (body < end && text_byte(core, body) == '!')
                ++body;
            current_leader = body - first;
            while (body < end && (text_byte(core, body) == ' ' || text_byte(core, body) == '\t'))
                ++body;
        }
        size_t next = end < range.end ? end + 1 : end;
        if (body == end) {
            if (paragraph && !append_edit_text(&output, "\n", 1))
                goto failure;
            if (!append_core_text(&output, core, start, next - start))
                goto failure;
            paragraph = 0;
            start = next;
            continue;
        }
        if (paragraph && (current_leader != leader_length ||
            (leader_length && !same_ranges(core, first, leader, leader_length)))) {
            if (!append_edit_text(&output, "\n", 1))
                goto failure;
            paragraph = 0;
        }
        if (!paragraph) {
            prefix = start;
            prefix_length = body - start;
            leader = first;
            leader_length = current_leader;
            if (!append_core_text(&output, core, prefix, prefix_length))
                goto failure;
            column = advance_columns(core, 0, start, body);
            has_word = 0;
            paragraph = 1;
        }
        for (size_t position = body; position < end;) {
            if (text_byte(core, position) == ' ' || text_byte(core, position) == '\t') {
                ++position;
                continue;
            }
            size_t word_end = position;
            while (word_end < end && text_byte(core, word_end) != ' ' && text_byte(core, word_end) != '\t')
                word_end = next_character(core, word_end);
            size_t width = advance_columns(core, 0, position, word_end);
            if (has_word && (column >= core->settings.maxcol ||
                width > core->settings.maxcol - column - 1)) {
                if (!append_edit_text(&output, "\n", 1) ||
                    !append_core_text(&output, core, prefix, prefix_length))
                    goto failure;
                column = advance_columns(core, 0, prefix, prefix + prefix_length);
                has_word = 0;
            }
            if (has_word) {
                if (column == SIZE_MAX || !append_edit_text(&output, " ", 1))
                    goto failure;
                ++column;
            }
            if (width > SIZE_MAX - column ||
                !append_core_text(&output, core, position, word_end - position))
                goto failure;
            column += width;
            has_word = 1;
            position = word_end;
        }
        start = next;
    }
    if (paragraph && range.end > range.start && text_byte(core, range.end - 1) == '\n' &&
        !append_edit_text(&output, "\n", 1))
        goto failure;
    return apply_line_edit(core, range, &output);
failure:
    fclose(output.output_stream);
    free(output.text);
    return 0;
}

static int c_indent_newline(EditorCore *core, size_t *replacement_start, char **text, size_t *length)
{
    CIndentFrame *frames = NULL;
    size_t depth = 0, capacity = 0;
    size_t fallback = 0, switch_base = 0;
    int switch_pending = 0;
    size_t current_start = line_start(core, core->cursor);
    size_t current_first = current_start;
    while (current_first < core->cursor &&
        (text_byte(core, current_first) == ' ' || text_byte(core, current_first) == '\t'))
        ++current_first;
    size_t current_indent = advance_columns(core, 0, current_start, current_first);
    size_t corrected_indent = current_indent;
    LexerCCppState state = { 0 };
    for (size_t start = 0; start <= core->cursor;) {
        size_t end = line_end(core, start);
        if (end > core->cursor)
            end = core->cursor;
        size_t first = start;
        while (first < end && (text_byte(core, first) == ' ' || text_byte(core, first) == '\t'))
            ++first;
        size_t original_indent = advance_columns(core, 0, start, first);
        size_t indent = original_indent;
        size_t column = 0, measured = start;
        int seen_code = 0;
        LexerCCppLine parsed = { 0 };
        const char *subject = text_span(core, start, end - start);
        if (!subject || !lexer_c_cpp_line(subject, end - start, state, &parsed)) {
            free(frames);
            return 0;
        }
        state = parsed.end_state;
        for (size_t token_index = 0; token_index < parsed.count; ++token_index) {
            const LexerCCppToken *token = &parsed.tokens[token_index];
            if (token->kind == LEXER_C_CPP_COMMENT || token->kind == LEXER_C_CPP_PREPROCESSOR)
                continue;
            size_t token_end = start + token->start + token->length;
            for (size_t position = start + token->start; position < token_end;) {
                unsigned char value = (unsigned char)text_byte(core, position);
                if (value == ' ' || value == '\t' || value == '\r') {
                    ++position;
                    continue;
                }
                column = advance_columns(core, column, measured, position);
                measured = position;
                size_t next = next_character(core, position);
                int literal = token->kind == LEXER_C_CPP_STRING;
                if (literal)
                    next = token_end;
                else if (c_indent_identifier(value))
                    while (next < token_end && c_indent_identifier((unsigned char)text_byte(core, next)))
                        ++next;
                size_t word_length = next - position;
                int case_label = !literal && !seen_code &&
                    ((word_length == 4 && text_equal(core, position, "case", 4)) ||
                        (word_length == 7 && text_equal(core, position, "default", 7)));
                int closing = !literal && (value == ')' || value == ']' || value == '}');
                if (case_label) {
                    for (size_t index = depth; index > 0; --index) {
                        CIndentFrame *frame = &frames[index - 1];
                        if (frame->opener == '{' && frame->switch_block) {
                            frame->case_active = 1;
                            indent = frame->base;
                            break;
                        }
                    }
                }
                if (closing) {
                    char opener = value == ')' ? '(' : value == ']' ? '[' : '{';
                    for (size_t index = depth; index > 0; --index) {
                        CIndentFrame closed = frames[index - 1];
                        if (closed.opener != opener)
                            continue;
                        depth = index - 1;
                        fallback = closed.base;
                        if (!seen_code)
                            indent = closed.base;
                        if (closed.switch_block && opener == '(') {
                            switch_pending = 1;
                            switch_base = closed.base;
                        }
                        break;
                    }
                } else {
                    if (column >= original_indent && indent > SIZE_MAX - (column - original_indent)) {
                        lexer_c_cpp_free_line(&parsed);
                        free(frames);
                        return 0;
                    }
                    size_t adjusted_column = column >= original_indent ?
                        column - original_indent + indent : column;
                    if (depth && !case_label && (!frames[depth - 1].aligned || !seen_code) &&
                        value != ',' && value != ';' && value != ':') {
                        frames[depth - 1].body = adjusted_column;
                        frames[depth - 1].aligned = 1;
                    }
                    if (!literal && word_length == 6 && text_equal(core, position, "switch", 6)) {
                        switch_pending = 1;
                        switch_base = indent;
                    }
                    if (!literal && (value == '(' || value == '[' || value == '{')) {
                        if (depth == capacity) {
                            size_t grown = capacity ? capacity * 2 : 16;
                            if (grown < capacity || grown > SIZE_MAX / sizeof(*frames)) {
                                lexer_c_cpp_free_line(&parsed);
                                free(frames);
                                return 0;
                            }
                            CIndentFrame *replacement = realloc(frames, grown * sizeof(*frames));
                            if (!replacement) {
                                lexer_c_cpp_free_line(&parsed);
                                free(frames);
                                return 0;
                            }
                            frames = replacement;
                            capacity = grown;
                        }
                        CIndentFrame frame = { 0 };
                        frame.opener = (char)value;
                        frame.switch_block = switch_pending && (value == '(' || value == '{');
                        frame.base = frame.switch_block ? switch_base : indent;
                        frames[depth++] = frame;
                        switch_pending = 0;
                    }
                    if (!literal && value == ';')
                        switch_pending = 0;
                    if (!seen_code && !depth)
                        fallback = indent;
                }
                seen_code = 1;
                position = next;
            }
        }
        lexer_c_cpp_free_line(&parsed);
        if (start == current_start)
            corrected_indent = indent;
        if (end == core->cursor)
            break;
        start = end + 1;
    }
    size_t next_indent = fallback;
    if (depth) {
        const CIndentFrame *frame = &frames[depth - 1];
        if (frame->opener == '{' && frame->switch_block && !frame->case_active) {
            next_indent = frame->base;
        } else if (frame->aligned && !(frame->opener == '{' && frame->switch_block)) {
            next_indent = frame->body;
        } else {
            if (frame->base > SIZE_MAX - core->settings.tabwidth) {
                free(frames);
                return 0;
            }
            next_indent = frame->base + core->settings.tabwidth;
        }
    } else if (current_first == core->cursor && current_indent) {
        next_indent = current_indent;
    }
    free(frames);
    int rewrite = corrected_indent != current_indent ||
        (current_first == core->cursor && current_first > current_start);
    size_t prefix_column = current_first == core->cursor ? 0 : corrected_indent;
    size_t prefix_bytes = !rewrite ? 0 : core->settings.expandtabs ? prefix_column :
        prefix_column / core->settings.tabwidth + prefix_column % core->settings.tabwidth;
    size_t content_bytes = rewrite ? core->cursor - current_first : 0;
    size_t indent_bytes = core->settings.expandtabs ? next_indent :
        next_indent / core->settings.tabwidth + next_indent % core->settings.tabwidth;
    if (prefix_bytes > SIZE_MAX - content_bytes || prefix_bytes + content_bytes > SIZE_MAX - 2 ||
        indent_bytes > SIZE_MAX - 2 - prefix_bytes - content_bytes)
        return 0;
    *length = prefix_bytes + content_bytes + 1 + indent_bytes;
    *text = malloc(*length + 1);
    if (!*text)
        return 0;
    size_t offset = 0;
    if (rewrite) {
        offset = c_indent_write(*text, prefix_column, &core->settings);
        if (!read_text(core, current_first, *text + offset, content_bytes)) {
            free(*text);
            return 0;
        }
        offset += content_bytes;
    }
    (*text)[offset++] = '\n';
    offset += c_indent_write(*text + offset, next_indent, &core->settings);
    (*text)[offset] = 0;
    *replacement_start = rewrite ? current_start : core->cursor;
    return 1;
}

static int insert_newline(EditorCore *core)
{
    if (core->lexer != &lexer_c_cpp)
        return editor_core_send(core, EDITOR_ADD_CHAR, "\n");
    size_t start, length;
    char *text;
    if (!c_indent_newline(core, &start, &text, &length))
        return 0;
    int success = write_span(core, start, core->cursor, text, length);
    free(text);
    if (success)
        core->cursor = start + length;
    return success;
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
    size_t start = core->cursor;
    if ((key == '\r' || key == '\n') && core->lexer == &lexer_c_cpp) {
        if (!c_indent_newline(core, &start, &expanded, &length))
            return 0;
        text = expanded;
    } else if (key == '\t' && core->settings.expandtabs) {
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
    edit.position = start;
    edit.cursor = core->cursor;
    edit.original_length = end - start;
    edit.inserted_length = length;
    edit.original = malloc(edit.original_length ? edit.original_length : 1);
    if (!edit.original) {
        free(expanded);
        return 0;
    }
    if (!read_text(core, start, edit.original, edit.original_length)) {
        free(edit.original);
        free(expanded);
        return 0;
    }
    if (!write_span(core, start, end, text, length)) {
        free(edit.original);
        free(expanded);
        return 0;
    }
    free(expanded);
    core->replace_edits[core->replace_count++] = edit;
    core->cursor = start + length;
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
        if (text_byte(core, position) == '\n') {
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
        int newline = text_byte(core, position) == '\n';
        int replace = !newline && (!block || (line <= last && column >= left && column <= right));
        if (replace) {
            if (!positioned) {
                cursor = start + length;
                positioned = 1;
            }
            memcpy(text + length, encoded, width);
            length += width;
        } else {
            if (!read_text(core, position, text + length, next - position)) {
                free(text);
                return 0;
            }
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

static int repeat_change(EditorCore *core, size_t count, int explicit_count);

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
    if (!key || key > EDITOR_KEY_PAGE_DOWN ||
        (key > 0x10FFFF && key < EDITOR_KEY_DELETE) || (key >= 0xD800 && key <= 0xDFFF))
        return 0;
    if (core->mode == EDITOR_MODE_COMMAND_LINE)
        return 1;
    if (key == EDITOR_KEY_PAGE_UP || key == EDITOR_KEY_PAGE_DOWN ||
        ((core->mode == EDITOR_MODE_COMMAND || is_visual(core)) &&
            (key == 2 || key == 6 || key == 4 || key == 21))) {
        EditorCaret caret;
        size_t lines;
        editor_core_send(core, EDITOR_GET_CARET, &caret);
        editor_core_send(core, EDITOR_GET_LINE_COUNT, &lines);
        size_t rows = key == 4 || key == 21 ? core->page_lines / 2 :
            core->page_lines > 2 ? core->page_lines - 2 : 1;
        if (!rows)
            rows = 1;
        size_t count = core->count ? core->count : 1;
        size_t distance = count > SIZE_MAX / rows ? SIZE_MAX : rows * count;
        if (key == EDITOR_KEY_PAGE_UP || key == 2 || key == 21)
            caret.line = distance > caret.line ? 0 : caret.line - distance;
        else
            caret.line += distance > lines - 1 - caret.line ? lines - 1 - caret.line : distance;
        reset_command(core);
        return editor_core_send(core, EDITOR_SET_CARET, &caret);
    }
    if (core->mode == EDITOR_MODE_REPLACE) {
        if (key == '\b') {
            if (!core->replace_count)
                return 1;
            ReplaceEdit *edit = &core->replace_edits[core->replace_count - 1];
            if (!write_span(core, edit->position, edit->position + edit->inserted_length,
                edit->original, edit->original_length))
                return 0;
            core->cursor = edit->cursor;
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
        case '\n': return insert_newline(core);
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
            if (key == 'q') {
                if (is_visual(core))
                    return format_lines(core, selected_range(core));
                core->pending = 'q';
                core->operator_count = count;
                core->operator_has_count = motion_counted;
                return 1;
            }
            if ((key == 't' || key == 'T') && core->mode == EDITOR_MODE_COMMAND) {
                core->buffer_switch.count = count;
                core->buffer_switch.backwards = key == 'T';
                core->buffer_switch.absolute = key == 't' && motion_counted;
            }
            if (key == 'g')
                goto_line(core, count);
            if (core->mode == EDITOR_MODE_VISUAL_BLOCK) {
                size_t start = line_start(core, core->cursor);
                core->cursor = column_position(core, start, line_end(core, start), core->block_column);
            }
            return 1;
        }
        if (pending == '!' || pending == '@') {
            if (key == 'p')
                return format_lines(core, paragraph_lines(core, count, pending == '@'));
            return 1;
        }
        if (pending == 'q' || pending == 'Q') {
            if (pending == 'q' && key == 'q')
                return format_lines(core, counted_lines(core, count));
            if (pending == 'q' && (key == 'i' || key == 'a')) {
                core->pending = key == 'i' ? '!' : '@';
                core->operator_count = count;
                core->operator_has_count = motion_counted;
                return 1;
            }
            if (pending == 'q' && key == 'g') {
                core->pending = 'Q';
                core->operator_count = count;
                core->operator_has_count = motion_counted;
                return 1;
            }
            size_t original = core->cursor;
            if (pending == 'Q') {
                if (key != 'g')
                    return 1;
                goto_line(core, count);
            } else if (!move_command(core, key, count, motion_counted)) {
                return 1;
            }
            size_t first = original < core->cursor ? original : core->cursor;
            size_t last = original > core->cursor ? original : core->cursor;
            int inclusive = pending == 'Q' || key == 'e' || key == 'j' || key == 'k' ||
                key == 'G' || key == EDITOR_KEY_UP || key == EDITOR_KEY_DOWN;
            TextRange range = { first, inclusive ? next_character(core, last) : last, 1 };
            core->cursor = original;
            return format_lines(core, range);
        }
        if (key == pending)
            return use_range(core, counted_lines(core, count), pending == 'd');
        size_t original = core->cursor;
        if (!move_command(core, key, count, motion_counted))
            return 1;
        TextRange range = { original < core->cursor ? original : core->cursor,
            original > core->cursor ? original : core->cursor, 0 };
        if (key == 'e')
            range.end = next_character(core, range.end);
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
    if ((key == ':' && (core->mode == EDITOR_MODE_COMMAND || is_visual(core))) ||
        (key == '/' && core->mode == EDITOR_MODE_COMMAND)) {
        core->command_has_range = key == ':' && is_visual(core);
        if (core->command_has_range) {
            size_t first = core->anchor < core->cursor ? core->anchor : core->cursor;
            size_t last = core->anchor > core->cursor ? core->anchor : core->cursor;
            core->command_range_start = line_start(core, first);
            core->command_range_end = line_end(core, last);
        }
        core->command_prefix = (char)key;
        core->mode = EDITOR_MODE_COMMAND_LINE;
        return 1;
    }
    if (key == 'u' || key == 18)
        return step_history(core, count, key == 18);
    if (key == '.' && core->mode == EDITOR_MODE_COMMAND)
        return repeat_change(core, count, explicit_count);
    if (key == '<' || key == '>') {
        int visual = is_visual(core);
        TextRange range = visual ? selected_range(core) : counted_lines(core, count);
        return shift_lines(core, range, key == '>', visual ? count : 1);
    }
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
    if ((key == 'i' || key == 'a' || key == 'A') && !is_visual(core)) {
        if (key == 'A')
            core->cursor = line_end(core, core->cursor);
        else if (key == 'a' && core->cursor < line_end(core, core->cursor))
            core->cursor = next_character(core, core->cursor);
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

static int repeat_change(EditorCore *core, size_t count, int explicit_count)
{
    if (!core->last_change_length) {
        core->status = EDITOR_STATUS_NO_CHANGE;
        return 1;
    }
    size_t override = explicit_count ? count : core->repeat_count;
    size_t first = 0;
    if (override) {
        while (first < core->last_change_length &&
            core->last_change[first] >= '0' && core->last_change[first] <= '9')
            ++first;
    }
    unsigned long command = core->last_change[first];
    int session = command == 'i' || command == 'a' || command == 'A' || command == 'R';
    if (session && override && core->last_change[core->last_change_length - 1] == 27) {
        if (!handle_key(core, command))
            return 0;
        while (override--) {
            for (size_t index = first + 1; index + 1 < core->last_change_length; ++index)
                if (!handle_key(core, core->last_change[index]))
                    return 0;
        }
        if (!handle_key(core, 27))
            return 0;
    } else {
        if (override)
            core->count = override;
        for (size_t index = first; index < core->last_change_length; ++index) {
            if (!handle_key(core, core->last_change[index]))
                return 0;
            if (override && index == first && command == 'd' &&
                index + 1 < core->last_change_length &&
                core->last_change[index + 1] >= '1' && core->last_change[index + 1] <= '9') {
                while (index + 1 < core->last_change_length &&
                    core->last_change[index + 1] >= '0' && core->last_change[index + 1] <= '9')
                    ++index;
            }
        }
    }
    if (explicit_count)
        core->repeat_count = count;
    return 1;
}

static int record_change_key(EditorCore *core, unsigned long key)
{
    if (core->change_length == core->change_capacity) {
        size_t capacity = core->change_capacity ? core->change_capacity * 2 : 32;
        if (capacity < core->change_capacity || capacity > SIZE_MAX / sizeof(*core->change_keys))
            return 0;
        unsigned long *keys = realloc(core->change_keys, capacity * sizeof(*keys));
        if (!keys)
            return 0;
        core->change_keys = keys;
        core->change_capacity = capacity;
    }
    core->change_keys[core->change_length++] = key;
    return 1;
}

static void finish_change_recording(EditorCore *core)
{
    if (core->change_dirty) {
        unsigned long *keys = core->last_change;
        size_t capacity = core->last_change_capacity;
        core->last_change = core->change_keys;
        core->last_change_length = core->change_length;
        core->last_change_capacity = core->change_capacity;
        core->change_keys = keys;
        core->change_capacity = capacity;
        core->repeat_count = 0;
    }
    core->change_length = 0;
    core->change_dirty = 0;
}

static int retab(EditorCore *core)
{
    if (!core->settings.expandtabs)
        return 1;
    EditorCore output = { 0 };
    output.output_stream = tmpfile();
    if (!output.output_stream)
        return 0;
    size_t column = 0;
    size_t cursor = 0;
    int success = 1;
    for (size_t position = 0; position < core->length;) {
        if (position == core->cursor)
            cursor = output.length;
        size_t next = next_character(core, position);
        if (text_byte(core, position) == '\t') {
            size_t spaces = core->settings.tabwidth - column;
            for (size_t index = 0; success && index < spaces; ++index)
                success = append_edit_text(&output, " ", 1);
            column = 0;
        } else {
            success = append_core_text(&output, core, position, next - position);
            column = text_byte(core, position) == '\n' || column + 1 == core->settings.tabwidth ? 0 : column + 1;
        }
        if (!success)
            break;
        position = next;
    }
    if (core->cursor == core->length)
        cursor = output.length;
    core->history_open = 0;
    if (success)
        success = write_stream_span(core, 0, core->length, output.output_stream, output.length);
    fclose(output.output_stream);
    if (!success)
        return 0;
    core->history_open = 0;
    core->cursor = core->anchor = cursor;
    core->block_insert = 0;
    clear_replace_history(core);
    core->mode = EDITOR_MODE_COMMAND;
    reset_command(core);
    return 1;
}

static int convert_line_endings(EditorCore *core, EditorLineEnding ending)
{
    EditorCore output = { 0 };
    output.output_stream = tmpfile();
    if (!output.output_stream)
        return 0;
    size_t cursor = 0;
    int success = 1;
    for (size_t position = 0; position < core->length; ++position) {
        if (position == core->cursor)
            cursor = output.length;
        char value = (char)text_byte(core, position);
        if (value == '\r' && position + 1 < core->length && text_byte(core, position + 1) == '\n')
            continue;
        if (value == '\n' && ending == EDITOR_LINE_ENDING_CRLF)
            success = append_edit_text(&output, "\r\n", 2);
        else
            success = append_edit_text(&output, &value, 1);
        if (!success)
            break;
    }
    if (core->cursor == core->length)
        cursor = output.length;
    core->history_open = 0;
    if (success)
        success = write_stream_span(core, 0, core->length, output.output_stream, output.length);
    fclose(output.output_stream);
    if (success && ending != core->line_ending)
        success = prepare_edit(core);
    if (success) {
        if (ending != core->line_ending)
            core->modified = 1;
        core->line_ending = ending;
        core->literal_bytes = 1;
        core->cursor = core->anchor = cursor;
    }
    core->history_open = 0;
    return success;
}

static int complete_command(EditorCore *core)
{
    core->status = EDITOR_STATUS_NONE;
    if (core->mode == EDITOR_MODE_COMMAND_LINE) {
        core->mode = EDITOR_MODE_COMMAND;
        reset_command(core);
    }
    return 1;
}

static char *substitute_field(const char **command, char delimiter, int required)
{
    const char *position = *command;
    char *field = malloc(strlen(position) + 1);
    if (!field)
        return NULL;
    size_t length = 0;
    while (*position && *position != delimiter) {
        if (*position == '\\') {
            field[length++] = *position++;
            if (!*position) {
                free(field);
                return NULL;
            }
        }
        field[length++] = *position++;
    }
    if (required && !*position) {
        free(field);
        return NULL;
    }
    field[length] = 0;
    *command = *position ? position + 1 : position;
    return field;
}

static int substitute_callout(pcre2_callout_block *block, void *context)
{
    size_t *budget = context;
    (void)block;
    if (!*budget)
        return PCRE2_ERROR_CALLOUT;
    --*budget;
    return 0;
}

static int substitute_replacement(EditorCore *output, const char *replacement,
    const char *subject, const PCRE2_SIZE *offsets, uint32_t captures)
{
    while (*replacement) {
        char value = *replacement++;
        unsigned int capture = 0;
        int reference = value == '&';
        if (value == '\\') {
            value = *replacement++;
            if (value >= '0' && value <= '9') {
                reference = 1;
                capture = (unsigned int)(value - '0');
            } else if (value == 'n' || value == 'r') {
                value = '\n';
            } else if (value == 't') {
                value = '\t';
            }
        }
        if (reference) {
            if (capture < captures && offsets[2 * capture] != PCRE2_UNSET &&
                !append_edit_text(output, subject + offsets[2 * capture],
                    offsets[2 * capture + 1] - offsets[2 * capture]))
                return 0;
        } else if (!append_edit_text(output, &value, 1)) {
            return 0;
        }
    }
    return 1;
}

static int substitute(EditorCore *core, const char *command, size_t first, size_t last)
{
    if (core->directory_active)
        return 0;
    unsigned char delimiter = (unsigned char)*command;
    if (delimiter <= 32 || delimiter >= 127 || delimiter == '\\' ||
        (delimiter >= '0' && delimiter <= '9') ||
        (delimiter >= 'a' && delimiter <= 'z') ||
        (delimiter >= 'A' && delimiter <= 'Z'))
        return 0;
    ++command;
    char *pattern = substitute_field(&command, (char)delimiter, 1);
    char *replacement = NULL;
    pcre2_code *code = NULL;
    pcre2_match_data *matches = NULL;
    pcre2_match_context *context = NULL;
    pcre2_compile_context *compile_context = NULL;
    EditorCore output = { 0 };
    int success = 0;
    output.output_stream = tmpfile();
    if (!output.output_stream)
        goto cleanup;
    if (!pattern || !*pattern)
        goto cleanup;
    replacement = substitute_field(&command, (char)delimiter, 0);
    if (!replacement)
        goto cleanup;
    int global = 0;
    uint32_t options = PCRE2_NEVER_UTF | PCRE2_NEVER_UCP | PCRE2_AUTO_CALLOUT;
    while (*command == 'g' || *command == 'i') {
        if (*command++ == 'g')
            global = 1;
        else
            options |= PCRE2_CASELESS;
    }
    while (*command == ' ' || *command == '\t')
        ++command;
    if (*command)
        goto cleanup;
    core->status = EDITOR_STATUS_SUBSTITUTION_FAILED;
    compile_context = pcre2_compile_context_create(NULL);
    if (!compile_context)
        goto cleanup;
    pcre2_set_parens_nest_limit(compile_context, 100);
    pcre2_set_max_pattern_length(compile_context, 65536);
    int error;
    PCRE2_SIZE error_offset;
    code = pcre2_compile((PCRE2_SPTR)pattern, strlen(pattern), options,
        &error, &error_offset, compile_context);
    if (!code) {
        core->status = EDITOR_STATUS_INVALID_PATTERN;
        goto cleanup;
    }
    matches = pcre2_match_data_create_from_pattern(code, NULL);
    context = pcre2_match_context_create(NULL);
    if (!matches || !context)
        goto cleanup;
    size_t budget = 2000000;
    pcre2_set_match_limit(context, 1000000);
    pcre2_set_depth_limit(context, 1000);
    pcre2_set_heap_limit(context, 8192);
    pcre2_set_callout(context, substitute_callout, &budget);
    size_t start = first;
    size_t cursor = core->cursor;
    int found = 0;
    for (;;) {
        size_t end = line_end(core, start);
        size_t length = end - start;
        int crlf = length && end < core->length && text_byte(core, end - 1) == '\r';
        if (crlf)
            --length;
        const char *subject = text_span(core, start, length);
        if (!subject) {
            core->status = EDITOR_STATUS_REGEX_LIMIT;
            goto cleanup;
        }
        size_t copied = 0;
        size_t offset = 0;
        size_t output_line = first + output.length;
        uint32_t match_options = 0;
        for (;;) {
            if (!budget) {
                core->status = EDITOR_STATUS_REGEX_LIMIT;
                goto cleanup;
            }
            --budget;
            int result = pcre2_match(code, (PCRE2_SPTR)subject, length, offset,
                match_options, matches, context);
            if (result == PCRE2_ERROR_NOMATCH) {
                if (match_options && offset < length) {
                    ++offset;
                    match_options = 0;
                    continue;
                }
                break;
            }
            if (result < 0) {
                if (result == PCRE2_ERROR_MATCHLIMIT || result == PCRE2_ERROR_DEPTHLIMIT ||
                    result == PCRE2_ERROR_HEAPLIMIT || result == PCRE2_ERROR_CALLOUT)
                    core->status = EDITOR_STATUS_REGEX_LIMIT;
                goto cleanup;
            }
            PCRE2_SIZE *offsets = pcre2_get_ovector_pointer(matches);
            if (offsets[0] < copied || offsets[1] < offsets[0] || offsets[1] > length)
                goto cleanup;
            if (!append_edit_text(&output, subject + copied, offsets[0] - copied) ||
                !substitute_replacement(&output, replacement, subject, offsets,
                    pcre2_get_ovector_count(matches)))
                goto cleanup;
            found = 1;
            cursor = output_line;
            copied = offset = offsets[1];
            if (!global)
                break;
            match_options = offsets[0] == offsets[1] ? PCRE2_ANCHORED | PCRE2_NOTEMPTY_ATSTART : 0;
        }
        if (!append_edit_text(&output, subject + copied, length - copied))
            goto cleanup;
        if (crlf && !append_edit_text(&output, "\r", 1))
            goto cleanup;
        if (end == last)
            break;
        if (!append_edit_text(&output, "\n", 1))
            goto cleanup;
        start = end + 1;
    }
    if (!found) {
        core->status = EDITOR_STATUS_PATTERN_NOT_FOUND;
        goto cleanup;
    }
    core->history_open = 0;
    success = write_stream_span(core, first, last, output.output_stream, output.length);
    core->history_open = 0;
    if (success) {
        core->cursor = line_start(core, cursor);
        size_t end = line_end(core, core->cursor);
        while (core->cursor < end &&
            (text_byte(core, core->cursor) == ' ' || text_byte(core, core->cursor) == '\t'))
            ++core->cursor;
        core->anchor = core->cursor;
        core->block_insert = 0;
        clear_replace_history(core);
        core->mode = EDITOR_MODE_COMMAND;
        reset_command(core);
        complete_command(core);
    }
cleanup:
    if (output.output_stream)
        fclose(output.output_stream);
    free(output.text);
    pcre2_compile_context_free(compile_context);
    pcre2_match_context_free(context);
    pcre2_match_data_free(matches);
    pcre2_code_free(code);
    free(replacement);
    free(pattern);
    return success;
}

static int execute_command(EditorCore *core, const char *text)
{
    core->status = EDITOR_STATUS_INVALID_COMMAND;
    if (!text)
        return 0;
    if (core->mode == EDITOR_MODE_COMMAND_LINE && core->command_prefix == '/') {
        if (*text == '/')
            ++text;
        return start_search(core, text) ? complete_command(core) : 0;
    }
    while (*text == ' ' || *text == '\t')
        ++text;
    if (*text == '/')
        return start_search(core, text + 1) ? complete_command(core) : 0;
    if (*text == ':')
        ++text;
    while (*text == ' ' || *text == '\t')
        ++text;
    if (strncmp(text, "'<,'>", 5) == 0) {
        if (!core->command_has_range)
            return 0;
        text += 5;
        while (*text == ' ' || *text == '\t')
            ++text;
        if (*text != 's')
            return 0;
        return substitute(core, text + 1, core->command_range_start, core->command_range_end);
    }
    if (text[0] == 's' && strncmp(text, "set", 3) != 0) {
        size_t first = line_start(core, core->cursor);
        return substitute(core, text + 1, first, line_end(core, first));
    }
    if (text[0] == '%' && text[1] == 's')
        return substitute(core, text + 2, 0, core->length);
    if (strncmp(text, "retab", 5) == 0) {
        text += 5;
        while (*text == ' ' || *text == '\t')
            ++text;
        if (*text || !retab(core))
            return 0;
        return complete_command(core);
    }
    if (strncmp(text, "set", 3) || (text[3] != ' ' && text[3] != '\t'))
        return 0;
    text += 3;
    while (*text == ' ' || *text == '\t')
        ++text;
    const char *name = text;
    while (*text >= 'a' && *text <= 'z')
        ++text;
    size_t length = (size_t)(text - name);
    while (*text == ' ' || *text == '\t')
        ++text;
    if (!*text && ((length == 2 && !memcmp(name, "lf", 2)) ||
        (length == 4 && !memcmp(name, "crlf", 4)))) {
        if (core->directory_active)
            return 0;
        EditorLineEnding ending = length == 2 ? EDITOR_LINE_ENDING_LF : EDITOR_LINE_ENDING_CRLF;
        if (!convert_line_endings(core, ending))
            return 0;
        return complete_command(core);
    }
    if (*text != '=')
        return 0;
    ++text;
    while (*text == ' ' || *text == '\t')
        ++text;
    if (*text < '0' || *text > '9')
        return 0;
    size_t value = 0;
    while (*text >= '0' && *text <= '9') {
        size_t digit = (size_t)(*text++ - '0');
        if (value > (SIZE_MAX - digit) / 10)
            return 0;
        value = value * 10 + digit;
    }
    while (*text == ' ' || *text == '\t')
        ++text;
    if (*text)
        return 0;
    EditorSettings settings = core->settings;
    if (length == 6 && memcmp(name, "maxcol", 6) == 0)
        settings.maxcol = value;
    else if (length == 8 && memcmp(name, "tabwidth", 8) == 0) {
        if (!value || value == SIZE_MAX)
            return 0;
        settings.tabwidth = value;
    } else if (length == 10 && memcmp(name, "expandtabs", 10) == 0) {
        if (value > 1)
            return 0;
        settings.expandtabs = (int)value;
    } else {
        return 0;
    }
    core->settings = settings;
    return complete_command(core);
}

static int dispatch_message(EditorCore *core, EditorMessage message, void *data)
{
    if (!core)
        return 0;
    if (core->directory_active) {
        switch (message) {
        case EDITOR_ADD_CHAR: case EDITOR_ADD_CODEPOINT: case EDITOR_INSERT_TEXT:
        case EDITOR_BACKSPACE: case EDITOR_DELETE_CHAR: case EDITOR_DELETE_LINE:
        case EDITOR_UNDO: case EDITOR_REDO: case EDITOR_SAVE_FILE: case EDITOR_REPLACE_BYTES:
            return 0;
        case EDITOR_EXECUTE_COMMAND: {
            const char *command = data;
            if (!command)
                return 0;
            if (core->command_prefix == '/')
                break;
            while (*command == ' ' || *command == '\t' || *command == ':')
                ++command;
            if (strncmp(command, "set", 3) ||
                (command[3] && command[3] != ' ' && command[3] != '\t'))
                return 0;
            break;
        }
        case EDITOR_HANDLE_KEY: {
            if (!data)
                return 0;
            unsigned long key = *(const unsigned long *)data;
            if (key == '\r' || key == '\n')
                return activate_directory_entry(core);
            if (key == '-' || key == 8)
                return open_directory(core, "..");
                if (key < EDITOR_KEY_LEFT || key > EDITOR_KEY_PAGE_DOWN) {
                if (key != 27 && key != ':' && key != '/' &&
                    key != 2 && key != 6 && key != 4 && key != 21 &&
                    !(key > 0 && key < 128 && strchr("0123456789hjklwbeWBE^$gGtTnN*", (int)key)))
                    return 0;
            }
            break;
        }
        default: break;
        }
    }
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
    case EDITOR_REGISTER_DIRECTORY_FUNCTIONS: {
        const EditorDirectoryFunctions *functions = data;
        if (!functions || !functions->list || !functions->change)
            return 0;
        core->directory_functions = *functions;
        return 1;
    }
    case EDITOR_OPEN_DIRECTORY:
        return open_directory(core, data);
    case EDITOR_ACTIVATE_DIRECTORY_ENTRY:
        return activate_directory_entry(core);
    case EDITOR_GET_DIRECTORY:
    case EDITOR_TAKE_DIRECTORY_CHANGE:
        if (!data)
            return 0;
        ((EditorDirectory *)data)->active = core->directory_active;
        ((EditorDirectory *)data)->path = message == EDITOR_GET_DIRECTORY || core->directory_changed
            ? core->directory_path : NULL;
        if (message == EDITOR_TAKE_DIRECTORY_CHANGE)
            core->directory_changed = 0;
        return 1;
    case EDITOR_REGISTER_LEXER:
        return register_lexer(core, data);
    case EDITOR_SELECT_LEXER:
        return select_lexer(core, data);
    case EDITOR_GET_LEXER:
        if (!data)
            return 0;
        *(const Lexer **)data = core->lexer;
        return 1;
    case EDITOR_TAKE_CHANGE:
        if (!data)
            return 0;
        *(EditorChange *)data = core->document_change;
        core->document_change.first_line = SIZE_MAX;
        core->document_change.last_line = 0;
        return 1;
    case EDITOR_TAKE_BUFFER_SWITCH:
        if (!data)
            return 0;
        *(EditorBufferSwitch *)data = core->buffer_switch;
        core->buffer_switch.count = 0;
        return 1;
    case EDITOR_GET_COMMAND_PREFIX:
        if (!data)
            return 0;
        *(char *)data = core->command_prefix;
        return 1;
    case EDITOR_GET_COMMAND_PROMPT:
        if (!data)
            return 0;
        *(const char **)data = core->command_prefix == '/' ? "/" :
            core->command_has_range ? ":'<,'>" : ":";
        return 1;
    case EDITOR_EXECUTE_COMMAND:
        return execute_command(core, data);
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
    case EDITOR_REPLACE_BYTES:
        return replace_bytes(core, data);
    case EDITOR_GET_BUFFER: {
        EditorBuffer *buffer = data;
        if (!buffer)
            return 0;
        return serialize_buffer(core, buffer);
    }
    case EDITOR_GET_LINE_ENDING:
        if (!data)
            return 0;
        *(EditorLineEnding *)data = core->line_ending;
        return 1;
    case EDITOR_GET_TEXT_BUFFER:
        if (!data)
            return 0;
        if (core->file)
            return serialize_buffer(core, data);
        ((EditorBuffer *)data)->data = (const unsigned char *)core->text;
        ((EditorBuffer *)data)->length = core->length;
        return 1;
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
        if (caret && core->mode != EDITOR_MODE_VISUAL_BLOCK && !core->block_insert) {
            caret->active = 0;
            caret->column = caret->advance = 0;
            return 1;
        }
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
        if (selection && !core->directory_active && !is_visual(core)) {
            selection->start = selection->end = 0;
            selection->active = selection->includes_newline = 0;
            return 1;
        }
        if (!selection || !find_line(core, selection->line, &start, &end))
            return 0;
        selection->start = selection->end = 0;
        selection->active = selection->includes_newline = 0;
        if (core->directory_active) {
            selection->active = core->directory_count && start == line_start(core, core->cursor);
            if (selection->active) {
                selection->end = end - start;
                selection->includes_newline = 1;
            }
        } else if (core->mode == EDITOR_MODE_VISUAL_BLOCK) {
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
        if (core->file) {
            if (!line || (!core->line_text && !text_span(core, 0, 0)) ||
                !virtual_buffer_line_prefix(core->file, line->index, core->line_text,
                    65535, &line->length, &line->truncated))
                return 0;
            core->line_text[line->length] = 0;
            line->text = core->line_text;
            return 1;
        }
        if (!line || !find_line(core, line->index, &start, &end))
            return 0;
        line->text = core->text + start;
        line->length = end - start;
        line->truncated = 0;
        return 1;
    }
    case EDITOR_GET_CHARACTER: {
        EditorCharacter *character = data;
        size_t start, end;
        if (core->file) {
            if (!character || !virtual_buffer_line_start(core->file, character->line, &start) ||
                character->column >= core->length - start || text_byte(core, start + character->column) == '\n')
                return 0;
            character->length = decode_text(core, start + character->column,
                core->length - start - character->column, &character->value);
            if (!character->length) {
                character->value = 0xFFFD;
                character->length = 1;
            }
            character->hidden = character->value == '\r' &&
                start + character->column + 1 < core->length &&
                text_byte(core, start + character->column + 1) == '\n';
            return 1;
        }
        if (!character || !find_line(core, character->line, &start, &end) ||
            character->column >= end - start)
            return 0;
        character->length = decode_text(core, start + character->column,
            end - start - character->column, &character->value);
        character->hidden = 0;
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
        if (core->file) {
            size_t count;
            if (!virtual_buffer_lines(core->file, core->length, &count))
                return 0;
            *(size_t *)data = count + 1;
            return 1;
        }
        size_t count = 1;
        for (size_t index = 0; index < core->length; ++index)
            if (text_byte(core, index) == '\n')
                ++count;
        *(size_t *)data = count;
        return 1;
    }
    case EDITOR_GET_KNOWN_LINE_COUNT: {
        EditorLineCount *count = data;
        if (!count || !virtual_buffer_known_lines(core->file, &count->count, &count->complete))
            return 0;
        ++count->count;
        return 1;
    }
    case EDITOR_SET_PAGE_SIZE:
        if (!data || !*(const size_t *)data)
            return 0;
        core->page_lines = *(const size_t *)data;
        return 1;
    case EDITOR_GET_POSITION: {
        if (!data)
            return 0;
        EditorPosition position = { 0, 0, core->cursor, core->length, 1 };
        position.column = advance_columns(core, 0, line_start(core, core->cursor), core->cursor);
        if (core->file) {
            size_t count;
            if (!virtual_buffer_known_lines(core->file, &count, &position.line_count_complete))
                return 0;
            position.line_count = count + 1;
            position.line = position_line(core, core->cursor);
            *(EditorPosition *)data = position;
            return 1;
        }
        position.line_count_complete = 1;
        for (size_t index = 0; index < core->length; ++index) {
            if (text_byte(core, index) != '\n')
                continue;
            ++position.line_count;
            if (index < core->cursor)
                ++position.line;
            if (!core->literal_bytes && core->line_ending == EDITOR_LINE_ENDING_CRLF) {
                if (position.length == SIZE_MAX)
                    return 0;
                ++position.length;
                if (index < core->cursor)
                    ++position.offset;
            }
        }
        *(EditorPosition *)data = position;
        return 1;
    }
    case EDITOR_GET_CARET: {
        EditorCaret *caret = data;
        if (!caret)
            return 0;
        caret->line = position_line(core, core->cursor);
        caret->column = core->cursor - line_start(core, core->cursor);
        return 1;
    }
    case EDITOR_GET_LENGTH:
        if (!data)
            return 0;
        *(size_t *)data = core->length;
        return 1;
    case EDITOR_READ_RANGE: {
        EditorReadRange *range = data;
        return range && (!range->length || range->data) &&
            read_text(core, range->offset, range->data, range->length);
    }
    case EDITOR_SET_OFFSET:
        if (!data || *(const size_t *)data > core->length)
            return 0;
        core->cursor = *(const size_t *)data;
        core->cursor = clamp_column(core, line_start(core, core->cursor),
            core->length, core->cursor - line_start(core, core->cursor));
        core->anchor = core->cursor;
        return 1;
    case EDITOR_SET_CARET: {
        const EditorCaret *caret = data;
        size_t start, end;
        if (core->file) {
            if (!caret || !virtual_buffer_line_start(core->file, caret->line, &start))
                return 0;
            size_t target = caret->column > core->length - start ? core->length : start + caret->column;
            end = start;
            while (end < target && text_byte(core, end) != '\n')
                ++end;
            core->block_insert = 0;
            core->cursor = clamp_column(core, start, core->length, end - start);
            if (core->mode == EDITOR_MODE_VISUAL_BLOCK)
                core->block_column = character_column(core, core->cursor);
            return 1;
        }
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
    int recording = 0;
    if (outer) {
        core->history_changed = 0;
        int navigation = message == EDITOR_SET_CARET ||
            (message >= EDITOR_MOVE_LEFT && message <= EDITOR_MOVE_END) ||
            (message == EDITOR_HANDLE_KEY && data &&
                *(const unsigned long *)data >= EDITOR_KEY_LEFT &&
                *(const unsigned long *)data <= EDITOR_KEY_PAGE_DOWN);
        int session_navigation = navigation &&
            (core->mode == EDITOR_MODE_INSERT || core->mode == EDITOR_MODE_REPLACE);
        if (session_navigation && core->change_length) {
            if (!record_change_key(core, 27))
                return 0;
            finish_change_recording(core);
        }
        if (message == EDITOR_HANDLE_KEY && data && core->mode != EDITOR_MODE_COMMAND_LINE) {
            unsigned long key = *(const unsigned long *)data;
            recording = !session_navigation &&
                !(key == '.' && core->mode == EDITOR_MODE_COMMAND && !core->pending);
            if (recording) {
                if (!core->change_length &&
                    (core->mode == EDITOR_MODE_INSERT || core->mode == EDITOR_MODE_REPLACE) &&
                    !record_change_key(core, core->mode == EDITOR_MODE_INSERT ? 'i' : 'R'))
                    return 0;
                if (!record_change_key(core, key))
                    return 0;
            } else {
                core->change_length = 0;
                core->change_dirty = 0;
            }
        }
        switch (message) {
        case EDITOR_REPLACE_BYTES:
        case EDITOR_ADD_CHAR: case EDITOR_ADD_CODEPOINT: case EDITOR_INSERT_TEXT:
        case EDITOR_BACKSPACE: case EDITOR_DELETE_CHAR: case EDITOR_DELETE_LINE:
        case EDITOR_SET_CARET: case EDITOR_MOVE_LEFT: case EDITOR_MOVE_RIGHT:
        case EDITOR_MOVE_UP: case EDITOR_MOVE_DOWN: case EDITOR_MOVE_HOME: case EDITOR_MOVE_END:
            core->history_open = 0;
            independent = 1;
            break;
        case EDITOR_HANDLE_KEY:
            if (data && *(const unsigned long *)data >= EDITOR_KEY_LEFT &&
                *(const unsigned long *)data <= EDITOR_KEY_PAGE_DOWN)
                core->history_open = 0;
            break;
        default: break;
        }
    }
    ++core->dispatch_depth;
    int success = dispatch_message(core, message, data);
    --core->dispatch_depth;
    if (outer) {
        if (recording) {
            core->change_dirty |= core->history_changed;
            if (!success || core->mode == EDITOR_MODE_COMMAND_LINE) {
                core->change_length = 0;
                core->change_dirty = 0;
            } else if (core->mode == EDITOR_MODE_COMMAND && !core->pending && !core->count) {
                finish_change_recording(core);
            }
        }
        if (core->history_changed || (core->history_open && core->mode == EDITOR_MODE_COMMAND))
            core->revision_cursor = core->cursor;
        if (independent || (message == EDITOR_HANDLE_KEY &&
            core->mode != EDITOR_MODE_INSERT && core->mode != EDITOR_MODE_REPLACE))
            core->history_open = 0;
    }
    return success;
}