#include "editor_core.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct EditorCore {
    char *text;
    size_t length;
    size_t capacity;
    size_t cursor;
    EditorMode mode;
};

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
    memmove(core->text + core->cursor + count, core->text + core->cursor,
        core->length - core->cursor + 1);
    memcpy(core->text + core->cursor, normalized, count);
    core->length += count;
    core->cursor += count;
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
    --position;
    while (position > 0 && ((unsigned char)core->text[position] & 0xC0) == 0x80)
        --position;
    return position;
}

static size_t next_character(const EditorCore *core, size_t position)
{
    if (position == core->length)
        return position;
    unsigned long value;
    return position + decode_character(core->text + position, core->length - position, &value);
}

static void erase_text(EditorCore *core, size_t start, size_t end)
{
    memmove(core->text + start, core->text + end,
        core->length - end + 1);
    core->length -= end - start;
    if (core->cursor > end)
        core->cursor -= end - start;
    else if (core->cursor > start)
        core->cursor = start;
}

static size_t clamp_column(const EditorCore *core, size_t start, size_t end, size_t column)
{
    size_t position = start + (column < end - start ? column : end - start);
    while (position > start && position < end &&
        ((unsigned char)core->text[position] & 0xC0) == 0x80)
        --position;
    return position;
}

EditorCore *editor_core_create(const char *initial_text)
{
    EditorCore *core = calloc(1, sizeof(*core));
    if (!core)
        return NULL;
    if (!insert_text(core, initial_text ? initial_text : "")) {
        editor_core_destroy(core);
        return NULL;
    }
    core->cursor = 0;
    return core;
}

void editor_core_destroy(EditorCore *core)
{
    if (core) {
        free(core->text);
        free(core);
    }
}

static int handle_key(EditorCore *core, unsigned long key)
{
    if (key == 27) {
        core->mode = EDITOR_MODE_COMMAND;
        return 1;
    }
    switch (key) {
    case EDITOR_KEY_DELETE: return editor_core_send(core, EDITOR_DELETE_CHAR, NULL);
    case EDITOR_KEY_LEFT: return editor_core_send(core, EDITOR_MOVE_LEFT, NULL);
    case EDITOR_KEY_RIGHT: return editor_core_send(core, EDITOR_MOVE_RIGHT, NULL);
    case EDITOR_KEY_UP: return editor_core_send(core, EDITOR_MOVE_UP, NULL);
    case EDITOR_KEY_DOWN: return editor_core_send(core, EDITOR_MOVE_DOWN, NULL);
    case EDITOR_KEY_HOME: return editor_core_send(core, EDITOR_MOVE_HOME, NULL);
    case EDITOR_KEY_END: return editor_core_send(core, EDITOR_MOVE_END, NULL);
    }
    if (!key || key > 0x10FFFF || (key >= 0xD800 && key <= 0xDFFF))
        return 0;
    switch (core->mode) {
    case EDITOR_MODE_COMMAND:
        switch (key) {
        case 'i': core->mode = EDITOR_MODE_INSERT; return 1;
        case 'h': return editor_core_send(core, EDITOR_MOVE_LEFT, NULL);
        case 'j': return editor_core_send(core, EDITOR_MOVE_DOWN, NULL);
        case 'k': return editor_core_send(core, EDITOR_MOVE_UP, NULL);
        case 'l': return editor_core_send(core, EDITOR_MOVE_RIGHT, NULL);
        case 'x': return editor_core_send(core, EDITOR_DELETE_CHAR, NULL);
        case '0': return editor_core_send(core, EDITOR_MOVE_HOME, NULL);
        case '$': return editor_core_send(core, EDITOR_MOVE_END, NULL);
        default: return 1;
        }
    case EDITOR_MODE_INSERT:
        switch (key) {
        case '\b': return editor_core_send(core, EDITOR_BACKSPACE, NULL);
        case '\t': return editor_core_send(core, EDITOR_INSERT_TEXT, "    ");
        case '\r':
        case '\n': return editor_core_send(core, EDITOR_ADD_CHAR, "\n");
        default:
            if (key < ' ' || key == 127)
                return 1;
            return editor_core_send(core, EDITOR_ADD_CODEPOINT, &key);
        }
    default:
        return 0;
    }
}

int editor_core_send(EditorCore *core, EditorMessage message, void *data)
{
    if (!core)
        return 0;
    switch (message) {
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
        if (!value || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
            return 0;
        if (value < 0x80) {
            text[0] = (char)value;
        } else if (value < 0x800) {
            text[0] = (char)(0xC0 | (value >> 6));
            text[1] = (char)(0x80 | (value & 0x3F));
        } else if (value < 0x10000) {
            text[0] = (char)(0xE0 | (value >> 12));
            text[1] = (char)(0x80 | ((value >> 6) & 0x3F));
            text[2] = (char)(0x80 | (value & 0x3F));
        } else {
            text[0] = (char)(0xF0 | (value >> 18));
            text[1] = (char)(0x80 | ((value >> 12) & 0x3F));
            text[2] = (char)(0x80 | ((value >> 6) & 0x3F));
            text[3] = (char)(0x80 | (value & 0x3F));
        }
        return insert_text(core, text);
    }
    case EDITOR_INSERT_TEXT:
        return insert_text(core, data);
    case EDITOR_BACKSPACE:
        erase_text(core, previous_character(core, core->cursor), core->cursor);
        return 1;
    case EDITOR_DELETE_CHAR:
        erase_text(core, core->cursor, next_character(core, core->cursor));
        return 1;
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
        return character->length != 0;
    }
    case EDITOR_DELETE_LINE: {
        size_t start, end;
        if (!data || !find_line(core, *(const size_t *)data, &start, &end))
            return 0;
        if (end < core->length)
            ++end;
        else if (start > 0)
            --start;
        erase_text(core, start, end);
        return 1;
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
        core->cursor = clamp_column(core, start, end, caret->column);
        return 1;
    }
    case EDITOR_MOVE_LEFT:
        core->cursor = previous_character(core, core->cursor);
        return 1;
    case EDITOR_MOVE_RIGHT:
        core->cursor = next_character(core, core->cursor);
        return 1;
    case EDITOR_MOVE_HOME:
        core->cursor = line_start(core, core->cursor);
        return 1;
    case EDITOR_MOVE_END:
        core->cursor = line_end(core, core->cursor);
        return 1;
    case EDITOR_MOVE_UP:
    case EDITOR_MOVE_DOWN: {
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