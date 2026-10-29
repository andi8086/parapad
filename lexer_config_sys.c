#include "lexer_config_sys.h"

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

static int in_words(const char *text, size_t length, const char *const *words, size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        if (word_equal(text, length, words[index]))
            return 1;
    }
    return 0;
}

static int space(unsigned char character)
{
    return character == ' ' || character == '\t' || character == '\r';
}

static int number_literal(const char *text, size_t length)
{
    size_t position = 0;
    int base = 10;
    if (position < length && (text[position] == '+' || text[position] == '-'))
        ++position;
    if (length - position >= 2 && text[position] == '0' &&
        lower_ascii((unsigned char)text[position + 1]) == 'x') {
        base = 16;
        position += 2;
    } else if (length > position && lower_ascii((unsigned char)text[length - 1]) == 'h') {
        base = 16;
        --length;
    }
    size_t first = position;
    while (position < length) {
        unsigned char character = lower_ascii((unsigned char)text[position]);
        if (!((character >= '0' && character <= '9') ||
            (base == 16 && character >= 'a' && character <= 'f')))
            return 0;
        ++position;
    }
    return position > first;
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
    static const char *const commands[] = {
        "accdate", "availdev", "break", "buffers", "buffershigh", "country", "device",
        "devicehigh", "dos", "dosdata", "drivparm", "fcbs", "fcbshigh", "files",
        "fileshigh", "include", "install", "installhigh", "lastdrive", "lastdrivehigh",
        "menucolor", "menudefault", "menuitem", "numlock", "rem", "set", "shell",
        "stacks", "stackshigh", "submenu", "switchar", "switches"
    };
    static const char *const values[] = {
        "on", "off", "yes", "no", "high", "low", "umb", "noumb", "auto", "noauto", "single"
    };
    (void)state;
    if (!result || (!text && length))
        return 0;
    LexerLine line = { 0 };
    size_t capacity = 0;
    size_t first = length >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3) ? 3 : 0;
    while (first < length && space((unsigned char)text[first]))
        ++first;
    size_t command_start = first;
    while (command_start < length && (text[command_start] == '?' || text[command_start] == '!'))
        ++command_start;
    size_t command_end = command_start;
    while (command_end < length && !space((unsigned char)text[command_end]) && text[command_end] != '=')
        ++command_end;
    size_t name_end = command_end;
    if (name_end > command_start && text[name_end - 1] == '?')
        --name_end;
    int comment = first < length && (text[first] == ';' ||
        word_equal(text + command_start, name_end - command_start, "rem"));
    int command = name_end > command_start && in_words(text + command_start,
        name_end - command_start, commands, sizeof(commands) / sizeof(commands[0]));
    size_t position = 0;
    while (position < length) {
        size_t start = position;
        unsigned char character = (unsigned char)text[position];
        LexerTokenKind kind = LEXER_TEXT;
        if (position < first) {
            position = first;
        } else if (comment && position == first) {
            kind = LEXER_COMMENT;
            position = length;
        } else if (position == first && character == '[') {
            kind = LEXER_PREPROCESSOR;
            const char *closing = memchr(text + position + 1, ']', length - position - 1);
            position = closing ? (size_t)(closing - text) + 1 : length;
        } else if (position == first && command_end > first) {
            kind = command ? LEXER_KEYWORD : LEXER_TEXT;
            position = command_end;
        } else if (character == '"') {
            kind = LEXER_STRING;
            ++position;
            while (position < length && text[position] != '"')
                ++position;
            if (position < length)
                ++position;
        } else if (character == '%') {
            const char *closing = memchr(text + position + 1, '%', length - position - 1);
            if (closing && closing > text + position + 1) {
                kind = LEXER_PREPROCESSOR;
                position = (size_t)(closing - text) + 1;
            } else {
                ++position;
            }
        } else if (character == '=' || character == ',') {
            kind = LEXER_PREPROCESSOR;
            ++position;
        } else if (space(character)) {
            ++position;
        } else {
            ++position;
            while (position < length && !space((unsigned char)text[position]) &&
                text[position] != '=' && text[position] != ',' &&
                text[position] != '"' && text[position] != '%')
                ++position;
            if (character == '/')
                kind = LEXER_PREPROCESSOR;
            else if (number_literal(text + start, position - start))
                kind = LEXER_NUMBER;
            else if (in_words(text + start, position - start, values, sizeof(values) / sizeof(values[0])))
                kind = LEXER_KEYWORD;
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
    (void)extension;
    return 0;
}

static int supports_filename(const char *basename)
{
    return basename && word_equal(basename, strlen(basename), "config.sys");
}

const Lexer lexer_config_sys = {
    "CONFIG.SYS", supports_extension, lex_line, states_equal, free_line, supports_filename
};