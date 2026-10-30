#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "editor_core.h"
#include "virtual_buffer.h"
#include "lexer_c_cpp.h"

#ifdef NDEBUG
#error These tests require assertions to be enabled.
#endif

typedef struct TestFiles {
    size_t created;
    size_t mapped;
    size_t unmapped;
    int fail_create;
    int fail_map;
} TestFiles;

typedef struct Bytes {
    unsigned char *data;
    size_t length;
} Bytes;

static FILE *create_temporary(void *context)
{
    TestFiles *files = context;
    if (files->fail_create)
        return NULL;
    FILE *file = tmpfile();
    if (file)
        ++files->created;
    return file;
}

static int map_readonly(void *context, FILE *file, size_t length,
    const unsigned char **data, void **mapping)
{
    TestFiles *files = context;
    *data = NULL;
    *mapping = NULL;
    if (files->fail_map)
        return 0;
    HANDLE section = CreateFileMappingA((HANDLE)_get_osfhandle(_fileno(file)),
        NULL, PAGE_READONLY, 0, 0, NULL);
    if (!section)
        return 0;
    const unsigned char *view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, length);
    if (!view) {
        CloseHandle(section);
        return 0;
    }
    *data = view;
    *mapping = section;
    ++files->mapped;
    return 1;
}

static void unmap_readonly(void *context, const unsigned char *data, void *mapping)
{
    TestFiles *files = context;
    assert(UnmapViewOfFile(data));
    assert(CloseHandle((HANDLE)mapping));
    ++files->unmapped;
}

static EditorCore *create_core(TestFiles *files)
{
    EditorCore *core = editor_core_create(NULL);
    EditorFileFunctions functions = { files, create_temporary, map_readonly, unmap_readonly };
    assert(core);
    assert(editor_core_send(core, EDITOR_REGISTER_FILE_FUNCTIONS, &functions));
    return core;
}

static Bytes make_bytes(size_t length)
{
    Bytes bytes = { malloc(length ? length : 1), length };
    assert(bytes.data);
    for (size_t index = 0; index < length; ++index)
        bytes.data[index] = (unsigned char)(index * 37u + index / 251u);
    return bytes;
}

static Bytes copy_bytes(const Bytes *source)
{
    Bytes copy = make_bytes(source->length);
    memcpy(copy.data, source->data, source->length);
    return copy;
}

static void splice_bytes(Bytes *bytes, size_t offset, size_t removed,
    const void *data, size_t length)
{
    assert(offset <= bytes->length && removed <= bytes->length - offset);
    size_t size = bytes->length - removed + length;
    unsigned char *replacement = malloc(size ? size : 1);
    assert(replacement);
    memcpy(replacement, bytes->data, offset);
    if (length)
        memcpy(replacement + offset, data, length);
    memcpy(replacement + offset + length, bytes->data + offset + removed,
        bytes->length - offset - removed);
    free(bytes->data);
    bytes->data = replacement;
    bytes->length = size;
}

static void expect_backend(VirtualBuffer *buffer, const Bytes *expected)
{
    unsigned char actual[4093];
    assert(virtual_buffer_length(buffer) == expected->length);
    size_t end = expected->length;
    while (end) {
        size_t length = end > sizeof(actual) ? sizeof(actual) : end;
        size_t start = end - length;
        assert(virtual_buffer_read(buffer, start, actual, length));
        if (memcmp(actual, expected->data + start, length))
            fprintf(stderr, "backend mismatch near byte %lu\n", (unsigned long)start);
        assert(!memcmp(actual, expected->data + start, length));
        end = start;
    }
    assert(!virtual_buffer_failed(buffer));
}

static void expect_core(EditorCore *core, const Bytes *expected)
{
    size_t length = 0;
    unsigned char actual[4093];
    assert(editor_core_send(core, EDITOR_GET_LENGTH, &length));
    assert(length == expected->length);
    for (size_t offset = 0; offset < length;) {
        size_t count = length - offset;
        if (count > sizeof(actual))
            count = sizeof(actual);
        EditorReadRange range = { offset, actual, count };
        assert(editor_core_send(core, EDITOR_READ_RANGE, &range));
        if (memcmp(actual, expected->data + offset, count))
            fprintf(stderr, "editor mismatch near byte %lu\n", (unsigned long)offset);
        assert(!memcmp(actual, expected->data + offset, count));
        offset += count;
    }
}

