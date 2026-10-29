#include "lexer_c_cpp.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int identifier_start(unsigned char character)
{
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') || character == '_' || character >= 128;
}

static int digit(unsigned char character)
{
    return character >= '0' && character <= '9';
}

static int identifier_part(unsigned char character)
{
    return identifier_start(character) || digit(character);
}

static int keyword(const char *text, size_t length)
{
    static const char *const words[] = {
        "_Alignas", "_Alignof", "_Atomic", "_BitInt", "_Bool", "_Complex", "_Decimal128",
        "_Decimal32", "_Decimal64", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
        "_Thread_local", "_Pragma", "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
        "bitor", "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
        "class", "co_await", "co_return", "co_yield", "compl", "concept", "const", "const_cast",
        "consteval", "constexpr", "constinit", "continue", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
        "false", "final", "float", "for", "friend", "goto", "if", "inline", "int", "long",
        "import", "module", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator",
        "or", "or_eq", "override", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "restrict", "return", "short", "signed", "sizeof",
        "static", "static_assert", "static_cast", "struct", "switch", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "typeof",
        "typeof_unqual", "union", "unsigned", "using", "virtual", "void", "volatile",
        "wchar_t", "while", "xor", "xor_eq"
    };
    for (size_t index = 0; index < sizeof(words) / sizeof(words[0]); ++index)
        if (strlen(words[index]) == length && memcmp(text, words[index], length) == 0)
            return 1;
    return 0;
}

static int add_token(LexerCCppLine *line, size_t start, size_t end, LexerCCppKind kind)
{
    if (start == end)
        return 1;
    if (line->count && line->tokens[line->count - 1].kind == kind) {
        line->tokens[line->count - 1].length += end - start;
        return 1;
    }
    if (line->count == line->capacity) {
        size_t capacity = line->capacity ? line->capacity * 2 : 16;
        if (capacity < line->capacity || capacity > SIZE_MAX / sizeof(*line->tokens))
            return 0;
        LexerCCppToken *tokens = realloc(line->tokens, capacity * sizeof(*tokens));
        if (!tokens)
            return 0;
        line->tokens = tokens;
        line->capacity = capacity;
    }
    LexerCCppToken token = { start, end - start, kind };
    line->tokens[line->count++] = token;
    return 1;
}

static int literal_prefix(const char *text, size_t length, size_t *width, int *raw)
{
    static const char *const prefixes[] = { "u8R\"", "uR\"", "UR\"", "LR\"", "R\"",
        "u8\"", "u\"", "U\"", "L\"", "\"", "u8'", "u'", "U'", "L'", "'" };
    for (size_t index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
        size_t size = strlen(prefixes[index]);
        if (size <= length && memcmp(text, prefixes[index], size) == 0) {
            *width = size;
            *raw = index < 5;
            return 1;
        }
    }
    return 0;
}

int lexer_c_cpp_state_equal(const LexerCCppState *left, const LexerCCppState *right)
{
    return left->mode == right->mode && left->directive == right->directive &&
        left->escape == right->escape && left->delimiter_length == right->delimiter_length &&
        memcmp(left->delimiter, right->delimiter, left->delimiter_length) == 0;
}

void lexer_c_cpp_free_line(LexerCCppLine *line)
{
    free(line->tokens);
    memset(line, 0, sizeof(*line));
}

