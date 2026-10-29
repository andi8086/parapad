#include "editor_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_line(EditorCore *core, size_t index, const char *expected)
{
    EditorLine line = { index, NULL, 0 };
    assert(editor_core_send(core, EDITOR_GET_LINE, &line));
    assert(line.length == strlen(expected));
    assert(memcmp(line.text, expected, line.length) == 0);
}

static void expect_caret(EditorCore *core, size_t line, size_t column)
{
    EditorCaret caret;
    assert(editor_core_send(core, EDITOR_GET_CARET, &caret));
    assert(caret.line == line && caret.column == column);
}

static void send_key(EditorCore *core, unsigned long key)
{
    assert(editor_core_send(core, EDITOR_HANDLE_KEY, &key));
}

static void expect_mode(EditorCore *core, EditorMode expected)
{
    EditorMode mode;
    assert(editor_core_send(core, EDITOR_GET_MODE, &mode));
    assert(mode == expected);
}

static void test_modes(void)
{
    EditorCore *core = editor_core_create("");
    assert(core);
    expect_mode(core, EDITOR_MODE_COMMAND);
    send_key(core, 'a');
    send_key(core, '\r');
    send_key(core, '\t');
    send_key(core, '\b');
    expect_line(core, 0, "");
    send_key(core, 'i');
    expect_mode(core, EDITOR_MODE_INSERT);
    expect_line(core, 0, "");
    send_key(core, 'h');
    send_key(core, 'i');
    send_key(core, 0x1F600);
    expect_line(core, 0, "hi\xF0\x9F\x98\x80");
    send_key(core, '\b');
    expect_line(core, 0, "hi");
    send_key(core, '\r');
    send_key(core, '\t');
    send_key(core, 'x');
    expect_line(core, 1, "    x");
    send_key(core, 27);
    expect_mode(core, EDITOR_MODE_COMMAND);
    send_key(core, 27);
    send_key(core, 'z');
    send_key(core, '\b');
    expect_line(core, 1, "    x");
    send_key(core, '0');
    expect_caret(core, 1, 0);
    send_key(core, 'k');
    send_key(core, 'l');
    expect_caret(core, 0, 1);
    send_key(core, 'h');
    send_key(core, 'x');
    expect_line(core, 0, "i");
    send_key(core, 'j');
    send_key(core, '$');
    expect_caret(core, 1, 5);
    send_key(core, EDITOR_KEY_LEFT);
    send_key(core, EDITOR_KEY_DELETE);
    expect_line(core, 1, "    ");
    send_key(core, 'i');
    send_key(core, 0x20AC);
    send_key(core, EDITOR_KEY_LEFT);
    send_key(core, EDITOR_KEY_DELETE);
    expect_line(core, 1, "    ");
    expect_mode(core, EDITOR_MODE_INSERT);
    send_key(core, 27);
    unsigned long invalid = 0xD800;
    assert(!editor_core_send(core, EDITOR_HANDLE_KEY, &invalid));
    assert(!editor_core_send(core, EDITOR_HANDLE_KEY, NULL));
    assert(!editor_core_send(core, EDITOR_GET_MODE, NULL));
    expect_mode(core, EDITOR_MODE_COMMAND);
    EditorCore *other = editor_core_create(NULL);
    assert(other);
    send_key(other, 'i');
    expect_mode(core, EDITOR_MODE_COMMAND);
    expect_mode(other, EDITOR_MODE_INSERT);
    editor_core_destroy(other);
    editor_core_destroy(core);
}