static void set_bytes(EditorCore *core, const Bytes *bytes)
{
    EditorBuffer buffer = { bytes->data, bytes->length };
    assert(editor_core_send(core, EDITOR_SET_BUFFER, &buffer));
    expect_core(core, bytes);
}

static void edit_bytes(EditorCore *core, Bytes *expected, size_t offset,
    size_t removed, const void *data, size_t length)
{
    EditorByteEdit edit = { offset, removed, data, length };
    assert(editor_core_send(core, EDITOR_REPLACE_BYTES, &edit));
    splice_bytes(expected, offset, removed, data, length);
    expect_core(core, expected);
}

static void key(EditorCore *core, unsigned long value)
{
    assert(editor_core_send(core, EDITOR_HANDLE_KEY, &value));
}

static void expect_roundtrip(EditorCore *core, const Bytes *before, const Bytes *after)
{
    assert(editor_core_send(core, EDITOR_UNDO, NULL));
    expect_core(core, before);
    assert(editor_core_send(core, EDITOR_REDO, NULL));
    expect_core(core, after);
}

static void expect_no_history(EditorCore *core, const Bytes *expected)
{
    EditorStatus status;
    assert(editor_core_send(core, EDITOR_UNDO, NULL));
    assert(editor_core_send(core, EDITOR_GET_STATUS, &status));
    assert(status == EDITOR_STATUS_NO_CHANGE);
    expect_core(core, expected);
}

static void expect_lines(VirtualBuffer *buffer, const Bytes *expected)
{
    size_t line = 0;
    size_t start = 0;
    for (size_t offset = 0; offset <= expected->length; ++offset) {
        if (offset != expected->length && expected->data[offset] != '\n')
            continue;
        size_t actual_start, actual_end;
        assert(virtual_buffer_line(buffer, line, &actual_start, &actual_end));
        assert(actual_start == start && actual_end == offset);
        start = offset + 1;
        ++line;
    }
    size_t count;
    assert(virtual_buffer_lines(buffer, expected->length, &count));
    assert(count + 1 == line);
}

static void test_sizes_and_failed_writes(void)
{
    const size_t sizes[] = { 0, 1, 65535, 65536, 65537, 131072, 131073 };
    for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
        TestFiles files = { 0 };
        Bytes expected = make_bytes(sizes[index]);
        VirtualBuffer *buffer = virtual_buffer_create();
        assert(buffer);
        virtual_buffer_set_temporary_factory(buffer, &files, create_temporary);
        if (expected.length) {
            files.fail_create = 1;
            assert(!virtual_buffer_replace(buffer, 0, 0, expected.data, expected.length));
            assert(!virtual_buffer_length(buffer) && !virtual_buffer_failed(buffer));
            files.fail_create = 0;
        }
        assert(virtual_buffer_replace(buffer, 0, 0, expected.data, expected.length));
        expect_backend(buffer, &expected);
        expect_lines(buffer, &expected);
        FILE *short_stream = create_temporary(&files);
        assert(short_stream);
        assert(fwrite("abc", 1, 3, short_stream) == 3);
        assert(!fseek(short_stream, 0, SEEK_SET));
        assert(!virtual_buffer_replace_stream(buffer, 0,
            expected.length ? 1 : 0, short_stream, 8));
        assert(!fclose(short_stream));
        expect_backend(buffer, &expected);
        const unsigned char marker = 0xFE;
        assert(virtual_buffer_replace(buffer, expected.length, 0, &marker, 1));
        splice_bytes(&expected, expected.length, 0, &marker, 1);
        expect_backend(buffer, &expected);
        virtual_buffer_destroy(buffer);
        free(expected.data);
    }
}

