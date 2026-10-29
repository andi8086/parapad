#ifndef LEXER_C_CPP_H
#define LEXER_C_CPP_H

#include <stddef.h>

typedef enum LexerCCppKind {
    LEXER_C_CPP_TEXT,
    LEXER_C_CPP_KEYWORD,
    LEXER_C_CPP_NUMBER,
    LEXER_C_CPP_STRING,
    LEXER_C_CPP_COMMENT,
    LEXER_C_CPP_PREPROCESSOR
} LexerCCppKind;

typedef enum LexerCCppMode {
    LEXER_C_CPP_NORMAL,
    LEXER_C_CPP_BLOCK_COMMENT,
    LEXER_C_CPP_LINE_COMMENT,
    LEXER_C_CPP_QUOTED_STRING,
    LEXER_C_CPP_CHARACTER,
    LEXER_C_CPP_RAW_STRING,
    LEXER_C_CPP_HEADER
} LexerCCppMode;

typedef struct LexerCCppState {
    LexerCCppMode mode;
    int directive;
    int escape;
    size_t delimiter_length;
    char delimiter[17];
} LexerCCppState;

typedef struct LexerCCppToken {
    size_t start;
    size_t length;
    LexerCCppKind kind;
} LexerCCppToken;

typedef struct LexerCCppLine {
    LexerCCppToken *tokens;
    size_t count;
    size_t capacity;
    LexerCCppState end_state;
} LexerCCppLine;

int lexer_c_cpp_line(const char *text, size_t length, LexerCCppState state, LexerCCppLine *result);
int lexer_c_cpp_state_equal(const LexerCCppState *left, const LexerCCppState *right);
void lexer_c_cpp_free_line(LexerCCppLine *line);

#endif