#include "lexer_pascal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct PascalState {
    int block;
    int directive;
} PascalState;

static unsigned char lower_ascii(unsigned char character)
{
    return character >= 'A' && character <= 'Z'
        ? (unsigned char)(character + ('a' - 'A')) : character;
}

static int word_equal(const char *text, size_t length, const char *word)
{
    if (strlen(word) != length)
        return 0;
    for (size_t index = 0; index < length; ++index) {
        if (lower_ascii((unsigned char)text[index]) != (unsigned char)word[index])
            return 0;
    }
    return 1;
}

static int word_start(unsigned char character)
{
    character = lower_ascii(character);
    return (character >= 'a' && character <= 'z') || character == '_' || character >= 128;
}

static int digit_value(unsigned char character)
{
    character = lower_ascii(character);
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    return 16;
}

static size_t digits_end(const char *text, size_t length, size_t position, int base)
{
    while (position < length) {
        if (digit_value((unsigned char)text[position]) < base) {
            ++position;
        } else if (text[position] == '_' && position + 1 < length &&
            digit_value((unsigned char)text[position + 1]) < base) {
            position += 2;
        } else {
            break;
        }
    }
    return position;
}

static void free_line(LexerLine *line)
{
    free(line->tokens);
    free(line->end_state);
    memset(line, 0, sizeof(*line));
}

static int append_token(LexerLine *line, size_t *capacity, size_t start,
    size_t length, LexerTokenKind kind)
{
    if (!length)
        return 1;
    if (line->count && line->tokens[line->count - 1].kind == kind) {
        line->tokens[line->count - 1].length += length;
        return 1;
    }
    if (line->count == *capacity) {
        if (*capacity > SIZE_MAX / sizeof(*line->tokens) / 2)
            return 0;
        size_t next = *capacity ? *capacity * 2 : 16;
        LexerToken *tokens = realloc(line->tokens, next * sizeof(*tokens));
        if (!tokens)
            return 0;
        line->tokens = tokens;
        *capacity = next;
    }
    LexerToken *token = &line->tokens[line->count++];
    token->start = start;
    token->length = length;
    token->kind = kind;
    return 1;
}