static void test_splices_and_snapshots(void)
{
    TestFiles files = { 0 };
    Bytes expected = make_bytes(3 * VIRTUAL_BUFFER_CHUNK_SIZE + 37);
    Bytes original = copy_bytes(&expected);
    VirtualBuffer *buffer = virtual_buffer_create();
    assert(buffer);
    virtual_buffer_set_temporary_factory(buffer, &files, create_temporary);
    assert(virtual_buffer_replace(buffer, 0, 0, expected.data, expected.length));
    VirtualBuffer *snapshot = virtual_buffer_clone(buffer);
    assert(snapshot);
    const size_t offsets[] = { 0, 65535, 65536, 65537, 131071, 131072, 131073 };
    const unsigned char insertion[] = { 0, 255, '\r', '\n', 128, 'X', 0 };
    for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        assert(virtual_buffer_replace(buffer, offsets[index], index, insertion, sizeof(insertion)));
        splice_bytes(&expected, offsets[index], index, insertion, sizeof(insertion));
        expect_backend(buffer, &expected);
        expect_backend(snapshot, &original);
    }
    assert(virtual_buffer_replace(buffer, expected.length, 0, insertion, sizeof(insertion)));
    splice_bytes(&expected, expected.length, 0, insertion, sizeof(insertion));
    Bytes streamed = make_bytes(VIRTUAL_BUFFER_CHUNK_SIZE + 19);
    FILE *stream = create_temporary(&files);
    assert(stream);
    assert(fwrite(streamed.data, 1, streamed.length, stream) == streamed.length);
    assert(!fseek(stream, 0, SEEK_SET));
    assert(virtual_buffer_replace_stream(buffer, 65530, 65560, stream, streamed.length));
    splice_bytes(&expected, 65530, 65560, streamed.data, streamed.length);
    assert(!fclose(stream));
    expect_backend(buffer, &expected);
    expect_lines(buffer, &expected);
    assert(!virtual_buffer_replace(buffer, expected.length + 1, 0, insertion, sizeof(insertion)));
    assert(!virtual_buffer_replace(buffer, expected.length, 1, NULL, 0));
    expect_backend(buffer, &expected);
    virtual_buffer_destroy(buffer);
    expect_backend(snapshot, &original);
    assert(virtual_buffer_replace(snapshot, 65535, 4, insertion, sizeof(insertion)));
    splice_bytes(&original, 65535, 4, insertion, sizeof(insertion));
    expect_backend(snapshot, &original);
    assert(virtual_buffer_replace(snapshot, 0, original.length, NULL, 0));
    splice_bytes(&original, 0, original.length, NULL, 0);
    expect_backend(snapshot, &original);
    expect_lines(snapshot, &original);
    virtual_buffer_destroy(snapshot);
    assert(files.created);
    free(streamed.data);
    free(expected.data);
    free(original.data);
}

static uint32_t next_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void test_deterministic_edits(void)
{
    TestFiles files = { 0 };
    Bytes expected = make_bytes(2 * VIRTUAL_BUFFER_CHUNK_SIZE + 97);
    VirtualBuffer *buffer = virtual_buffer_create();
    assert(buffer);
    virtual_buffer_set_temporary_factory(buffer, &files, create_temporary);
    assert(virtual_buffer_replace(buffer, 0, 0, expected.data, expected.length));
    uint32_t random = UINT32_C(0x70617261);
    for (size_t iteration = 0; iteration < 200; ++iteration) {
        unsigned char replacement[129];
        size_t offset = next_random(&random) % (expected.length + 1);
        size_t removed = next_random(&random) % 97;
        size_t length = next_random(&random) % sizeof(replacement);
        if (removed > expected.length - offset)
            removed = expected.length - offset;
        for (size_t index = 0; index < length; ++index)
            replacement[index] = (unsigned char)(next_random(&random) >> 24);
        assert(virtual_buffer_replace(buffer, offset, removed, replacement, length));
        splice_bytes(&expected, offset, removed, replacement, length);
        expect_backend(buffer, &expected);
    }
    expect_lines(buffer, &expected);
    virtual_buffer_destroy(buffer);
    free(expected.data);
}

