#include "lexer_makefile.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct MakefileState {
    size_t define_depth;
    int continuation;
} MakefileState;

static int word_equal(const char *text, size_t length, const char *word)
{
    return strlen(word) == length && !memcmp(text, word, length);
}

static int directive(const char *text, size_t length)
{
    static const char *const words[] = {
        "include", "-include", "sinclude", "define", "endef", "undefine",
        "ifdef", "ifndef", "ifeq", "ifneq", "else", "endif", "override",
        "export", "unexport", "private", "vpath"
    };
    for (size_t index = 0; index < sizeof(words) / sizeof(words[0]); ++index) {
        if (word_equal(text, length, words[index]))
            return 1;
    }
    return 0;
}

static int space(unsigned char character)
{
    return character == ' ' || character == '\t' || character == '\r';
}

static size_t expansion_end(const char *text, size_t length, size_t position)
{
    ++position;
    if (position == length)
        return position;
    char opening = text[position++];
    if (opening != '(' && opening != '{')
        return position;
    char closing = opening == '(' ? ')' : '}';
    size_t depth = 1;
    while (position < length) {
        char character = text[position++];
        if (character == opening)
            ++depth;
        else if (character == closing && !--depth)
            break;
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
    if (!result || (!text && length))
        return 0;
    MakefileState current = { 0 };
    if (state)
        current = *(const MakefileState *)state;
    LexerLine line = { 0 };
    size_t capacity = 0;
    size_t first = 0;
    while (first < length && space((unsigned char)text[first]))
        ++first;
    int recipe = current.continuation == 2 || (length && text[0] == '\t' &&
        !current.define_depth && !current.continuation);
    int comment = current.continuation == 3;
    size_t word_end = first;
    while (word_end < length && !space((unsigned char)text[word_end]) &&
        text[word_end] != '(' && text[word_end] != '#' && text[word_end] != ':')
        ++word_end;
    int is_directive = !recipe && !comment && !current.continuation &&
        directive(text ? text + first : "", word_end - first);
    if (is_directive && word_equal(text + first, word_end - first, "define")) {
        if (current.define_depth != SIZE_MAX)
            ++current.define_depth;
    } else if (is_directive && word_equal(text + first, word_end - first, "endef")) {
        if (current.define_depth)
            --current.define_depth;
    }
    size_t operator_start = length;
    size_t operator_end = length;
    if (!recipe && !comment && !is_directive && !current.define_depth &&
        !current.continuation) {
        for (size_t position = first; position < length; ++position) {
            if (text[position] == '$') {
                position = expansion_end(text, length, position) - 1;
            } else if (text[position] == '\\' && position + 1 < length) {
                ++position;
            } else if (text[position] == '#') {
                break;
            } else if (text[position] == ':' || text[position] == '=' ||
                (position + 1 < length && text[position + 1] == '=' &&
                    (text[position] == '+' || text[position] == '?' || text[position] == '!'))) {
                operator_start = position;
                operator_end = position + 1;
                while (operator_end < length &&
                    (text[operator_end] == ':' || text[operator_end] == '='))
                    ++operator_end;
                break;
            }
        }
    }
    size_t position = 0;
    while (position < length) {
        size_t start = position;
        LexerTokenKind kind = LEXER_TEXT;
        unsigned char character = (unsigned char)text[position];
        if (comment || character == '#') {
            kind = LEXER_COMMENT;
            comment = 1;
            position = length;
        } else if (character == '$') {
            kind = LEXER_KEYWORD;
            position = expansion_end(text, length, position);
        } else if (character == '\\' && position + 1 < length) {
            position += 2;
        } else if (recipe && (character == '\'' || character == '"')) {
            kind = LEXER_STRING;
            ++position;
            while (position < length) {
                if (text[position] == '\\' && character == '"' && position + 1 < length)
                    position += 2;
                else if ((unsigned char)text[position++] == character)
                    break;
            }
        } else if (is_directive && position == first) {
            kind = LEXER_PREPROCESSOR;
            position = word_end;
        } else if (position >= operator_start && position < operator_end) {
            kind = LEXER_PREPROCESSOR;
            position = operator_end;
        } else {
            if (position < operator_start && operator_start < length && !space(character))
                kind = LEXER_KEYWORD;
            else if (recipe && position == first &&
                (character == '@' || character == '-' || character == '+'))
                kind = LEXER_PREPROCESSOR;
            ++position;
        }
        if (!append_token(&line, &capacity, start, position - start, kind)) {
            free_line(&line);
            return 0;
        }
    }
    size_t end = length;
    if (end && text[end - 1] == '\r')
        --end;
    size_t backslashes = 0;
    while (end && text[end - 1] == '\\') {
        --end;
        ++backslashes;
    }
    current.continuation = backslashes % 2 ? (comment ? 3 : recipe ? 2 : 1) : 0;
    if (current.define_depth || current.continuation) {
        MakefileState *ending = malloc(sizeof(*ending));
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
    MakefileState empty = { 0 };
    const MakefileState *left_state = left ? left : &empty;
    const MakefileState *right_state = right ? right : &empty;
    return left_state->define_depth == right_state->define_depth &&
        left_state->continuation == right_state->continuation;
}

static int supports_extension(const char *extension)
{
    return extension && !strcmp(extension, ".mk");
}

static int supports_filename(const char *basename)
{
    return basename && !strcmp(basename, "Makefile");
}

const Lexer lexer_makefile = {
    "Makefile", supports_extension, lex_line, states_equal, free_line, supports_filename
};