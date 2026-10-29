#include "editor_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_line(EditorCore *core, size_t index, const char *expected)
{
    EditorLine line = { index, NULL, 0 };
    assert(editor_core_send(core, EDITOR_GET_LINE, &line));
    if (line.length != strlen(expected) || memcmp(line.text, expected, line.length) != 0)
        fprintf(stderr, "line %lu: expected [%s], got [%.*s]\n",
            (unsigned long)index, expected, (int)line.length, line.text);
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

static void send_keys(EditorCore *core, const char *keys)
{
    while (*keys)
        send_key(core, (unsigned char)*keys++);
}

static void expect_register(EditorCore *core, const char *text, int linewise)
{
    EditorRegister value;
    assert(editor_core_send(core, EDITOR_GET_REGISTER, &value));
    assert(value.length == strlen(text));
    assert(memcmp(value.text, text, value.length) == 0);
    assert(value.linewise == linewise);
}

static void test_visual_commands(void)
{
    EditorCore *core = editor_core_create("A\xC3\xA4\xF0\x9F\x98\x80Z\nsecond\nthird");
    assert(core);
    send_keys(core, "lvly");
    expect_register(core, "\xC3\xA4\xF0\x9F\x98\x80", 0);
    expect_mode(core, EDITOR_MODE_COMMAND);
    send_key(core, 'p');
    expect_line(core, 0, "A\xC3\xA4\xC3\xA4\xF0\x9F\x98\x80\xF0\x9F\x98\x80Z");
    send_keys(core, "0v2ld");
    expect_register(core, "A\xC3\xA4\xC3\xA4", 0);
    expect_line(core, 0, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80Z");
    send_keys(core, "Vjy");
    expect_register(core, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80Z\nsecond\n", 1);
    send_keys(core, "Gp");
    expect_line(core, 3, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80Z");
    expect_line(core, 4, "second");
    send_keys(core, "Vd");
    expect_line(core, 3, "second");
    expect_register(core, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80Z\n", 1);
    editor_core_destroy(core);

    core = editor_core_create("abcd\nef\n");
    assert(core);
    send_keys(core, "2lvh");
    EditorSelection selection = { 0 };
    assert(editor_core_send(core, EDITOR_GET_SELECTION, &selection));
    assert(selection.active && selection.start == 1 && selection.end == 3);
    send_keys(core, "y");
    expect_register(core, "bc", 0);
    send_keys(core, "Vj");
    selection.line = 1;
    assert(editor_core_send(core, EDITOR_GET_SELECTION, &selection));
    assert(selection.active && selection.start == 0 && selection.end == 2 && selection.includes_newline);
    selection.line = 2;
    assert(editor_core_send(core, EDITOR_GET_SELECTION, &selection));
    assert(!selection.active);
    send_key(core, 27);
    send_keys(core, "G V");
    selection.line = 2;
    assert(editor_core_send(core, EDITOR_GET_SELECTION, &selection));
    assert(selection.active && selection.end == 0 && selection.includes_newline);
    send_key(core, 'y');
    expect_register(core, "\n", 1);
    editor_core_destroy(core);
}

static void test_counted_commands(void)
{
    EditorCore *core = editor_core_create("one two three four\nline2\nline3\nline4\nline5\nline6\nline7");
    assert(core);
    send_keys(core, "2d2w");
    expect_line(core, 0, "line2");
    expect_line(core, 1, "line3");
    expect_register(core, "one two three four\n", 0);
    editor_core_destroy(core);

    core = editor_core_create("one\ntwo\nthree\nfour\nfive\nsix\nseven");
    assert(core);
    send_keys(core, "3G");
    expect_caret(core, 2, 0);
    send_keys(core, "gg2dd");
    expect_line(core, 0, "three");
    expect_register(core, "one\ntwo\n", 1);
    send_key(core, 'p');
    expect_line(core, 1, "one");
    expect_line(core, 2, "two");
    send_keys(core, "99gg");
    expect_caret(core, 6, 0);
    send_keys(core, "dd");
    expect_register(core, "seven\n", 1);
    expect_line(core, 5, "six");
    send_keys(core, "gg2d2d");
    expect_line(core, 0, "five");
    send_keys(core, "yy2p");
    expect_line(core, 1, "five");
    expect_line(core, 2, "five");
    send_keys(core, "gg2x");
    expect_line(core, 0, "ve");
    expect_register(core, "fi", 0);
    send_keys(core, "d$");
    expect_line(core, 0, "");
    send_keys(core, "3");
    send_key(core, 27);
    send_key(core, 'j');
    expect_caret(core, 1, 0);
    send_key(core, 'd');
    send_key(core, 27);
    send_key(core, 'j');
    expect_line(core, 1, "five");
    expect_caret(core, 2, 0);
    editor_core_destroy(core);
}

static void test_command_edges(void)
{
    EditorCore *core = editor_core_create("a\nb\nc");
    assert(core);
    send_keys(core, "2GdG");
    expect_line(core, 0, "a");
    expect_register(core, "b\nc\n", 1);
    send_keys(core, "p");
    expect_line(core, 2, "c");
    send_keys(core, "2GyG");
    expect_register(core, "b\nc\n", 1);
    editor_core_destroy(core);

    core = editor_core_create("ab\ncd");
    assert(core);
    send_keys(core, "vjy");
    expect_register(core, "ab\nc", 0);
    send_keys(core, "2G0vky");
    expect_register(core, "ab\nc", 0);
    send_keys(core, "0vl");
    send_key(core, 27);
    expect_register(core, "ab\nc", 0);
    send_keys(core, "0xy");
    send_key(core, 27);
    expect_register(core, "a", 0);
    send_keys(core, "jVp");
    expect_line(core, 0, "b");
    expect_line(core, 1, "a");
    send_keys(core, "ggvp");
    expect_line(core, 0, "a");
    editor_core_destroy(core);

    core = editor_core_create("abc");
    assert(core);
    send_keys(core, "vly");
    send_keys(core, "$v0p");
    expect_line(core, 0, "ab");
    expect_register(core, "ab", 0);
    send_keys(core, "0d");
    send_key(core, 'q');
    send_key(core, 'x');
    expect_line(core, 0, "b");
    unsigned long nine = '9';
    int overflow = 0;
    for (size_t digit = 0; digit < 3 * sizeof(size_t); ++digit) {
        if (!editor_core_send(core, EDITOR_HANDLE_KEY, &nine)) {
            overflow = 1;
            break;
        }
    }
    assert(overflow);
    send_key(core, 27);
    expect_line(core, 0, "b");
    editor_core_destroy(core);

    core = editor_core_create("");
    assert(core);
    send_keys(core, "ddp");
    expect_line(core, 0, "");
    expect_register(core, "\n", 1);
    send_keys(core, "Vx");
    expect_line(core, 0, "");
    expect_mode(core, EDITOR_MODE_COMMAND);
    assert(!editor_core_send(core, EDITOR_GET_SELECTION, NULL));
    assert(!editor_core_send(core, EDITOR_GET_REGISTER, NULL));
    editor_core_destroy(core);
}

static void test_paste_before(void)
{
    EditorCore *core = editor_core_create("A\xC3\xA4Z");
    assert(core);
    send_keys(core, "lvy0P");
    expect_line(core, 0, "\xC3\xA4" "A\xC3\xA4Z");
    expect_caret(core, 0, 0);
    send_keys(core, "l2P");
    expect_line(core, 0, "\xC3\xA4\xC3\xA4\xC3\xA4" "A\xC3\xA4Z");
    expect_caret(core, 0, 4);
    expect_register(core, "\xC3\xA4", 0);
    editor_core_destroy(core);

    core = editor_core_create("one\ntwo\nthree");
    assert(core);
    send_keys(core, "yy2G2P");
    expect_line(core, 0, "one");
    expect_line(core, 1, "one");
    expect_line(core, 2, "one");
    expect_line(core, 3, "two");
    expect_line(core, 4, "three");
    expect_caret(core, 1, 0);
    send_keys(core, "GP");
    expect_line(core, 4, "one");
    expect_line(core, 5, "three");
    expect_caret(core, 4, 0);
    editor_core_destroy(core);

    core = editor_core_create("a\n");
    assert(core);
    send_keys(core, "yyGP");
    expect_line(core, 0, "a");
    expect_line(core, 1, "a");
    expect_line(core, 2, "");
    expect_caret(core, 1, 0);
    editor_core_destroy(core);

    core = editor_core_create("");
    assert(core);
    send_keys(core, "PddP");
    expect_line(core, 0, "");
    expect_caret(core, 0, 0);
    send_keys(core, "iP");
    expect_line(core, 0, "P");
    expect_mode(core, EDITOR_MODE_INSERT);
    editor_core_destroy(core);

    core = editor_core_create("abc");
    assert(core);
    send_keys(core, "vylvP");
    expect_line(core, 0, "aac");
    expect_caret(core, 0, 1);
    expect_register(core, "a", 0);
    expect_mode(core, EDITOR_MODE_COMMAND);
    editor_core_destroy(core);
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

static void test_raw_files(void)
{
    unsigned char bytes[8453];
    for (size_t index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (unsigned char)index;
    bytes[0] = 0xEF;
    bytes[1] = 0xBB;
    bytes[2] = 0xBF;
    bytes[3] = '\r';
    bytes[4] = '\n';
    bytes[5] = 0;
    EditorCore *core = editor_core_create("");
    assert(core);
    EditorBuffer input = { bytes, sizeof(bytes) };
    assert(editor_core_send(core, EDITOR_SET_BUFFER, &input));
    EditorBuffer output;
    assert(editor_core_send(core, EDITOR_GET_BUFFER, &output));
    assert(output.length == sizeof(bytes) && memcmp(output.data, bytes, sizeof(bytes)) == 0);
    int modified = 1;
    assert(editor_core_send(core, EDITOR_GET_MODIFIED, &modified) && !modified);
    char path[] = "Debug/core-tests/editor_core_raw_roundtrip.bin";
    assert(editor_core_send(core, EDITOR_SAVE_FILE, path));
    FILE *file = fopen(path, "rb");
    assert(file);
    unsigned char actual[sizeof(bytes)];
    assert(fread(actual, 1, sizeof(actual), file) == sizeof(bytes));
    assert(memcmp(actual, bytes, sizeof(bytes)) == 0);
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
    send_keys(core, "iX");
    assert(editor_core_send(core, EDITOR_GET_MODIFIED, &modified) && modified);
    assert(!editor_core_send(core, EDITOR_LOAD_FILE, "Debug/core-tests/missing-directory/no-file"));
    assert(editor_core_send(core, EDITOR_GET_MODIFIED, &modified) && modified);
    assert(!editor_core_send(core, EDITOR_SAVE_FILE, "Debug/core-tests"));
    assert(editor_core_send(core, EDITOR_GET_MODIFIED, &modified) && modified);
    assert(editor_core_send(core, EDITOR_LOAD_FILE, path));
    assert(editor_core_send(core, EDITOR_GET_BUFFER, &output));
    assert(output.length == sizeof(bytes) && memcmp(output.data, bytes, sizeof(bytes)) == 0);
    expect_caret(core, 0, 0);
    expect_mode(core, EDITOR_MODE_COMMAND);
    assert(editor_core_send(core, EDITOR_GET_MODIFIED, &modified) && !modified);
    assert(editor_core_send(core, EDITOR_SET_BUFFER, &output));
    assert(editor_core_send(core, EDITOR_GET_BUFFER, &output));
    assert(memcmp(output.data, bytes, sizeof(bytes)) == 0);
    EditorBuffer invalid = { NULL, 1 };
    assert(!editor_core_send(core, EDITOR_SET_BUFFER, &invalid));
    EditorBuffer empty = { NULL, 0 };
    assert(editor_core_send(core, EDITOR_SET_BUFFER, &empty));
    assert(editor_core_send(core, EDITOR_SAVE_FILE, path));
    assert(editor_core_send(core, EDITOR_LOAD_FILE, path));
    assert(editor_core_send(core, EDITOR_GET_BUFFER, &output) && !output.length);
    assert(remove(path) == 0);

    unsigned char malformed[] = { 0x80, 0xFF, 0, 0xC3, 0xA4, 0xE2, 0x82 };
    input.data = malformed;
    input.length = sizeof(malformed);
    assert(editor_core_send(core, EDITOR_SET_BUFFER, &input));
    EditorCharacter character = { 0, 0, 0, 0 };
    assert(editor_core_send(core, EDITOR_GET_CHARACTER, &character));
    assert(character.value == 0xFFFD && character.length == 1);
    send_key(core, EDITOR_KEY_RIGHT);
    expect_caret(core, 0, 1);
    send_key(core, EDITOR_KEY_RIGHT);
    expect_caret(core, 0, 2);
    send_key(core, EDITOR_KEY_RIGHT);
    expect_caret(core, 0, 3);
    send_key(core, EDITOR_KEY_RIGHT);
    expect_caret(core, 0, 5);
    send_key(core, EDITOR_KEY_RIGHT);
    expect_caret(core, 0, 6);
    send_key(core, EDITOR_KEY_RIGHT);
    expect_caret(core, 0, 7);
    send_key(core, EDITOR_KEY_LEFT);
    expect_caret(core, 0, 6);
    send_key(core, EDITOR_KEY_LEFT);
    expect_caret(core, 0, 5);
    send_key(core, EDITOR_KEY_LEFT);
    expect_caret(core, 0, 3);
    editor_core_destroy(core);
}

static void test_visual_block(void)
{
    EditorCore *core = editor_core_create("abcd\nefgh\nijkl");
    assert(core);
    send_keys(core, "l\026" "2jIHi");
    expect_line(core, 0, "aHibcd");
    expect_line(core, 1, "efgh");
    send_key(core, 27);
    expect_mode(core, EDITOR_MODE_COMMAND);
    expect_line(core, 1, "eHifgh");
    expect_line(core, 2, "iHijkl");
    send_keys(core, "ggl\026" "2jlx");
    expect_line(core, 0, "abcd");
    expect_line(core, 1, "efgh");
    expect_line(core, 2, "ijkl");
    editor_core_destroy(core);

    core = editor_core_create("abcd\nx\nABCD");
    assert(core);
    send_keys(core, "2l\026" "2j");
    EditorSelection selection = { 2, 0, 0, 0, 0 };
    assert(editor_core_send(core, EDITOR_GET_SELECTION, &selection));
    assert(selection.active && selection.start == 2 && selection.end == 3 && !selection.includes_newline);
    EditorLineCaret caret = { 2, 0, 0, 0 };
    assert(editor_core_send(core, EDITOR_GET_LINE_CARET, &caret));
    assert(caret.active && caret.column == 2);
    send_keys(core, "IUV\b");
    caret.line = 2;
    assert(editor_core_send(core, EDITOR_GET_LINE_CARET, &caret));
    assert(caret.active && caret.column == 2 && caret.advance == 1);
    caret.line = 1;
    assert(editor_core_send(core, EDITOR_GET_LINE_CARET, &caret));
    assert(!caret.active);
    send_key(core, 27);
    expect_line(core, 0, "abUcd");
    expect_line(core, 1, "x");
    expect_line(core, 2, "ABUCD");
    expect_caret(core, 0, 2);
    send_keys(core, "G2l\026" "2khx");
    expect_line(core, 0, "acd");
    expect_line(core, 1, "x");
    expect_line(core, 2, "ACD");
    editor_core_destroy(core);

    core = editor_core_create("a\xC3\xA4z\nbQz");
    assert(core);
    send_keys(core, "l\026jx");
    expect_line(core, 0, "az");
    expect_line(core, 1, "bz");
    send_keys(core, "ggl\026jI");
    send_key(core, '\b');
    send_key(core, 0xE4);
    send_key(core, '\b');
    send_key(core, 27);
    expect_line(core, 0, "az");
    expect_line(core, 1, "bz");
    send_keys(core, "ggl\026jI");
    send_key(core, 0xE4);
    send_key(core, 27);
    expect_line(core, 0, "a\xC3\xA4z");
    expect_line(core, 1, "b\xC3\xA4z");
    send_keys(core, "gg\026jI");
    send_key(core, 27);
    expect_line(core, 1, "b\xC3\xA4z");
    send_keys(core, "gg\026jIA");
    send_key(core, EDITOR_KEY_RIGHT);
    send_key(core, 27);
    expect_line(core, 0, "Aa\xC3\xA4z");
    expect_line(core, 1, "b\xC3\xA4z");
    send_keys(core, "gg\026j");
    send_key(core, 22);
    expect_mode(core, EDITOR_MODE_COMMAND);
    assert(!editor_core_send(core, EDITOR_GET_LINE_CARET, NULL));
    editor_core_destroy(core);

    core = editor_core_create("a\nb");
    assert(core);
    send_keys(core, "\026jIZ");
    const char *path = "Debug/core-tests/editor_core_block_save.bin";
    assert(editor_core_send(core, EDITOR_SAVE_FILE, (void *)path));
    int modified = 1;
    assert(editor_core_send(core, EDITOR_GET_MODIFIED, &modified) && !modified);
    send_key(core, 27);
    assert(editor_core_send(core, EDITOR_GET_MODIFIED, &modified) && modified);
    expect_line(core, 1, "Zb");
    assert(remove(path) == 0);
    editor_core_destroy(core);
}

static void test_replace(void)
{
    EditorCore *core = editor_core_create("abcd\nefgh");
    assert(core);
    send_keys(core, "l2rZ");
    expect_line(core, 0, "aZZd");
    expect_caret(core, 0, 2);
    send_keys(core, "0R12");
    expect_line(core, 0, "12Zd");
    send_key(core, '\b');
    expect_line(core, 0, "1ZZd");
    send_keys(core, "Q");
    send_key(core, 27);
    expect_line(core, 0, "1QZd");
    expect_mode(core, EDITOR_MODE_COMMAND);
    expect_caret(core, 0, 1);
    editor_core_destroy(core);

    core = editor_core_create("a\xC3\xA4\xF0\x9F\x98\x80z\nsecond");
    assert(core);
    send_keys(core, "l2r");
    send_key(core, 0x20AC);
    expect_line(core, 0, "a\xE2\x82\xAC\xE2\x82\xACz");
    expect_caret(core, 0, 4);
    send_keys(core, "0R");
    expect_mode(core, EDITOR_MODE_REPLACE);
    send_key(core, 0x1F600);
    send_key(core, 'q');
    expect_line(core, 0, "\xF0\x9F\x98\x80q\xE2\x82\xACz");
    send_key(core, '\b');
    send_key(core, '\b');
    send_key(core, '\b');
    expect_line(core, 0, "a\xE2\x82\xAC\xE2\x82\xACz");
    expect_caret(core, 0, 0);
    send_key(core, 27);
    send_keys(core, "$rX");
    expect_line(core, 0, "a\xE2\x82\xAC\xE2\x82\xACz");
    send_keys(core, "0r");
    send_key(core, 27);
    send_keys(core, "99rX");
    expect_line(core, 0, "a\xE2\x82\xAC\xE2\x82\xACz");
    send_keys(core, "$RXY");
    expect_line(core, 0, "a\xE2\x82\xAC\xE2\x82\xACzXY");
    expect_line(core, 1, "second");
    send_key(core, '\b');
    send_key(core, '\b');
    expect_line(core, 0, "a\xE2\x82\xAC\xE2\x82\xACz");
    send_key(core, '\r');
    expect_line(core, 1, "");
    send_key(core, '\b');
    expect_line(core, 1, "second");
    send_key(core, 27);
    editor_core_destroy(core);

    core = editor_core_create("abcd\nx\nABCD");
    assert(core);
    send_keys(core, "l\026" "2jlrZ");
    expect_line(core, 0, "aZZd");
    expect_line(core, 1, "x");
    expect_line(core, 2, "AZZD");
    expect_mode(core, EDITOR_MODE_COMMAND);
    expect_caret(core, 0, 1);
    send_keys(core, "0v2lrQ");
    expect_line(core, 0, "QQQd");
    expect_caret(core, 0, 0);
    send_keys(core, "Vjr.");
    expect_line(core, 0, "....");
    expect_line(core, 1, ".");
    expect_line(core, 2, "AZZD");
    send_keys(core, "0RZ");
    send_key(core, EDITOR_KEY_RIGHT);
    send_key(core, '\b');
    expect_line(core, 0, "Z...");
    send_key(core, 'Y');
    send_key(core, '\b');
    expect_line(core, 0, "Z...");
    send_key(core, 27);
    send_keys(core, "0R\t");
    expect_line(core, 0, "    ");
    send_key(core, '\b');
    expect_line(core, 0, "Z...");
    EditorBuffer empty = { NULL, 0 };
    assert(editor_core_send(core, EDITOR_SET_BUFFER, &empty));
    expect_mode(core, EDITOR_MODE_COMMAND);
    send_keys(core, "rX");
    expect_line(core, 0, "");
    send_keys(core, "Rabc");
    expect_line(core, 0, "abc");
    send_key(core, 27);
    expect_caret(core, 0, 2);
    editor_core_destroy(core);
}

int main(void)
{
    test_replace();
    test_visual_block();
    test_raw_files();
    test_visual_commands();
    test_counted_commands();
    test_command_edges();
    test_paste_before();
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
    assert(editor_core_send(core, EDITOR_GET_CHARACTER, &character));
    assert(character.value == 0xFFFD && character.length == 1);
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