static void create_path(char path[MAX_PATH])
{
    char directory[MAX_PATH];
    DWORD length = GetTempPathA(MAX_PATH, directory);
    assert(length && length < MAX_PATH);
    assert(GetTempFileNameA(directory, "pvt", 0, path));
}

static void write_file(const char *path, const Bytes *bytes)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(bytes->data, 1, bytes->length, file) == bytes->length);
    assert(!fclose(file));
}

static void expect_file(const char *path, const Bytes *expected)
{
    unsigned char actual[4093];
    FILE *file = fopen(path, "rb");
    assert(file);
    for (size_t offset = 0; offset < expected->length;) {
        size_t length = expected->length - offset;
        if (length > sizeof(actual))
            length = sizeof(actual);
        assert(fread(actual, 1, length, file) == length);
        assert(!memcmp(actual, expected->data + offset, length));
        offset += length;
    }
    assert(fgetc(file) == EOF && !ferror(file));
    assert(!fclose(file));
}

static void test_file_detachment_and_save(void)
{
    char path[MAX_PATH];
    create_path(path);
    TestFiles files = { 0 };
    Bytes expected = make_bytes(3 * VIRTUAL_BUFFER_CHUNK_SIZE + 71);
    Bytes original = copy_bytes(&expected);
    write_file(path, &original);
    VirtualBuffer *buffer = virtual_buffer_open(path);
    assert(buffer);
    virtual_buffer_set_temporary_factory(buffer, &files, create_temporary);
    VirtualBuffer *snapshot = virtual_buffer_clone(buffer);
    assert(snapshot);
    const unsigned char replacement[] = { 0, 254, '\n', '\r', 127 };
    assert(virtual_buffer_replace(buffer, 65534, 31, replacement, sizeof(replacement)));
    splice_bytes(&expected, 65534, 31, replacement, sizeof(replacement));
    assert(virtual_buffer_detach(buffer));
    assert(virtual_buffer_detach(snapshot));
    FILE *output = fopen(path, "wb");
    assert(output);
    assert(virtual_buffer_write(buffer, output, 0, expected.length));
    assert(!fclose(output));
    expect_file(path, &expected);
    expect_backend(buffer, &expected);
    expect_backend(snapshot, &original);
    virtual_buffer_destroy(buffer);
    virtual_buffer_destroy(snapshot);

    write_file(path, &original);
    EditorCore *core = create_core(&files);
    assert(editor_core_send(core, EDITOR_LOAD_FILE, path));
    expect_core(core, &original);
    edit_bytes(core, &original, 65534, 31, replacement, sizeof(replacement));
    assert(editor_core_send(core, EDITOR_SAVE_FILE, path));
    expect_file(path, &expected);
    Bytes initial = make_bytes(3 * VIRTUAL_BUFFER_CHUNK_SIZE + 71);
    expect_roundtrip(core, &initial, &expected);
    EditorCore *reloaded = create_core(&files);
    assert(editor_core_send(reloaded, EDITOR_LOAD_FILE, path));
    expect_core(reloaded, &expected);
    editor_core_destroy(reloaded);
    editor_core_destroy(core);
    assert(DeleteFileA(path));
    free(initial.data);
    free(expected.data);
    free(original.data);
}

