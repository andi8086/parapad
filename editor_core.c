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
    size_t anchor;
    size_t count;
    size_t operator_count;
    int operator_has_count;
    unsigned long pending;
    char *register_text;
    size_t register_length;
    int register_linewise;
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
    if (core->mode == EDITOR_MODE_VISUAL || core->mode == EDITOR_MODE_VISUAL_LINE)
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
    if (core->mode == EDITOR_MODE_VISUAL || core->mode == EDITOR_MODE_VISUAL_LINE)
        core->mode = EDITOR_MODE_COMMAND;
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
    return core->mode == EDITOR_MODE_VISUAL || core->mode == EDITOR_MODE_VISUAL_LINE;
}

static void reset_command(EditorCore *core)
{
    core->count = 0;
    core->operator_count = 0;
    core->operator_has_count = 0;
    core->pending = 0;
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
        erase_text(core, range.start, range.end);
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
    memmove(core->text + range.start + length, core->text + range.end,
        core->length - range.end + 1);
    memcpy(core->text + range.start, text, length);
    core->length = new_length;
    core->cursor = core->register_linewise || range.linewise ? range.start + prefix :
        previous_character(core, range.start + length);
    core->mode = EDITOR_MODE_COMMAND;
    free(text);
    return 1;
}

static int handle_key(EditorCore *core, unsigned long key)
{
    if (key == 27) {
        core->mode = EDITOR_MODE_COMMAND;
        reset_command(core);
        return 1;
    }
    if (!key || key > EDITOR_KEY_END ||
        (key > 0x10FFFF && key < EDITOR_KEY_DELETE) || (key >= 0xD800 && key <= 0xDFFF))
        return 0;
    if (core->mode == EDITOR_MODE_INSERT) {
        if (key == EDITOR_KEY_DELETE)
            return editor_core_send(core, EDITOR_DELETE_CHAR, NULL);
        if (key >= EDITOR_KEY_LEFT)
            return move_command(core, key, 1, 0);
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
    if (key == 'v' || key == 'V') {
        EditorMode mode = key == 'v' ? EDITOR_MODE_VISUAL : EDITOR_MODE_VISUAL_LINE;
        if (core->mode == mode)
            core->mode = EDITOR_MODE_COMMAND;
        else {
            if (!is_visual(core))
                core->anchor = core->cursor;
            core->mode = mode;
        }
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
    return 1;
}

int editor_core_send(EditorCore *core, EditorMessage message, void *data)
{
    if (!core)
        return 0;
    switch (message) {
    case EDITOR_GET_REGISTER: {
        EditorRegister *value = data;
        if (!value)
            return 0;
        value->text = core->register_text ? core->register_text : "";
        value->length = core->register_length;
        value->linewise = core->register_linewise;
        return 1;
    }
    case EDITOR_GET_SELECTION: {
        EditorSelection *selection = data;
        size_t start, end;
        if (!selection || !find_line(core, selection->line, &start, &end))
            return 0;
        selection->start = selection->end = 0;
        selection->active = selection->includes_newline = 0;
        if (is_visual(core)) {
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