int lexer_c_cpp_line(const char *text, size_t length, LexerCCppState state, LexerCCppLine *result)
{
    if (!result || (!text && length) || state.delimiter_length > 16)
        return 0;
    LexerCCppLine line = { 0 };
    size_t end = length;
    if (end && text[end - 1] == '\r')
        --end;
    int spliced = end && text[end - 1] == '\\';
    int at_start = !state.directive &&
        (state.mode == LEXER_C_CPP_NORMAL || state.mode == LEXER_C_CPP_BLOCK_COMMENT);
    int expect_header = 0;
    size_t position = 0;
    while (position < length) {
        size_t start = position;
        LexerCCppKind kind = state.directive ? LEXER_C_CPP_PREPROCESSOR : LEXER_C_CPP_TEXT;
        if (state.mode == LEXER_C_CPP_NORMAL) {
            unsigned char character = (unsigned char)text[position];
            if (character == ' ' || character == '\t' || character == '\r' || character == '\f') {
                ++position;
            } else if (position + 1 < length && character == '/' && text[position + 1] == '*') {
                state.mode = LEXER_C_CPP_BLOCK_COMMENT;
                position += 2;
            } else if (position + 1 < length && character == '/' && text[position + 1] == '/') {
                state.mode = LEXER_C_CPP_LINE_COMMENT;
                position += 2;
            } else if (at_start && character == '#') {
                state.directive = 1;
                kind = LEXER_C_CPP_PREPROCESSOR;
                ++position;
                while (position < length && (text[position] == ' ' || text[position] == '\t'))
                    ++position;
                size_t word = position;
                while (position < length && identifier_part((unsigned char)text[position]))
                    ++position;
                size_t size = position - word;
                expect_header = (size == 7 && memcmp(text + word, "include", 7) == 0) ||
                    (size == 12 && memcmp(text + word, "include_next", 12) == 0);
                at_start = 0;
            } else if (expect_header && character == '<') {
                state.mode = LEXER_C_CPP_HEADER;
                ++position;
                at_start = 0;
                expect_header = 0;
            } else {
                size_t prefix = 0;
                int raw = 0;
                at_start = 0;
                expect_header = 0;
                if (literal_prefix(text + position, length - position, &prefix, &raw)) {
                    size_t delimiter = position + prefix;
                    size_t opening = delimiter;
                    if (raw) {
                        while (opening < end && opening - delimiter <= 16 && text[opening] != '(' &&
                            (unsigned char)text[opening] > ' ' && text[opening] != ')' && text[opening] != '\\')
                            ++opening;
                    }
                    if (raw && opening < end && text[opening] == '(' && opening - delimiter <= 16) {
                        state.delimiter_length = opening - delimiter;
                        memcpy(state.delimiter, text + delimiter, state.delimiter_length);
                        state.mode = LEXER_C_CPP_RAW_STRING;
                        position = opening + 1;
                    } else if (!raw) {
                        state.mode = text[position + prefix - 1] == '\'' ?
                            LEXER_C_CPP_CHARACTER : LEXER_C_CPP_QUOTED_STRING;
                        state.escape = 0;
                        position += prefix;
                    } else {
                        ++position;
                    }
                } else if (identifier_start(character)) {
                    do {
                        ++position;
                    } while (position < length && identifier_part((unsigned char)text[position]));
                    if (!state.directive && keyword(text + start, position - start))
                        kind = LEXER_C_CPP_KEYWORD;
                } else if (digit(character) ||
                    (character == '.' && position + 1 < length && digit((unsigned char)text[position + 1]))) {
                    ++position;
                    while (position < length) {
                        unsigned char next = (unsigned char)text[position];
                        if (identifier_part(next) || next == '.' || next == '\'' ||
                            ((next == '+' || next == '-') &&
                                (text[position - 1] == 'e' || text[position - 1] == 'E' ||
                                    text[position - 1] == 'p' || text[position - 1] == 'P')))
                            ++position;
                        else
                            break;
                    }
                    kind = LEXER_C_CPP_NUMBER;
                } else {
                    ++position;
                }
            }
        }
        if (state.mode == LEXER_C_CPP_BLOCK_COMMENT) {
            kind = LEXER_C_CPP_COMMENT;
            while (position < length) {
                if (position + 1 < length && text[position] == '*' && text[position + 1] == '/') {
                    position += 2;
                    state.mode = LEXER_C_CPP_NORMAL;
                    break;
                }
                ++position;
            }
        } else if (state.mode == LEXER_C_CPP_LINE_COMMENT) {
            kind = LEXER_C_CPP_COMMENT;
            position = length;
            if (!spliced)
                state.mode = LEXER_C_CPP_NORMAL;
        } else if (state.mode == LEXER_C_CPP_RAW_STRING) {
            kind = LEXER_C_CPP_STRING;
            while (position < length) {
                if (text[position] == ')' && length - position >= state.delimiter_length + 2 &&
                    memcmp(text + position + 1, state.delimiter, state.delimiter_length) == 0 &&
                    text[position + state.delimiter_length + 1] == '"') {
                    position += state.delimiter_length + 2;
                    state.mode = LEXER_C_CPP_NORMAL;
                    state.delimiter_length = 0;
                    break;
                }
                ++position;
            }
        } else if (state.mode != LEXER_C_CPP_NORMAL) {
            kind = LEXER_C_CPP_STRING;
            char quote = state.mode == LEXER_C_CPP_CHARACTER ? '\'' :
                (state.mode == LEXER_C_CPP_HEADER ? '>' : '"');
            while (position < end) {
                char character = text[position++];
                if (state.escape) {
                    state.escape = 0;
                } else if (character == '\\') {
                    state.escape = 1;
                } else if (character == quote) {
                    state.mode = LEXER_C_CPP_NORMAL;
                    break;
                }
            }
            if (position == end && position < length)
                ++position;
        }
        if (!add_token(&line, start, position, kind)) {
            lexer_c_cpp_free_line(&line);
            return 0;
        }
    }
    if (state.mode == LEXER_C_CPP_QUOTED_STRING || state.mode == LEXER_C_CPP_CHARACTER ||
        state.mode == LEXER_C_CPP_HEADER) {
        if (!spliced) {
            state.mode = LEXER_C_CPP_NORMAL;
            state.escape = 0;
        } else {
            size_t slashes = 0;
            while (slashes < end && text[end - slashes - 1] == '\\')
                ++slashes;
            state.escape = (int)((slashes - 1) % 2);
        }
    }
    if (!spliced && state.mode == LEXER_C_CPP_LINE_COMMENT)
        state.mode = LEXER_C_CPP_NORMAL;
    if (!spliced)
        state.directive = 0;
    line.end_state = state;
    *result = line;
    return 1;
}

