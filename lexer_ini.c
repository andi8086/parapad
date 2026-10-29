#include "lexer_ini.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static int space(unsigned char character)
{
    return character == ' ' || character == '\t' || character == '\r';
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

static int number_literal(const char *text, size_t length)
{
    size_t position = 0;
    if (position < length && (text[position] == '+' || text[position] == '-'))
        ++position;
    if (length - position >= 2 && text[position] == '0' &&
        lower_ascii((unsigned char)text[position + 1]) == 'x') {
        position += 2;
        size_t first = position;
        while (position < length && digit_value((unsigned char)text[position]) < 16)
            ++position;
        return position > first && position == length;
    }
    size_t digits = 0;
    while (position < length && digit_value((unsigned char)text[position]) < 10) {
        ++position;
        ++digits;
    }
    if (position < length && text[position] == '.') {
        ++position;
        while (position < length && digit_value((unsigned char)text[position]) < 10) {
            ++position;
            ++digits;
        }
    }
    if (!digits)
        return 0;
    if (position < length && lower_ascii((unsigned char)text[position]) == 'e') {
        ++position;
        if (position < length && (text[position] == '+' || text[position] == '-'))
            ++position;
        size_t first = position;
        while (position < length && digit_value((unsigned char)text[position]) < 10)
            ++position;
        if (position == first)
            return 0;
    }
    return position == length;
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
    static const char *const values[] = { "true", "false", "yes", "no", "on", "off", "null" };
    (void)state;
    if (!result || (!text && length))
        return 0;
    LexerLine line = { 0 };
    size_t capacity = 0;
    size_t first = length >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3) ? 3 : 0;
    while (first < length && space((unsigned char)text[first]))
        ++first;
    int comment = first < length && (text[first] == ';' || text[first] == '#');
    int section = first < length && text[first] == '[';
    size_t section_end = first;
    size_t separator = length;
    size_t key_end = first;
    if (section) {
        const char *closing = memchr(text + first + 1, ']', length - first - 1);
        section_end = closing ? (size_t)(closing - text) + 1 : length;
    } else if (!comment && first < length) {
        const char *equals = memchr(text + first, '=', length - first);
        if (equals) {
            separator = (size_t)(equals - text);
            key_end = separator;
            while (key_end > first && space((unsigned char)text[key_end - 1]))
                --key_end;
        }
    }
    size_t position = 0;
    while (position < length) {
        size_t start = position;
        unsigned char character = (unsigned char)text[position];
        LexerTokenKind kind = LEXER_TEXT;
        if (position < first) {
            position = first;
        } else if ((comment && position == first) ||
            (character == ';' && position && space((unsigned char)text[position - 1]))) {
            kind = LEXER_COMMENT;
            position = length;
        } else if (section && position == first) {
            kind = LEXER_PREPROCESSOR;
            position = section_end;
        } else if (position == first && key_end > first) {
            kind = LEXER_KEYWORD;
            position = key_end;
        } else if (position == separator) {
            kind = LEXER_PREPROCESSOR;
            ++position;
        } else if (character == '"' || character == '\'') {
            kind = LEXER_STRING;
            ++position;
            while (position < length) {
                if ((unsigned char)text[position++] == character) {
                    if (position == length || (unsigned char)text[position] != character)
                        break;
                    ++position;
                }
            }
        } else if (space(character)) {
            ++position;
        } else {
            ++position;
            while (position < length && position != separator &&
                !space((unsigned char)text[position]) && text[position] != '"' && text[position] != '\'')
                ++position;
            if (number_literal(text + start, position - start)) {
                kind = LEXER_NUMBER;
            } else {
                for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
                    if (word_equal(text + start, position - start, values[index])) {
                        kind = LEXER_KEYWORD;
                        break;
                    }
                }
            }
        }
        if (!append_token(&line, &capacity, start, position - start, kind)) {
            free_line(&line);
            return 0;
        }
    }
    *result = line;
    return 1;
}

static int states_equal(const void *left, const void *right)
{
    return left == right;
}

static int supports_extension(const char *extension)
{
    return extension && word_equal(extension, strlen(extension), ".ini");
}

const Lexer lexer_ini = {
    "INI", supports_extension, lex_line, states_equal, free_line, NULL
};