static void test_editor_edits(void)
{
    TestFiles files = { 0 };
    EditorCore *core = create_core(&files);
    Bytes expected = make_bytes(3 * VIRTUAL_BUFFER_CHUNK_SIZE + 43);
    set_bytes(core, &expected);
    const unsigned char replacement[] = { 255, 0, '\n', '\r', 128 };
    const size_t offsets[] = { 65535, 65536, 65537, 131071, 131072 };
    for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        Bytes before = copy_bytes(&expected);
        edit_bytes(core, &expected, offsets[index], 7, replacement, sizeof(replacement));
        expect_roundtrip(core, &before, &expected);
        free(before.data);
    }
    free(expected.data);
    expected = make_bytes(2 * VIRTUAL_BUFFER_CHUNK_SIZE + 31);
    memset(expected.data, 'a', expected.length);
    set_bytes(core, &expected);
    size_t offset = VIRTUAL_BUFFER_CHUNK_SIZE - 1;
    Bytes before = copy_bytes(&expected);
    assert(editor_core_send(core, EDITOR_SET_OFFSET, &offset));
    key(core, 'i');
    key(core, 'X');
    key(core, 'Y');
    key(core, 'Z');
    key(core, 27);
    splice_bytes(&expected, offset, 0, "XYZ", 3);
    expect_core(core, &expected);
    expect_roundtrip(core, &before, &expected);
    free(before.data);
    before = copy_bytes(&expected);
    assert(editor_core_send(core, EDITOR_SET_OFFSET, &offset));
    key(core, 'R');
    key(core, 'Q');
    key(core, 'Z');
    key(core, 27);
    splice_bytes(&expected, offset, 2, "QZ", 2);
    expect_core(core, &expected);
    expect_roundtrip(core, &before, &expected);
    free(before.data);
    before = copy_bytes(&expected);
    assert(editor_core_send(core, EDITOR_SET_OFFSET, &offset));
    key(core, 'x');
    splice_bytes(&expected, offset, 1, NULL, 0);
    expect_core(core, &expected);
    expect_roundtrip(core, &before, &expected);
    free(before.data);
    free(expected.data);
    editor_core_destroy(core);
}

static void test_mapped_substitution(void)
{
    TestFiles files = { 0 };
    EditorCore *core = create_core(&files);
    Bytes expected = make_bytes(2 * VIRTUAL_BUFFER_CHUNK_SIZE + 61);
    memset(expected.data, 'a', expected.length);
    expected.data[100] = 0;
    expected.data[101] = 255;
    memcpy(expected.data + 65534, "QxyxyZ", 6);
    memcpy(expected.data + 131070, "QxyxyZ", 6);
    size_t last_match = expected.length + 3;
    const char tail[] = "\r\nQxyxyZ\nlast";
    splice_bytes(&expected, expected.length, 0, tail, sizeof(tail) - 1);
    Bytes before = copy_bytes(&expected);
    set_bytes(core, &expected);
    const char *command = "%s/(?<=Q)(xy)\\1(?=Z)/\\1!/g";
    assert(editor_core_send(core, EDITOR_EXECUTE_COMMAND, (void *)command));
    splice_bytes(&expected, last_match, 4, "xy!", 3);
    splice_bytes(&expected, 131071, 4, "xy!", 3);
    splice_bytes(&expected, 65535, 4, "xy!", 3);
    expect_core(core, &expected);
    expect_roundtrip(core, &before, &expected);
    assert(files.mapped && files.mapped == files.unmapped);
    editor_core_destroy(core);
    free(before.data);
    free(expected.data);
}

static void test_failed_subject_is_atomic(void)
{
    TestFiles files = { 0 };
    EditorCore *core = create_core(&files);
    Bytes expected = make_bytes(VIRTUAL_BUFFER_CHUNK_SIZE + 29);
    memset(expected.data, 'a', expected.length);
    splice_bytes(&expected, 0, 0, "a\n", 2);
    set_bytes(core, &expected);
    files.fail_map = 1;
    assert(!editor_core_send(core, EDITOR_EXECUTE_COMMAND, "%s/a/b/g"));
    expect_core(core, &expected);
    EditorStatus status;
    int modified = 1;
    assert(editor_core_send(core, EDITOR_GET_STATUS, &status));
    assert(status == EDITOR_STATUS_SUBSTITUTION_FAILED);
    assert(editor_core_send(core, EDITOR_GET_MODIFIED, &modified) && !modified);
    expect_no_history(core, &expected);
    files.fail_map = 0;
    files.fail_create = 1;
    assert(!editor_core_send(core, EDITOR_EXECUTE_COMMAND, "%s/a/b/g"));
    expect_core(core, &expected);
    expect_no_history(core, &expected);
    editor_core_destroy(core);
    free(expected.data);
}