static int supports_extension(const char *extension)
{
    static const char *const extensions[] = { ".c", ".h", ".hpp" };
    if (!extension)
        return 0;
    for (size_t index = 0; index < sizeof(extensions) / sizeof(extensions[0]); ++index) {
        const char *candidate = extensions[index];
        size_t position = 0;
        while (extension[position] && candidate[position]) {
            unsigned char character = (unsigned char)extension[position];
            if (character >= 'A' && character <= 'Z')
                character += 'a' - 'A';
            if (character != (unsigned char)candidate[position])
                break;
            ++position;
        }
        if (!extension[position] && !candidate[position])
            return 1;
    }
    return 0;
}

static int lex_line(const char *text, size_t length, const void *state, LexerLine *result)
{
    if (!result)
        return 0;
    LexerCCppState initial = { 0 };
    if (state)
        initial = *(const LexerCCppState *)state;
    LexerCCppLine parsed = { 0 };
    if (!lexer_c_cpp_line(text, length, initial, &parsed))
        return 0;
    LexerCCppState *ending = malloc(sizeof(*ending));
    if (!ending) {
        lexer_c_cpp_free_line(&parsed);
        return 0;
    }
    *ending = parsed.end_state;
    result->tokens = parsed.tokens;
    result->count = parsed.count;
    result->end_state = ending;
    return 1;
}

static int states_equal(const void *left, const void *right)
{
    if (!left || !right)
        return left == right;
    return lexer_c_cpp_state_equal(left, right);
}

static void free_line(LexerLine *line)
{
    free(line->tokens);
    free(line->end_state);
    memset(line, 0, sizeof(*line));
}

const Lexer lexer_c_cpp = {
    "C/C++", supports_extension, lex_line, states_equal, free_line
};