static int lex_line(const char *text, size_t length, const void *state, LexerLine *result)
{
    static const char *const keywords[] = {
        "absolute", "abstract", "and", "ansichar", "ansistring", "array", "as", "asm", "assembler", "automated",
        "begin", "boolean", "break", "byte", "cardinal", "case", "cdecl", "char", "class", "comp", "const",
        "constructor", "contains", "continue", "currency", "default", "deprecated",
        "destructor", "dispid", "dispinterface", "div", "do", "double", "downto",
        "dynamic", "else", "end", "except", "exit", "export", "exports", "extended", "external",
        "false", "far", "file", "final", "finalization", "finally", "for", "forward",
        "function", "generic", "goto", "helper", "if", "implementation", "implements",
        "in", "index", "inherited", "initialization", "inline", "int64", "integer",
        "interface", "is", "label", "library", "local", "longint", "longword", "message",
        "mod", "name", "nativeint", "nativeuint", "near", "nil", "nodefault", "not",
        "object", "of", "on", "operator", "or", "out", "overload", "override", "package",
        "packed", "pascal", "platform", "pointer", "private", "procedure", "program",
        "property", "protected", "public", "published", "raise", "read", "readonly",
        "real", "record", "reference", "register", "reintroduce", "repeat", "requires",
        "resident", "resourcestring", "result", "safecall", "sealed", "self", "set",
        "shl", "shortint", "shortstring", "shr", "single", "smallint", "specialize",
        "static", "stdcall", "stored", "strict", "string", "then", "threadvar", "to",
        "true", "try", "type", "uint64", "unicodestring", "unit", "unsafe", "until", "uses", "var",
        "varargs", "variant", "virtual", "while", "widechar", "widestring", "with",
        "word", "write", "writeonly", "xor"
    };
    if (!result || (!text && length))
        return 0;
    PascalState current = { 0, 0 };
    if (state)
        current = *(const PascalState *)state;
    LexerLine line = { 0 };
    size_t capacity = 0;
    size_t position = 0;
    while (position < length) {
        size_t start = position;
        unsigned char character = (unsigned char)text[position];
        LexerTokenKind kind = LEXER_TEXT;
        if (!current.block && (character == '{' ||
            (character == '(' && position + 1 < length && text[position + 1] == '*'))) {
            current.block = character == '{' ? 1 : 2;
            position += current.block == 1 ? 1 : 2;
            current.directive = position < length && text[position] == '$';
        }
        if (current.block) {
            kind = current.directive ? LEXER_PREPROCESSOR : LEXER_COMMENT;
            while (position < length) {
                if (current.block == 1 && text[position] == '}') {
                    ++position;
                    current.block = current.directive = 0;
                    break;
                }
                if (current.block == 2 && text[position] == '*' &&
                    position + 1 < length && text[position + 1] == ')') {
                    position += 2;
                    current.block = current.directive = 0;
                    break;
                }
                ++position;
            }
        } else if (character == '/' && position + 1 < length && text[position + 1] == '/') {
            kind = LEXER_COMMENT;
            position = length;
        } else if (character == '\'') {
            kind = LEXER_STRING;
            ++position;
            while (position < length) {
                if (text[position++] == '\'') {
                    if (position == length || text[position] != '\'')
                        break;
                    ++position;
                }
            }
        } else if (word_start(character) || (character == '&' && position + 1 < length &&
            word_start((unsigned char)text[position + 1]))) {
            int escaped = character == '&';
            position += escaped ? 2 : 1;
            while (position < length && (word_start((unsigned char)text[position]) ||
                digit_value((unsigned char)text[position]) < 10))
                ++position;
            if (!escaped) {
                for (size_t index = 0; index < sizeof(keywords) / sizeof(keywords[0]); ++index) {
                    if (word_equal(text + start, position - start, keywords[index])) {
                        kind = LEXER_KEYWORD;
                        break;
                    }
                }
            }
        } else {
            size_t number = position;
            int base = 10;
            int character_code = character == '#';
            if (character_code)
                ++number;
            if (number < length && (text[number] == '$' || text[number] == '%' || text[number] == '&')) {
                base = text[number] == '$' ? 16 : text[number] == '%' ? 2 : 8;
                ++number;
            }
            if (number < length && digit_value((unsigned char)text[number]) < base) {
                kind = character_code ? LEXER_STRING : LEXER_NUMBER;
                position = digits_end(text, length, number, base);
                if (!character_code && base == 10) {
                    if (position < length && text[position] == '.' &&
                        (position + 1 == length || text[position + 1] != '.'))
                        position = digits_end(text, length, position + 1, 10);
                    if (position < length && lower_ascii((unsigned char)text[position]) == 'e') {
                        size_t exponent = position + 1;
                        if (exponent < length && (text[exponent] == '+' || text[exponent] == '-'))
                            ++exponent;
                        if (exponent < length && digit_value((unsigned char)text[exponent]) < 10)
                            position = digits_end(text, length, exponent, 10);
                    }
                }
            } else {
                ++position;
            }
        }
        if (!append_token(&line, &capacity, start, position - start, kind))
            goto failure;
    }
    if (current.block) {
        PascalState *ending = malloc(sizeof(*ending));
        if (!ending)
            goto failure;
        *ending = current;
        line.end_state = ending;
    }
    *result = line;
    return 1;
failure:
    free_line(&line);
    return 0;
}

static int states_equal(const void *left, const void *right)
{
    if (!left || !right)
        return left == right;
    const PascalState *left_state = left;
    const PascalState *right_state = right;
    return left_state->block == right_state->block && left_state->directive == right_state->directive;
}

static int supports_extension(const char *extension)
{
    return extension && (word_equal(extension, strlen(extension), ".pas") ||
        word_equal(extension, strlen(extension), ".dpr"));
}

const Lexer lexer_pascal = {
    "Pascal/Delphi", supports_extension, lex_line, states_equal, free_line, NULL
};