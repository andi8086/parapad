#include "lexer_batch.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct BatchState {
    int quoted;
    int command_start;
} BatchState;

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

static int word_character(unsigned char character)
{
    character = lower_ascii(character);
    return (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9') || character == '_';
}

static size_t variable_end(const char *text, size_t length, size_t position)
{
    char delimiter = text[position];
    size_t following = position + 1;
    if (following == length)
        return following;
    if (delimiter == '%') {
        if (text[following] == '%') {
            ++following;
            if (following < length && text[following] != '~')
                return word_character((unsigned char)text[following]) ? following + 1 : following;
        }
        if (following < length && text[following] == '~') {
            ++following;
            while (following < length && (word_character((unsigned char)text[following]) ||
                text[following] == '$' || text[following] == ':'))
                ++following;
            return following;
        }
        if (following < length && ((text[following] >= '0' && text[following] <= '9') ||
            text[following] == '*'))
            return following + 1;
    }
    for (size_t end = following; end < length; ++end) {
        if (text[end] == delimiter)
            return end + 1;
    }
    return delimiter == '%' && word_character((unsigned char)text[position + 1])
        ? position + 2 : position + 1;
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
        "assoc", "break", "call", "cd", "chdir", "chcp", "cls", "color", "copy",
        "ctty", "date", "del", "dir", "echo", "endlocal", "erase", "exit", "for",
        "ftype", "goto", "if", "lh", "loadhigh", "md", "mkdir", "mklink", "move",
        "path", "pause", "popd", "prompt", "pushd", "rd", "rem", "ren", "rename",
        "rmdir", "set", "setlocal", "shift", "start", "time", "title", "type",
        "ver", "verify", "vol"
    };
    static const char *const control[] = {
        "if", "else", "for", "in", "do", "not", "exist", "errorlevel", "defined",
        "cmdextversion", "equ", "neq", "lss", "leq", "gtr", "geq"
    };
    if (!result || (!text && length))
        return 0;
    BatchState current = { 0, 1 };
    if (state)
        current = *(const BatchState *)state;
    LexerLine line = { 0 };
    size_t capacity = 0;
    size_t position = 0;
    int continued = 0;
    while (position < length) {
        size_t start = position;
        unsigned char character = (unsigned char)text[position];
        LexerTokenKind kind = current.quoted ? LEXER_STRING : LEXER_TEXT;
        if (!current.quoted && character == '^') {
            ++position;
            if (position == length || (position + 1 == length && text[position] == '\r')) {
                position = length;
                continued = 1;
            } else {
                ++position;
            }
        } else if (character == '%' || character == '!') {
            position = variable_end(text, length, position);
            if (position > start + 1)
                kind = LEXER_PREPROCESSOR;
        } else if (character == '"') {
            kind = LEXER_STRING;
            current.quoted = !current.quoted;
            current.command_start = 0;
            ++position;
        } else if (current.quoted) {
            ++position;
        } else if (current.command_start && character == ':') {
            kind = position + 1 < length && text[position + 1] == ':'
                ? LEXER_COMMENT : LEXER_PREPROCESSOR;
            position = length;
        } else if (current.command_start && length - position >= 3 &&
            word_equal(text + position, 3, "rem") &&
            (position + 3 == length || !word_character((unsigned char)text[position + 3]))) {
            kind = LEXER_COMMENT;
            position = length;
        } else if (character == '&' || character == '|' || character == '(' || character == ')') {
            kind = LEXER_KEYWORD;
            current.command_start = 1;
            ++position;
        } else if (character == '<' || character == '>' || character == '@') {
            kind = LEXER_KEYWORD;
            ++position;
        } else if (word_character(character)) {
            ++position;
            while (position < length && word_character((unsigned char)text[position]))
                ++position;
            size_t word_length = position - start;
            if ((current.command_start && in_words(text + start, word_length, commands,
                sizeof(commands) / sizeof(commands[0]))) ||
                in_words(text + start, word_length, control, sizeof(control) / sizeof(control[0])))
                kind = LEXER_KEYWORD;
            else if (character >= '0' && character <= '9')
                kind = LEXER_NUMBER;
            current.command_start = word_equal(text + start, word_length, "call") ||
                word_equal(text + start, word_length, "else") ||
                word_equal(text + start, word_length, "do");
        } else {
            if (character != ' ' && character != '\t' && character != '\r')
                current.command_start = 0;
            ++position;
        }
        if (!append_token(&line, &capacity, start, position - start, kind)) {
            free_line(&line);
            return 0;
        }
    }
    if (continued) {
        BatchState *ending = malloc(sizeof(*ending));
        if (!ending) {
            free_line(&line);
            return 0;
        }
        *ending = current;
        line.end_state = ending;
    }
    *result = line;
    return 1;
}

static int states_equal(const void *left, const void *right)
{
    if (!left || !right)
        return left == right;
    const BatchState *left_state = left;
    const BatchState *right_state = right;
    return left_state->quoted == right_state->quoted &&
        left_state->command_start == right_state->command_start;
}

static int supports_extension(const char *extension)
{
    return extension && (word_equal(extension, strlen(extension), ".bat") ||
        word_equal(extension, strlen(extension), ".cmd"));
}

const Lexer lexer_batch = {
    "DOS Batch", supports_extension, lex_line, states_equal, free_line, NULL
};