int main(void)
{
    test_modes();
    EditorCore *core = editor_core_create("ab\r\ncd\ref\n");
    assert(core);
    size_t count = 0;
    assert(editor_core_send(core, EDITOR_GET_LINE_COUNT, &count) && count == 4);
    expect_line(core, 0, "ab");
    expect_line(core, 1, "cd");
    expect_line(core, 2, "ef");
    expect_line(core, 3, "");
    expect_caret(core, 0, 0);
    assert(editor_core_send(core, EDITOR_BACKSPACE, NULL));
    assert(editor_core_send(core, EDITOR_ADD_CHAR, "X"));
    expect_line(core, 0, "Xab");
    assert(editor_core_send(core, EDITOR_DELETE_CHAR, NULL));
    expect_line(core, 0, "Xb");
    assert(editor_core_send(core, EDITOR_INSERT_TEXT, "\r\n12"));
    expect_line(core, 0, "X");
    expect_line(core, 1, "12b");
    expect_caret(core, 1, 2);
    assert(editor_core_send(core, EDITOR_MOVE_HOME, NULL));
    assert(editor_core_send(core, EDITOR_BACKSPACE, NULL));
    expect_line(core, 0, "X12b");
    expect_caret(core, 0, 1);
    assert(editor_core_send(core, EDITOR_MOVE_END, NULL));
    assert(editor_core_send(core, EDITOR_DELETE_CHAR, NULL));
    expect_line(core, 0, "X12bcd");
    size_t index = 1;
    assert(editor_core_send(core, EDITOR_DELETE_LINE, &index));
    expect_line(core, 1, "");
    assert(editor_core_send(core, EDITOR_DELETE_LINE, &index));
    expect_line(core, 0, "X12bcd");
    assert(editor_core_send(core, EDITOR_GET_LINE_COUNT, &count) && count == 1);
    index = 0;
    assert(editor_core_send(core, EDITOR_DELETE_LINE, &index));
    expect_line(core, 0, "");
    expect_caret(core, 0, 0);
    assert(editor_core_send(core, EDITOR_DELETE_CHAR, NULL));
    assert(editor_core_send(core, EDITOR_INSERT_TEXT, "abcd\nx\nxyz"));
    EditorCaret caret = { 0, 3 };
    assert(editor_core_send(core, EDITOR_SET_CARET, &caret));
    assert(editor_core_send(core, EDITOR_MOVE_DOWN, NULL));
    expect_caret(core, 1, 1);
    assert(editor_core_send(core, EDITOR_MOVE_UP, NULL));
    expect_caret(core, 0, 1);
    assert(editor_core_send(core, EDITOR_MOVE_HOME, NULL));
    assert(editor_core_send(core, EDITOR_MOVE_LEFT, NULL));
    expect_caret(core, 0, 0);
    caret.line = 2;
    caret.column = 99;
    assert(editor_core_send(core, EDITOR_SET_CARET, &caret));
    assert(editor_core_send(core, EDITOR_MOVE_RIGHT, NULL));
    expect_caret(core, 2, 3);
    EditorLine invalid = { 3, NULL, 0 };
    assert(!editor_core_send(core, EDITOR_GET_LINE, &invalid));
    assert(!editor_core_send(core, EDITOR_GET_LINE, NULL));
    assert(!editor_core_send(NULL, EDITOR_BACKSPACE, NULL));
    editor_core_destroy(core);

    core = editor_core_create(NULL);
    assert(core);
    char emoji[] = "\xF0\x9F\x98\x80";
    assert(editor_core_send(core, EDITOR_ADD_CHAR, emoji));
    assert(editor_core_send(core, EDITOR_MOVE_LEFT, NULL));
    expect_caret(core, 0, 0);
    assert(editor_core_send(core, EDITOR_MOVE_RIGHT, NULL));
    expect_caret(core, 0, 4);
    assert(editor_core_send(core, EDITOR_BACKSPACE, NULL));
    expect_line(core, 0, "");
    assert(editor_core_send(core, EDITOR_INSERT_TEXT, emoji));
    assert(editor_core_send(core, EDITOR_MOVE_HOME, NULL));
    assert(editor_core_send(core, EDITOR_DELETE_CHAR, NULL));
    expect_line(core, 0, "");
    assert(editor_core_send(core, EDITOR_INSERT_TEXT, "A\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80"));
    expect_caret(core, 0, 10);
    caret.line = 0;
    caret.column = 8;
    assert(editor_core_send(core, EDITOR_SET_CARET, &caret));
    expect_caret(core, 0, 6);
    EditorCharacter character = { 0, 6, 0, 0 };
    assert(editor_core_send(core, EDITOR_GET_CHARACTER, &character));
    assert(character.value == 0x1F600 && character.length == 4);
    character.column = 7;
    assert(!editor_core_send(core, EDITOR_GET_CHARACTER, &character));
    assert(editor_core_send(core, EDITOR_MOVE_LEFT, NULL));
    expect_caret(core, 0, 3);
    assert(editor_core_send(core, EDITOR_DELETE_CHAR, NULL));
    expect_line(core, 0, "A\xC3\xA4\xF0\x9F\x98\x80");
    assert(editor_core_send(core, EDITOR_BACKSPACE, NULL));
    expect_line(core, 0, "A\xF0\x9F\x98\x80");
    const char *invalid_text[] = { "\x80", "\xC0\xAF", "\xE2\x82", "\xED\xA0\x80",
        "\xF4\x90\x80\x80", "\xF8\x80\x80\x80\x80", "ok\xFF" };
    for (size_t test = 0; test < sizeof(invalid_text) / sizeof(invalid_text[0]); ++test) {
        assert(!editor_core_send(core, EDITOR_INSERT_TEXT, (void *)invalid_text[test]));
        expect_line(core, 0, "A\xF0\x9F\x98\x80");
        expect_caret(core, 0, 1);
        assert(!editor_core_create(invalid_text[test]));
    }
    assert(!editor_core_send(core, EDITOR_ADD_CHAR, "ab"));
    unsigned long codepoint = 0x20AC;
    assert(editor_core_send(core, EDITOR_ADD_CODEPOINT, &codepoint));
    expect_line(core, 0, "A\xE2\x82\xAC\xF0\x9F\x98\x80");
    codepoint = 0x1F600;
    assert(editor_core_send(core, EDITOR_ADD_CODEPOINT, &codepoint));
    codepoint = 0xD800;
    assert(!editor_core_send(core, EDITOR_ADD_CODEPOINT, &codepoint));
    codepoint = 0x110000;
    assert(!editor_core_send(core, EDITOR_ADD_CODEPOINT, &codepoint));
    editor_core_destroy(core);
    puts("editor_core tests passed");
    return 0;
}