static Bytes convert_endings(const Bytes *input, int crlf)
{
    Bytes output = make_bytes(2 * input->length);
    output.length = 0;
    for (size_t offset = 0; offset < input->length; ++offset) {
        unsigned char value = input->data[offset];
        if (value == '\r' && offset + 1 < input->length && input->data[offset + 1] == '\n')
            continue;
        if (value == '\n' && crlf)
            output.data[output.length++] = '\r';
        output.data[output.length++] = value;
    }
    return output;
}

static void test_line_ending_changes(void)
{
    TestFiles files = { 0 };
    EditorCore *core = create_core(&files);
    Bytes original = make_bytes(3 * VIRTUAL_BUFFER_CHUNK_SIZE + 37);
    memset(original.data, 'q', original.length);
    original.data[2] = 0;
    original.data[3] = 255;
    original.data[4] = 0xC3;
    original.data[5] = 0xA4;
    memcpy(original.data + 65535, "\r\n", 2);
    original.data[131071] = '\n';
    memcpy(original.data + 131082, "\r\n", 2);
    memcpy(original.data + original.length - 6, "\r\ntail", 6);
    set_bytes(core, &original);
    Bytes lf = convert_endings(&original, 0);
    Bytes crlf = convert_endings(&lf, 1);
    assert(editor_core_send(core, EDITOR_EXECUTE_COMMAND, "set lf"));
    expect_core(core, &lf);
    EditorLineEnding ending;
    assert(editor_core_send(core, EDITOR_GET_LINE_ENDING, &ending));
    assert(ending == EDITOR_LINE_ENDING_LF);
    expect_roundtrip(core, &original, &lf);
    assert(editor_core_send(core, EDITOR_EXECUTE_COMMAND, "set crlf"));
    expect_core(core, &crlf);
    assert(editor_core_send(core, EDITOR_GET_LINE_ENDING, &ending));
    assert(ending == EDITOR_LINE_ENDING_CRLF);
    expect_roundtrip(core, &lf, &crlf);
    char path[MAX_PATH];
    create_path(path);
    assert(editor_core_send(core, EDITOR_SAVE_FILE, path));
    expect_file(path, &crlf);
    assert(editor_core_send(core, EDITOR_LOAD_FILE, path));
    expect_core(core, &crlf);
    editor_core_destroy(core);
    assert(DeleteFileA(path));
    free(original.data);
    free(lf.data);
    free(crlf.data);
}

static LexerTokenKind token_at(const LexerLine *line, size_t offset)
{
    for (size_t index = 0; index < line->count; ++index) {
        const LexerToken *token = &line->tokens[index];
        if (offset >= token->start && offset - token->start < token->length)
            return token->kind;
    }
    assert(0);
    return LEXER_TEXT;
}

