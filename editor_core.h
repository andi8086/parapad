#ifndef EDITOR_CORE_H
#define EDITOR_CORE_H

#include <stddef.h>
#include "lexer.h"

typedef struct EditorCore EditorCore;

typedef enum EditorLineEnding {
    EDITOR_LINE_ENDING_LF,
    EDITOR_LINE_ENDING_CRLF
} EditorLineEnding;

typedef enum EditorMode {
    EDITOR_MODE_COMMAND,
    EDITOR_MODE_INSERT,
    EDITOR_MODE_VISUAL,
    EDITOR_MODE_VISUAL_LINE,
    EDITOR_MODE_VISUAL_BLOCK,
    EDITOR_MODE_REPLACE,
    EDITOR_MODE_COMMAND_LINE
} EditorMode;

typedef enum EditorStatus {
    EDITOR_STATUS_NONE,
    EDITOR_STATUS_NO_CHANGE,
    EDITOR_STATUS_INVALID_COMMAND,
    EDITOR_STATUS_PATTERN_NOT_FOUND,
    EDITOR_STATUS_NO_PREVIOUS_SEARCH
} EditorStatus;

typedef enum EditorKey {
    EDITOR_KEY_DELETE = 0x110000,
    EDITOR_KEY_LEFT,
    EDITOR_KEY_RIGHT,
    EDITOR_KEY_UP,
    EDITOR_KEY_DOWN,
    EDITOR_KEY_HOME,
    EDITOR_KEY_END,
    EDITOR_KEY_PAGE_UP,
    EDITOR_KEY_PAGE_DOWN
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
    EDITOR_GET_MODE,
    EDITOR_GET_SELECTION,
    EDITOR_GET_REGISTER,
    EDITOR_SET_BUFFER,
    EDITOR_GET_BUFFER,
    EDITOR_LOAD_FILE,
    EDITOR_SAVE_FILE,
    EDITOR_GET_MODIFIED,
    EDITOR_GET_LINE_CARET,
    EDITOR_UNDO,
    EDITOR_REDO,
    EDITOR_GET_STATUS,
    EDITOR_GET_SETTINGS,
    EDITOR_SET_SETTINGS,
    EDITOR_EXECUTE_COMMAND,
    EDITOR_GET_COMMAND_PREFIX,
    EDITOR_TAKE_BUFFER_SWITCH,
    EDITOR_TAKE_CHANGE,
    EDITOR_REGISTER_LEXER,
    EDITOR_SELECT_LEXER,
    EDITOR_GET_LEXER,
    EDITOR_REGISTER_DIRECTORY_FUNCTIONS,
    EDITOR_OPEN_DIRECTORY,
    EDITOR_ACTIVATE_DIRECTORY_ENTRY,
    EDITOR_GET_DIRECTORY,
    EDITOR_TAKE_DIRECTORY_CHANGE,
    EDITOR_GET_LINE_ENDING,
    EDITOR_GET_TEXT_BUFFER,
    EDITOR_GET_POSITION,
    EDITOR_SET_PAGE_SIZE
} EditorMessage;

typedef struct EditorPosition {
    size_t line;
    size_t column;
    size_t offset;
    size_t length;
    size_t line_count;
} EditorPosition;

typedef struct EditorDirectoryEntry {
    const char *name;
    const char *path;
    int is_directory;
} EditorDirectoryEntry;

typedef int (*EditorDirectoryEmit)(void *context, const EditorDirectoryEntry *entry);

typedef struct EditorDirectoryFunctions {
    void *context;
    int (*list)(void *context, const char *path, EditorDirectoryEmit emit, void *emit_context);
    int (*change)(void *context, const char *current_path, const char *requested_path,
        char **resolved_path);
} EditorDirectoryFunctions;

typedef struct EditorDirectory {
    const char *path;
    int active;
} EditorDirectory;

typedef struct EditorChange {
    size_t revision;
    size_t first_line;
    size_t last_line;
} EditorChange;

typedef struct EditorBufferSwitch {
    size_t count;
    int backwards;
    int absolute;
} EditorBufferSwitch;

typedef struct EditorSettings {
    size_t maxcol;
    size_t tabwidth;
    int expandtabs;
} EditorSettings;

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

typedef struct EditorLineCaret {
    size_t line;
    size_t column;
    size_t advance;
    int active;
} EditorLineCaret;

typedef struct EditorSelection {
    size_t line;
    size_t start;
    size_t end;
    int active;
    int includes_newline;
} EditorSelection;

typedef struct EditorRegister {
    const char *text;
    size_t length;
    int linewise;
} EditorRegister;

typedef struct EditorBuffer {
    const unsigned char *data;
    size_t length;
} EditorBuffer;

EditorCore *editor_core_create(const char *initial_text);
void editor_core_destroy(EditorCore *core);
int editor_core_send(EditorCore *core, EditorMessage message, void *data);

#endif