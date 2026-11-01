#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef enum LexerTokenKind {
    LEXER_TEXT,
    LEXER_KEYWORD,
    LEXER_NUMBER,
    LEXER_STRING,
    LEXER_COMMENT,
    LEXER_PREPROCESSOR
} LexerTokenKind;

typedef struct LexerToken {
    size_t start;
    size_t length;
    LexerTokenKind kind;
} LexerToken;

typedef struct LexerLine {
    LexerToken *tokens;
    size_t count;
    void *end_state;
} LexerLine;

typedef struct Lexer {
    const char *name;
    int (*supports_extension)(const char *extension);
    int (*lex_line)(const char *text, size_t length, const void *state, LexerLine *result);
    int (*states_equal)(const void *left, const void *right);
    void (*free_line)(LexerLine *line);
    int (*supports_filename)(const char *basename);
} Lexer;

#endif