static void expect_highlighting(EditorCore *core, const Bytes *expected, LexerTokenKind tail_kind)
{
    const Lexer *lexer = NULL;
    assert(editor_core_send(core, EDITOR_GET_LEXER, (void *)&lexer));
    assert(lexer == &lexer_c_cpp);
    LexerLine previous = { 0 }, reference_previous = { 0 };
    size_t offset = 0, line_index = 0;
    for (;;) {
        size_t end = offset;
        while (end < expected->length && expected->data[end] != '\n')
            ++end;
        EditorLine line = { 0 };
        line.index = line_index;
        assert(editor_core_send(core, EDITOR_GET_LINE, &line));
        assert(!line.truncated && line.length == end - offset);
        assert(!memcmp(line.text, expected->data + offset, line.length));
        LexerLine actual = { 0 }, reference = { 0 };
        assert(lexer->lex_line(line.text, line.length, previous.end_state, &actual));
        assert(lexer->lex_line((const char *)expected->data + offset,
            end - offset, reference_previous.end_state, &reference));
        assert(actual.count == reference.count);
        assert(lexer->states_equal(actual.end_state, reference.end_state));
        for (size_t index = 0; index < actual.count; ++index) {
            assert(actual.tokens[index].start == reference.tokens[index].start);
            assert(actual.tokens[index].length == reference.tokens[index].length);
            assert(actual.tokens[index].kind == reference.tokens[index].kind);
            assert(actual.tokens[index].start <= line.length);
            assert(actual.tokens[index].length <= line.length - actual.tokens[index].start);
        }
        if (line.length >= 13 && !memcmp(line.text, "still comment", 13))
            assert(token_at(&actual, 0) == LEXER_COMMENT);
        if (line.length >= 4 && !memcmp(line.text, "int ", 4)) {
            LexerTokenKind kind = line.length >= 8 && !memcmp(line.text, "int tail", 8)
                ? tail_kind : LEXER_KEYWORD;
            assert(token_at(&actual, 0) == kind);
            if (kind == LEXER_KEYWORD)
                assert(token_at(&actual, line.length - 2) == LEXER_NUMBER);
        }
        if (line.length >= 6 && !memcmp(line.text, "*/ int", 6)) {
            assert(token_at(&actual, 3) == LEXER_KEYWORD);
            assert(token_at(&actual, 16) == LEXER_NUMBER);
            const char *quote = memchr(line.text, '"', line.length);
            assert(quote && token_at(&actual, (size_t)(quote - line.text)) == LEXER_STRING);
        }
        lexer->free_line(&previous);
        lexer->free_line(&reference_previous);
        previous = actual;
        reference_previous = reference;
        ++line_index;
        if (end == expected->length)
            break;
        offset = end + 1;
    }
    size_t count;
    assert(editor_core_send(core, EDITOR_GET_LINE_COUNT, &count));
    assert(count == line_index);
    lexer->free_line(&previous);
    lexer->free_line(&reference_previous);
    expect_core(core, expected);
}

static void test_virtual_highlighting(void)
{
    const char *prefix = "int n = 7;\n";
    const char *middle = "/* open\nstill comment\n*/ int answer = 42; const char *s = \"ok\";\n";
    const char *tail = "int tail = 99;\n";
    size_t repeats = (VIRTUAL_BUFFER_CHUNK_SIZE - 9) / strlen(prefix);
    size_t prefix_length = repeats * strlen(prefix);
    Bytes expected = make_bytes(prefix_length + strlen(middle) + repeats * strlen(tail));
    for (size_t index = 0; index < repeats; ++index) {
        memcpy(expected.data + index * strlen(prefix), prefix, strlen(prefix));
        memcpy(expected.data + prefix_length + strlen(middle) + index * strlen(tail), tail, strlen(tail));
    }
    memcpy(expected.data + prefix_length, middle, strlen(middle));
    TestFiles files = { 0 };
    EditorCore *core = create_core(&files);
    set_bytes(core, &expected);
    assert(editor_core_send(core, EDITOR_SELECT_LEXER, "virtual.c"));
    expect_highlighting(core, &expected, LEXER_KEYWORD);
    Bytes before = copy_bytes(&expected);
    size_t closing = prefix_length + strlen("/* open\nstill comment\n");
    edit_bytes(core, &expected, closing, 2, "  ", 2);
    expect_highlighting(core, &expected, LEXER_COMMENT);
    assert(editor_core_send(core, EDITOR_UNDO, NULL));
    expect_highlighting(core, &before, LEXER_KEYWORD);
    assert(editor_core_send(core, EDITOR_REDO, NULL));
    expect_highlighting(core, &expected, LEXER_COMMENT);
    editor_core_destroy(core);
    free(before.data);
    free(expected.data);
}

int main(void)
{
    test_sizes_and_failed_writes();
    test_splices_and_snapshots();
    test_deterministic_edits();
    test_file_detachment_and_save();
    test_editor_edits();
    test_mapped_substitution();
    test_failed_subject_is_atomic();
    test_line_ending_changes();
    test_virtual_highlighting();
    puts("Virtual buffer integrity, editing, regex, line-ending and highlighting tests passed.");
    return 0;
}