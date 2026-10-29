#include "lexer_html.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum HtmlMode {
    HTML_TEXT, HTML_TAG, HTML_COMMENT, HTML_DECLARATION, HTML_INSTRUCTION, HTML_CDATA
};

typedef struct HtmlState {
    int mode;
    char quote;
    size_t brackets;
    int name_pending;
    int value_pending;
    int closing;
    int raw;
    int pending_raw;
} HtmlState;

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

static int starts_with(const char *text, size_t length, size_t position, const char *prefix)
{
    size_t count = strlen(prefix);
    return count <= length - position && !memcmp(text + position, prefix, count);
}

static int space(unsigned char character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\f';
}

static int letter(unsigned char character)
{
    character = lower_ascii(character);
    return character >= 'a' && character <= 'z';
}

static int name_character(unsigned char character)
{
    return letter(character) || (character >= '0' && character <= '9') ||
        character == '_' || character == ':' || character == '-' || character == '.' || character >= 128;
}

static size_t entity_end(const char *text, size_t length, size_t position)
{
    size_t following = position + 1;
    int base = 0;
    if (following < length && text[following] == '#') {
        ++following;
        base = 10;
        if (following < length && lower_ascii((unsigned char)text[following]) == 'x') {
            ++following;
            base = 16;
        }
    } else if (following == length || !letter((unsigned char)text[following])) {
        return position;
    }
    size_t first = following;
    while (following < length) {
        unsigned char character = lower_ascii((unsigned char)text[following]);
        int digit = character >= '0' && character <= '9';
        int hex = character >= 'a' && character <= 'f';
        if (base ? !(digit || (base == 16 && hex)) : !(digit || letter(character)))
            break;
        ++following;
    }
    return following > first && following < length && text[following] == ';' ? following + 1 : position;
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
    static const char *const raw_names[] = { "", "script", "style", "textarea", "title" };
    if (!result || (!text && length))
        return 0;
    HtmlState current = { 0 };
    if (state)
        current = *(const HtmlState *)state;
    LexerLine line = { 0 };
    size_t capacity = 0;
    size_t position = 0;
    while (position < length) {
        size_t start = position;
        unsigned char character = (unsigned char)text[position];
        LexerTokenKind kind = LEXER_TEXT;
        if (current.mode == HTML_TEXT && current.raw && starts_with(text, length, position, "</")) {
            size_t name_length = strlen(raw_names[current.raw]);
            size_t following = position + 2;
            if (name_length <= length - following &&
                word_equal(text + following, name_length, raw_names[current.raw])) {
                following += name_length;
                if (following == length || space((unsigned char)text[following]) ||
                    text[following] == '>' || text[following] == '/')
                    current.raw = 0;
            }
        }
        if (current.mode == HTML_TEXT && !current.raw) {
            if (starts_with(text, length, position, "<!--")) {
                current.mode = HTML_COMMENT;
                position += 4;
            } else if (starts_with(text, length, position, "<![CDATA[")) {
                current.mode = HTML_CDATA;
                position += 9;
            } else if (starts_with(text, length, position, "<!")) {
                current.mode = HTML_DECLARATION;
                position += 2;
            } else if (starts_with(text, length, position, "<?")) {
                current.mode = HTML_INSTRUCTION;
                position += 2;
            } else if (character == '<' && (position + 1 == length ||
                letter((unsigned char)text[position + 1]) || text[position + 1] == '/')) {
                current.mode = HTML_TAG;
                current.name_pending = 1;
                current.closing = position + 1 < length && text[position + 1] == '/';
                position += current.closing ? 2 : 1;
                kind = LEXER_KEYWORD;
            }
        }
        if (current.mode == HTML_COMMENT || current.mode == HTML_CDATA) {
            kind = current.mode == HTML_COMMENT ? LEXER_COMMENT : LEXER_STRING;
            const char *closing = current.mode == HTML_COMMENT ? "-->" : "]]>";
            while (position < length && !starts_with(text, length, position, closing))
                ++position;
            if (position < length) {
                position += 3;
                current.mode = HTML_TEXT;
            }
        } else if (position != start) {
            if (current.mode == HTML_DECLARATION || current.mode == HTML_INSTRUCTION)
                kind = LEXER_PREPROCESSOR;
        } else if (current.quote) {
            kind = LEXER_STRING;
            size_t entity = character == '&' ? entity_end(text, length, position) : position;
            if (entity > position) {
                kind = LEXER_PREPROCESSOR;
                position = entity;
            } else {
                if (text[position++] == current.quote)
                    current.quote = 0;
            }
        } else if (current.mode == HTML_DECLARATION || current.mode == HTML_INSTRUCTION) {
            kind = LEXER_PREPROCESSOR;
            if (character == '"' || character == '\'') {
                current.quote = (char)character;
                kind = LEXER_STRING;
                ++position;
            } else if (current.mode == HTML_INSTRUCTION && starts_with(text, length, position, "?>")) {
                position += 2;
                current.mode = HTML_TEXT;
            } else if (current.mode == HTML_DECLARATION && character == '>' && !current.brackets) {
                ++position;
                current.mode = HTML_TEXT;
            } else {
                if (current.mode == HTML_DECLARATION) {
                    if (character == '[') {
                        if (current.brackets == SIZE_MAX)
                            goto failure;
                        ++current.brackets;
                    } else if (character == ']' && current.brackets) {
                        --current.brackets;
                    }
                }
                ++position;
            }
        } else if (current.mode == HTML_TAG) {
            if (space(character)) {
                ++position;
            } else if (character == '>' || starts_with(text, length, position, "/>")) {
                int raw = !current.closing && character == '>' ? current.pending_raw : 0;
                position += character == '>' ? 1 : 2;
                memset(&current, 0, sizeof(current));
                current.raw = raw;
                kind = LEXER_KEYWORD;
            } else if (character == '=') {
                current.value_pending = 1;
                ++position;
            } else if (character == '"' || character == '\'') {
                current.quote = (char)character;
                current.value_pending = 0;
                kind = LEXER_STRING;
                ++position;
            } else if (current.value_pending) {
                kind = LEXER_STRING;
                while (position < length && !space((unsigned char)text[position]) &&
                    text[position] != '>' && !starts_with(text, length, position, "/>"))
                    ++position;
                current.value_pending = 0;
            } else if (name_character(character)) {
                ++position;
                while (position < length && name_character((unsigned char)text[position]))
                    ++position;
                kind = current.name_pending ? LEXER_KEYWORD : LEXER_PREPROCESSOR;
                if (current.name_pending && !current.closing) {
                    for (int index = 1; index < 5; ++index) {
                        if (word_equal(text + start, position - start, raw_names[index]))
                            current.pending_raw = index;
                    }
                }
                current.name_pending = 0;
            } else {
                ++position;
            }
        } else {
            size_t entity = character == '&' && (!current.raw || current.raw >= 3)
                ? entity_end(text, length, position) : position;
            if (entity > position) {
                kind = LEXER_PREPROCESSOR;
                position = entity;
            } else {
                ++position;
            }
        }
        if (!append_token(&line, &capacity, start, position - start, kind))
            goto failure;
    }
    if (current.mode != HTML_TEXT || current.raw) {
        HtmlState *ending = malloc(sizeof(*ending));
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
    const HtmlState *left_state = left;
    const HtmlState *right_state = right;
    return left_state->mode == right_state->mode && left_state->quote == right_state->quote &&
        left_state->brackets == right_state->brackets && left_state->name_pending == right_state->name_pending &&
        left_state->value_pending == right_state->value_pending && left_state->closing == right_state->closing &&
        left_state->raw == right_state->raw && left_state->pending_raw == right_state->pending_raw;
}

static int supports_extension(const char *extension)
{
    return extension && (word_equal(extension, strlen(extension), ".htm") ||
        word_equal(extension, strlen(extension), ".html"));
}

const Lexer lexer_html = {
    "HTML", supports_extension, lex_line, states_equal, free_line, NULL
};