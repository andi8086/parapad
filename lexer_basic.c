#include "lexer_basic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct BasicState {
    size_t comment_depth;
} BasicState;

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
    while (position < length && digit_value((unsigned char)text[position]) < base)
        ++position;
    return position;
}

static int type_suffix(unsigned char character)
{
    return character == '$' || character == '%' || character == '&' ||
        character == '!' || character == '#' || character == '@' || character == '^';
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
        "abs", "access", "addhandler", "address", "addressof", "alias", "and", "andalso",
        "any", "append", "as", "asc", "asin", "asm", "atn", "base", "beep", "bin",
        "binary", "boolean", "bsave", "byref", "byte", "byval", "call", "callbyname",
        "case", "cast", "cbool", "cbyte", "ccur", "cdbl", "cdecl", "cdec", "chain",
        "chdir", "chr", "cint", "circle", "class", "clear", "clng", "clngint",
        "close", "cls", "cobj", "color", "command", "common", "const", "constructor",
        "continue", "cos", "csign", "csng", "cstr", "cuint", "culng", "culngint",
        "curdir", "currency", "cvar", "cvd", "cvi", "cvl", "cvs", "data", "date",
        "dateadd", "datediff", "datepart", "dateserial", "datevalue", "day", "declare",
        "def", "defbyte", "defdbl", "defined", "defint", "deflng", "deflong", "defsng",
        "defstr", "defuint", "defulng", "delete", "destructor", "dim", "dir", "do",
        "double", "draw", "dylibfree", "dylibload", "dylibsymbol", "dynamic", "each",
        "else", "elseif", "empty", "end", "endif", "enum", "environ", "eof", "eqv",
        "erase", "err", "error", "event", "exit", "exp", "explicit", "export",
        "extern", "false", "field", "fileattr", "filecopy", "fix", "flip", "for",
        "format", "fre", "freefile", "function", "get", "getattr", "gosub", "goto",
        "hex", "hibyte", "hiword", "hour", "if", "iif", "imp", "implements",
        "import", "in", "inkey", "inp", "input", "inputbox", "instr", "int",
        "integer", "is", "isarray", "isdate", "isempty", "iserror", "isnull",
        "isnumeric", "isobject", "kill", "lbound", "lcase", "left", "len", "let",
        "lib", "like", "line", "lobyte", "loc", "local", "locate", "lock", "lof",
        "log", "long", "longint", "longptr", "loop", "loword", "lprint", "lset",
        "ltrim", "mid", "minute", "mkdir", "mkd", "mki", "mkl", "mks", "mod",
        "month", "msgbox", "multikey", "name", "namespace", "new", "next", "not",
        "nothing", "now", "null", "object", "oct", "on", "once", "open", "operator",
        "option", "optional", "or", "orelse", "out", "output", "overload",
        "paramarray", "pascal", "pcopy", "peek", "play", "pmap", "point", "pointer",
        "poke", "pos", "preserve", "preset", "print", "private", "procptr",
        "property", "protected", "pset", "ptr", "public", "put", "raiseevent",
        "random", "randomize", "read", "realloc", "redim", "rem", "removehandler",
        "reset", "restore", "resume", "return", "rgb", "rgba", "right", "rmdir",
        "rnd", "rset", "rtrim", "run", "sadd", "screen", "second", "seek", "select",
        "set", "setattr", "sgn", "shared", "shell", "short", "shl", "shr", "sin",
        "single", "sizeof", "sleep", "sound", "space", "spc", "sqr", "static",
        "stdcall", "step", "stop", "str", "string", "strptr", "sub", "swap",
        "system", "tab", "tan", "then", "threadcreate", "threadwait", "time",
        "timer", "timeserial", "timevalue", "to", "trim", "true", "type", "typeof",
        "ubound", "ubyte", "ucase", "uinteger", "ulong", "ulongint", "union",
        "unlock", "unsigned", "until", "ushort", "using", "val", "var", "variant",
        "varptr", "vartype", "view", "virtual", "wait", "wend", "while", "width",
        "window", "with", "write", "xor", "year", "zstring"
    };
    if (!result || (!text && length))
        return 0;
    BasicState current = { 0 };
    if (state)
        current = *(const BasicState *)state;
    LexerLine line = { 0 };
    size_t capacity = 0;
    size_t position = 0;
    int statement_start = 1;
    int line_start = 1;
    while (position < length) {
        size_t start = position;
        unsigned char character = (unsigned char)text[position];
        LexerTokenKind kind = LEXER_TEXT;
        if (current.comment_depth || (character == '/' && position + 1 < length && text[position + 1] == '\'')) {
            kind = LEXER_COMMENT;
            if (!current.comment_depth) {
                current.comment_depth = 1;
                position += 2;
            }
            while (position < length && current.comment_depth) {
                if (position + 1 < length && text[position] == '/' && text[position + 1] == '\'') {
                    if (current.comment_depth == SIZE_MAX)
                        goto failure;
                    ++current.comment_depth;
                    position += 2;
                } else if (position + 1 < length && text[position] == '\'' && text[position + 1] == '/') {
                    --current.comment_depth;
                    position += 2;
                } else {
                    ++position;
                }
            }
        } else if (character == '\'') {
            kind = LEXER_COMMENT;
            position = length;
        } else if (character == '#' && line_start && position + 1 < length &&
            word_start((unsigned char)text[position + 1])) {
            kind = LEXER_PREPROCESSOR;
            position = length;
        } else if (character == '"' || (character == '!' && position + 1 < length && text[position + 1] == '"')) {
            kind = LEXER_STRING;
            int escaped = character == '!';
            position += escaped ? 2 : 1;
            while (position < length) {
                if (escaped && text[position] == '\\' && position + 1 < length) {
                    position += 2;
                    continue;
                }
                if (text[position++] == '"') {
                    if (position == length || text[position] != '"')
                        break;
                    ++position;
                }
            }
            statement_start = 0;
        } else if (word_start(character)) {
            ++position;
            while (position < length && (word_start((unsigned char)text[position]) ||
                digit_value((unsigned char)text[position]) < 10))
                ++position;
            size_t word_length = position - start;
            if (position < length && type_suffix((unsigned char)text[position]))
                ++position;
            if (statement_start && position - start == word_length &&
                word_equal(text + start, word_length, "rem")) {
                kind = LEXER_COMMENT;
                position = length;
            } else {
                for (size_t index = 0; index < sizeof(keywords) / sizeof(keywords[0]); ++index) {
                    if (word_equal(text + start, word_length, keywords[index])) {
                        kind = LEXER_KEYWORD;
                        break;
                    }
                }
            }
            statement_start = word_equal(text + start, word_length, "then") ||
                word_equal(text + start, word_length, "else");
        } else if (character == '#') {
            const char *closing = memchr(text + position + 1, '#', length - position - 1);
            if (closing) {
                kind = LEXER_STRING;
                position = (size_t)(closing - text) + 1;
            } else {
                ++position;
            }
            statement_start = 0;
        } else {
            size_t number = position;
            int base = 10;
            if (character == '&' && position + 1 < length) {
                unsigned char prefix = lower_ascii((unsigned char)text[position + 1]);
                if (prefix == 'h' || prefix == 'o' || prefix == 'b') {
                    base = prefix == 'h' ? 16 : prefix == 'o' ? 8 : 2;
                    number += 2;
                } else {
                    base = 8;
                    ++number;
                }
            }
            int leading_dot = character == '.' && position + 1 < length &&
                digit_value((unsigned char)text[position + 1]) < 10;
            if ((number < length && digit_value((unsigned char)text[number]) < base) || leading_dot) {
                kind = LEXER_NUMBER;
                position = digits_end(text, length, number, base);
                if (base == 10) {
                    if (position < length && text[position] == '.')
                        position = digits_end(text, length, position + 1, 10);
                    if (position < length && (lower_ascii((unsigned char)text[position]) == 'e' ||
                        lower_ascii((unsigned char)text[position]) == 'd')) {
                        size_t exponent = position + 1;
                        if (exponent < length && (text[exponent] == '+' || text[exponent] == '-'))
                            ++exponent;
                        if (exponent < length && digit_value((unsigned char)text[exponent]) < 10)
                            position = digits_end(text, length, exponent, 10);
                    }
                }
                if (position < length && text[position] != '$' && type_suffix((unsigned char)text[position]))
                    ++position;
            } else {
                ++position;
                if (character == ':')
                    statement_start = 1;
                else if (character != ' ' && character != '\t' && character != '\r')
                    statement_start = 0;
                if (character == '?')
                    kind = LEXER_KEYWORD;
            }
        }
        if (character != ' ' && character != '\t' && character != '\r')
            line_start = 0;
        if (!append_token(&line, &capacity, start, position - start, kind))
            goto failure;
    }
    if (current.comment_depth) {
        BasicState *ending = malloc(sizeof(*ending));
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
    return ((const BasicState *)left)->comment_depth == ((const BasicState *)right)->comment_depth;
}

static int supports_extension(const char *extension)
{
    return extension && word_equal(extension, strlen(extension), ".bas");
}

const Lexer lexer_basic = {
    "BASIC", supports_extension, lex_line, states_equal, free_line, NULL
};