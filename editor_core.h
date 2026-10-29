#ifndef EDITOR_CORE_H
#define EDITOR_CORE_H

#include <stddef.h>

typedef struct EditorCore EditorCore;

typedef enum EditorMode {
    EDITOR_MODE_COMMAND,
    EDITOR_MODE_INSERT
} EditorMode;

typedef enum EditorKey {
    EDITOR_KEY_DELETE = 0x110000,
    EDITOR_KEY_LEFT,
    EDITOR_KEY_RIGHT,
    EDITOR_KEY_UP,
    EDITOR_KEY_DOWN,
    EDITOR_KEY_HOME,
    EDITOR_KEY_END
} EditorKey;

typedef enum EditorMessage {
    EDITOR_ADD_CHAR,
    EDITOR_ADD_CODEPOINT,
    EDITOR_INSERT_TEXT,
    EDITOR_BACKSPACE,
    EDITOR_DELETE_CHAR,
    EDITOR_GET_LINE,
    EDITOR_GET_CHARACTER,
    EDITOR_DELETE_LINE,
    EDITOR_GET_LINE_COUNT,
    EDITOR_GET_CARET,
    EDITOR_SET_CARET,
    EDITOR_MOVE_LEFT,
    EDITOR_MOVE_RIGHT,
    EDITOR_MOVE_UP,
    EDITOR_MOVE_DOWN,
    EDITOR_MOVE_HOME,
    EDITOR_MOVE_END,
    EDITOR_HANDLE_KEY,
    EDITOR_GET_MODE
} EditorMessage;

typedef struct EditorLine {
    size_t index;
    const char *text;
    size_t length;
} EditorLine;

typedef struct EditorCaret {
    size_t line;
    size_t column;
} EditorCaret;

typedef struct EditorCharacter {
    size_t line;
    size_t column;
    unsigned long value;
    size_t length;
} EditorCharacter;

EditorCore *editor_core_create(const char *initial_text);
void editor_core_destroy(EditorCore *core);
int editor_core_send(EditorCore *core, EditorMessage message, void *data);

#endif