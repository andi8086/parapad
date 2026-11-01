#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <limits.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor_core.h"
#include "lexer.h"

#ifndef OPENFILENAME_SIZE_VERSION_400A
#       define OPENFILENAME_SIZE_VERSION_400A \
                (sizeof(OPENFILENAMEA) - sizeof(void *) * 3 \
                - sizeof(DWORD) * 2)
#endif

static UINT legacy_wheel_message;

typedef struct SyntaxLine {
    LexerLine parsed;
    size_t version;
    int valid;
} SyntaxLine;

typedef struct SyntaxCache {
    SyntaxLine *lines;
    size_t line_count;
    size_t first_line;
    size_t revision;
    size_t serial;
    int valid;
    const Lexer *lexer;
} SyntaxCache;

#define ID_EDITOR 100
#define ID_FILE_EXIT 101
#define ID_STATUS 102
#define ID_FILE_NEW 103
#define ID_FILE_OPEN 104
#define ID_FILE_SAVE 105
#define ID_FILE_SAVE_AS 106
#define ID_COMMAND_LINE 107
#define ID_BUFFER_LIST 108
#define ID_FILE_CLOSE 109
#define ID_SECOND_EDITOR 110
#define ID_VIEW_TEXT 111
#define ID_VIEW_HEX 112
#define WM_EDITOR_MODE_CHANGED (WM_APP + 1)
#define WM_EDITOR_BUFFER_CHANGED (WM_APP + 2)
#define WM_EDITOR_REFRESH (WM_APP + 3)
#define WM_EDITOR_SWITCH_BUFFER (WM_APP + 4)
#define WM_EDITOR_FOCUS (WM_APP + 5)
#define WM_EDITOR_WINDOW_KEY (WM_APP + 6)

typedef struct CachedLine {
    char *text;
    size_t index;
    size_t length;
    EditorSelection selection;
    EditorLineCaret caret;
    size_t syntax_version;
    int exists;
    int valid;
} CachedLine;

typedef struct EditorControl {
    EditorCore *core;
    HDC memdc;
    HBITMAP back_bitmap;
    HGDIOBJ original_bitmap;
    int back_width;
    int back_height;
    HFONT font;
    int font_points;
    int line_height;
    int text_left;
    size_t first_line;
    int left_pixel;
    int wheel_delta;
    char pending_lead;
    CachedLine *lines;
    size_t line_count;
    size_t painted_first_line;
    int painted_left_pixel;
    int bitmap_valid;
    EditorSettings painted_settings;
    SyntaxCache *syntax;
    int syntax_ready;
    int hex_mode;
    int hex_ascii;
    int hex_low;
    int hex_insert;
    int hex_editing;
    char hex_command[11];
    size_t hex_offset;
    size_t hex_first;
    int hex_left;
    int character_width;
    EditorCore *hex_core;
} EditorControl;

typedef struct TextBuffer {
    EditorCore *core;
    char path[MAX_PATH];
    size_t first_line;
    int left_pixel;
    SyntaxCache syntax;
} TextBuffer;

typedef struct MainWindowState {
    EditorCore *core;
    TextBuffer **buffers;
    size_t buffer_count;
    size_t active_buffer;
    WNDPROC command_proc;
    int command_active;
    const char *command_error;
    HWND second_editor;
    TextBuffer *pane_buffers[2];
    EditorCaret pane_carets[2];
    int active_pane;
    int split_vertical;
    int window_pending;
} MainWindowState;

static HWND active_editor(HWND window)
{
    MainWindowState *state = (MainWindowState *)GetWindowLongPtrA(window, GWLP_USERDATA);
    return state && state->active_pane && state->second_editor ? state->second_editor :
        GetDlgItem(window, ID_EDITOR);
}

static void clear_syntax_cache(SyntaxCache *cache)
{
    for (size_t index = 0; index < cache->line_count; ++index)
        cache->lexer->free_line(&cache->lines[index].parsed);
    free(cache->lines);
    memset(cache, 0, sizeof(*cache));
}

static int update_syntax_cache(SyntaxCache *cache, EditorCore *core, size_t first, size_t count)
{
    if (!cache)
        return 1;
    const Lexer *lexer = NULL;
    editor_core_send(core, EDITOR_GET_LEXER, (void *)&lexer);
    if (cache->lexer != lexer) {
        size_t serial = cache->serial;
        clear_syntax_cache(cache);
        cache->serial = serial;
        cache->lexer = lexer;
    }
    if (!lexer)
        return 1;
    EditorChange change;
    editor_core_send(core, EDITOR_TAKE_CHANGE, &change);
    if (cache->valid && change.revision == cache->revision &&
        cache->first_line == first && cache->line_count == count)
        return 1;
    if (!count || count > (size_t)-1 / sizeof(*cache->lines) || count > (size_t)-1 - first)
        return 0;
    SyntaxLine *lines = calloc(count, sizeof(*lines));
    if (!lines)
        return 0;
    const void *state = NULL;
    size_t begin = 0;
    if (cache->valid && cache->revision == change.revision && cache->line_count &&
        first >= cache->first_line + cache->line_count &&
        cache->lines[cache->line_count - 1].valid) {
        begin = cache->first_line + cache->line_count;
        state = cache->lines[cache->line_count - 1].parsed.end_state;
    }
    LexerLine carry = { 0 };
    for (size_t index = begin; index < first + count; ++index) {
        EditorLine text = {0};
        text.index = index;
        if (!editor_core_send(core, EDITOR_GET_LINE, &text))
            break;
        LexerLine parsed = { 0 };
        if (!lexer->lex_line(text.text, text.length, state, &parsed)) {
            lexer->free_line(&carry);
            for (size_t slot = 0; slot < count; ++slot)
                lexer->free_line(&lines[slot].parsed);
            free(lines);
            cache->valid = 0;
            return 0;
        }
        lexer->free_line(&carry);
        if (index < first) {
            carry = parsed;
        } else {
            SyntaxLine *line = &lines[index - first];
            line->parsed = parsed;
            line->valid = 1;
            if (!++cache->serial)
                ++cache->serial;
            line->version = cache->serial;
        }
        state = parsed.end_state;
        if (text.truncated)
            break;
    }
    lexer->free_line(&carry);
    for (size_t slot = 0; slot < cache->line_count; ++slot)
        lexer->free_line(&cache->lines[slot].parsed);
    free(cache->lines);
    cache->lines = lines;
    cache->first_line = first;
    cache->line_count = count;
    cache->revision = change.revision;
    cache->valid = 1;
    return 1;
}

static COLORREF syntax_color(LexerTokenKind kind)
{
    switch (kind) {
    case LEXER_KEYWORD: return RGB(86, 200, 255);
    case LEXER_NUMBER: return RGB(205, 160, 240);
    case LEXER_STRING: return RGB(235, 200, 120);
    case LEXER_COMMENT: return RGB(110, 180, 120);
    case LEXER_PREPROCESSOR: return RGB(255, 160, 110);
    default: return RGB(225, 225, 225);
    }
}

static int character_to_ansi(unsigned long value, char *text, int capacity)
{
    if (value < 32 || value == 127)
        value = '.';
    WCHAR units[2];
    int count = 1;
    if (value <= 0xFFFF) {
        units[0] = (WCHAR)value;
    } else {
        value -= 0x10000;
        units[0] = (WCHAR)(0xD800 + (value >> 10));
        units[1] = (WCHAR)(0xDC00 + (value & 0x3FF));
        count = 2;
    }
    int length = WideCharToMultiByte(CP_ACP, 0, units, count,
        text, capacity, NULL, NULL);
    if (!length) {
        text[0] = '?';
        length = 1;
    }
    return length;
}

static int dispatch_ansi_key(EditorControl *editor, WPARAM value, unsigned int repeats)
{
    char text[2];
    int length = 1;
    if (HIBYTE(value)) {
        text[0] = (char)HIBYTE(value);
        text[1] = (char)LOBYTE(value);
        length = 2;
        editor->pending_lead = 0;
    } else if (editor->pending_lead) {
        text[0] = editor->pending_lead;
        text[1] = (char)value;
        length = 2;
        editor->pending_lead = 0;
    } else if (IsDBCSLeadByte((BYTE)value)) {
        editor->pending_lead = (char)value;
        return 1;
    } else {
        text[0] = (char)value;
    }
    WCHAR units[2];
    int count = MultiByteToWideChar(CP_ACP, 0, text, length, units, 2);
    unsigned long codepoint;
    if (!count) {
        if (length != 1 || (unsigned char)text[0] >= 0x80)
            return 0;
        codepoint = (unsigned char)text[0];
    } else {
        codepoint = units[0];
    }
    if (count == 2) {
        if (units[0] < 0xD800 || units[0] > 0xDBFF ||
            units[1] < 0xDC00 || units[1] > 0xDFFF)
            return 0;
        codepoint = 0x10000 + (((unsigned long)units[0] - 0xD800) << 10) + units[1] - 0xDC00;
    }
    for (unsigned int repeat = 0; repeat < repeats; ++repeat)
        if (!editor_core_send(editor->core, EDITOR_HANDLE_KEY, &codepoint))
            return 0;
    return 1;
}

static int text_extent(HDC dc, const char *text, int length, int fallback_width,
    SIZE *extent)
{
    if (GetTextExtentPointA(dc, text, length, extent) && extent->cx > 0)
        return 1;
    extent->cx = fallback_width > 0 ? fallback_width : 1;
    extent->cy = 0;
    return 0;
}

static int layout_editor_line(HDC dc, EditorControl *editor, size_t index,
    size_t byte_limit, int top, int paint, int hit_x, size_t *hit_column)
{
    EditorCharacter character = {0};
    character.line = index;
    EditorSelection selection = {0};
    selection.line = index;
    if (paint)
        editor_core_send(editor->core, EDITOR_GET_SELECTION, &selection);
    COLORREF normal_color = GetTextColor(dc);
    COLORREF normal_background = GetBkColor(dc);
    const LexerLine *syntax = paint && editor->syntax && editor->syntax_ready &&
        editor->syntax->lexer && index >= editor->syntax->first_line &&
        index - editor->syntax->first_line < editor->syntax->line_count ?
        &editor->syntax->lines[index - editor->syntax->first_line].parsed : NULL;
    size_t token_index = 0;
    EditorSettings settings;
    editor_core_send(editor->core, EDITOR_GET_SETTINGS, &settings);
    SIZE space_size = { 0, 0 };
    text_extent(dc, " ", 1, editor->character_width, &space_size);
    int column_limit = -1;
    if (space_size.cx > 0 && settings.maxcol && settings.maxcol <= (size_t)(INT_MAX / space_size.cx))
        column_limit = (int)settings.maxcol * space_size.cx;
    int width = 0;
    if (hit_column)
        *hit_column = 0;
    while (character.column < byte_limit &&
        editor_core_send(editor->core, EDITOR_GET_CHARACTER, &character)) {
        char text[8];
        int length = character_to_ansi(character.value, text, sizeof(text));
        SIZE extent = { 0, 0 };
        text_extent(dc, text, length, editor->character_width, &extent);
        if (character.hidden) {
            extent.cx = 0;
            length = 0;
        }
        if (character.value == '\t') {
            if (space_size.cx <= 0 || settings.tabwidth > (size_t)(INT_MAX / space_size.cx))
                break;
            int stop = (int)settings.tabwidth * space_size.cx;
            extent.cx = stop - width % stop;
            length = 0;
        }
        if (extent.cx > INT_MAX - width)
            break;
        if (hit_column) {
            if (hit_x < width + (extent.cx + 1) / 2)
                break;
            *hit_column = character.column + character.length;
        }
        if (paint) {
            int left = editor->text_left - editor->left_pixel + width;
            COLORREF color = normal_color;
            if (syntax) {
                while (token_index < syntax->count && character.column >=
                    syntax->tokens[token_index].start + syntax->tokens[token_index].length)
                    ++token_index;
                color = syntax_color(token_index < syntax->count ?
                    syntax->tokens[token_index].kind : LEXER_TEXT);
            }
            SetTextColor(dc, color);
            int selected = selection.active && character.column >= selection.start && character.column < selection.end;
            if (selected) {
                RECT highlight;
                highlight.left = left;
                highlight.top = top;
                highlight.right = left + extent.cx;
                highlight.bottom = top + editor->line_height;

                SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
                SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
                ExtTextOutA(dc, left, top, ETO_OPAQUE, &highlight, text, length, NULL);
                SetTextColor(dc, normal_color);
            } else {
                TextOutA(dc, left, top, text, length);
                if (column_limit >= 0 && width + extent.cx > column_limit) {
                    int overflow = column_limit > width ? column_limit - width : 0;
                    RECT highlight;
                    highlight.left = left + overflow;
                    highlight.top = top;
                    highlight.right = left + extent.cx;
                    highlight.bottom = top + editor->line_height;
                    SetTextColor(dc, RGB(255, 255, 255));
                    SetBkColor(dc, RGB(255, 0, 0));
                    ExtTextOutA(dc, left, top, ETO_OPAQUE | ETO_CLIPPED, &highlight, text, length, NULL);
                }
            }
        }
        width += extent.cx;
        character.column += character.length;
    }
    if (paint && selection.active && selection.includes_newline) {
        SIZE space = { 0, 0 };
        text_extent(dc, " ", 1, editor->character_width, &space);
        int left = editor->text_left - editor->left_pixel + width;
        RECT highlight;
        highlight.left = left;
        highlight.top = top;
        highlight.right = left + space.cx;
        highlight.bottom = top + editor->line_height;
        SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
        ExtTextOutA(dc, left, top, ETO_OPAQUE, &highlight, "", 0, NULL);
    }
    SetTextColor(dc, normal_color);
    SetBkColor(dc, normal_background);
    return width;
}

static int hex_buffer(EditorControl *editor, EditorBuffer *buffer)
{
    buffer->data = NULL;
    if (!editor_core_send(editor->core, EDITOR_GET_LENGTH, &buffer->length))
        return 0;
    if (editor->hex_core != editor->core) {
        editor->hex_core = editor->core;
        editor->hex_offset = 0;
        editor->hex_first = 0;
        editor->hex_left = 0;
        editor->hex_low = 0;
        editor->hex_editing = 0;
        editor->hex_command[0] = 0;
    }
    if (editor->hex_offset > buffer->length) {
        editor->hex_offset = buffer->length;
        editor->hex_low = 0;
    }
    size_t last = buffer->length / 16;
    if (editor->hex_first > last)
        editor->hex_first = last;
    return 1;
}

static int hex_column(const EditorControl *editor)
{
    int slot = (int)(editor->hex_offset % 16);
    return editor->hex_ascii ? 61 + slot : 10 + slot * 3 + (slot >= 8) + editor->hex_low;
}

static size_t hex_page(HWND window, const EditorControl *editor)
{
    RECT rect;
    GetClientRect(window, &rect);
    int rows = (rect.bottom - 16) / editor->line_height;
    return rows > 0 ? (size_t)rows : 1;
}

static void position_hex_caret(HWND window, EditorControl *editor, int reveal)
{
    EditorBuffer buffer;
    if (!hex_buffer(editor, &buffer))
        return;
    size_t row = editor->hex_offset / 16;
    size_t page = hex_page(window, editor);
    RECT rect;
    GetClientRect(window, &rect);
    int left = hex_column(editor) * editor->character_width;
    int minimum = 10 * editor->character_width;
    int available = rect.right > 24 ? rect.right - 24 : 1;
    if (reveal) {
        if (row < editor->hex_first)
            editor->hex_first = row;
        else if (row - editor->hex_first >= page)
            editor->hex_first = row - page + 1;
        if (left - editor->hex_left < minimum)
            editor->hex_left = left - minimum;
        else if (left - editor->hex_left >= available)
            editor->hex_left = left - available + editor->character_width;
    }
    int page_width = rect.right - 16 - minimum;
    if (page_width < 1)
        page_width = 1;
    SCROLLINFO scroll = { 0 };
    scroll.cbSize = sizeof(scroll);
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    scroll.nMax = 68 * editor->character_width - 1;
    scroll.nPage = (UINT)page_width;
    int maximum = scroll.nMax + 1 - page_width;
    if (maximum < 0)
        maximum = 0;
    if (editor->hex_left > maximum)
        editor->hex_left = maximum;
    if (editor->hex_left < 0)
        editor->hex_left = 0;
    scroll.nPos = editor->hex_left;
    SetScrollInfo(window, SB_HORZ, &scroll, TRUE);
    if (GetFocus() == window) {
        if (row < editor->hex_first || row - editor->hex_first >= page ||
            left - editor->hex_left < minimum || left - editor->hex_left >= rect.right - 8)
            SetCaretPos(-2, -editor->line_height);
        else
            SetCaretPos(8 + left - editor->hex_left,
                8 + (int)(row - editor->hex_first) * editor->line_height);
    }
}

static void paint_hex_editor(HDC dc, EditorControl *editor, int width, int height, int focused)
{
    RECT rect;
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    FillRect(dc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
    editor->bitmap_valid = 0;
    EditorBuffer buffer;
    if (!hex_buffer(editor, &buffer))
        return;
    HGDIOBJ old_font = SelectObject(dc, editor->font);
    SetBkMode(dc, TRANSPARENT);
    size_t rows = height > 16 ? (size_t)((height - 17) / editor->line_height + 1) : 0;
    int saved = SaveDC(dc);
    if (saved)
        IntersectClipRect(dc, 8, 8, width - 8, height - 8);
    for (size_t row = 0; row < rows; ++row) {
        size_t index = editor->hex_first + row;
        if (index > buffer.length / 16)
            break;
        size_t offset = index * 16;
        char line[79];
        memset(line, ' ', 78);
        line[78] = 0;
        char address[9];
        snprintf(address, sizeof(address), "%08lX", (unsigned long)offset);
        memcpy(line, address, 8);
        line[60] = '|';
        line[77] = '|';
        static const char digits[] = "0123456789ABCDEF";
        size_t count = buffer.length - offset;
        if (count > 16)
            count = 16;
        unsigned char bytes[16];
        EditorReadRange range;
        range.offset = offset;
        range.data = bytes;
        range.length = count;
        if (!editor_core_send(editor->core, EDITOR_READ_RANGE, &range))
            break;
        for (size_t slot = 0; slot < count; ++slot) {
            unsigned char byte = bytes[slot];
            size_t column = 10 + slot * 3 + (slot >= 8);
            line[column] = digits[byte >> 4];
            line[column + 1] = digits[byte & 15];
            line[61 + slot] = byte >= 32 && byte <= 126 ? (char)byte : '.';
        }
        int top = 8 + (int)row * editor->line_height;
        int left = 8 - editor->hex_left;
        int body_left = 8 + 10 * editor->character_width;
        RECT body;
        body.left = body_left;
        body.top = top;
        body.right = width - 8;
        body.bottom = top + editor->line_height;
        SetTextColor(dc, RGB(225, 225, 225));
        if (body.right > body.left)
            ExtTextOutA(dc, left + 10 * editor->character_width, top, ETO_CLIPPED,
                &body, line + 10, 68, NULL);
        SetTextColor(dc, RGB(86, 200, 255));
        TextOutA(dc, 8, top, line, 8);
        if (focused && editor->hex_offset / 16 == index) {
            int slot = (int)(editor->hex_offset % 16);
            for (int field = 0; field < 2; ++field) {
                int column = field ? 61 + slot : 10 + slot * 3 + (slot >= 8);
                int length = field ? 1 : 2;
                RECT highlight;
                highlight.left = left +
                        column * editor->character_width;
                highlight.top = top;
                highlight.right = left + (column + length) *
                        editor->character_width;
                highlight.bottom = top + editor->line_height;
                if (highlight.left < body_left)
                    highlight.left = body_left;
                if (highlight.right > width - 8)
                    highlight.right = width - 8;
                if (highlight.right <= highlight.left)
                    continue;
                SetTextColor(dc, RGB(255, 255, 255));
                SetBkColor(dc, field == editor->hex_ascii ? RGB(40, 85, 130) : RGB(60, 60, 60));
                ExtTextOutA(dc, left + column * editor->character_width, top,
                    ETO_OPAQUE | ETO_CLIPPED, &highlight,
                    line + column, length, NULL);
            }
        }
    }
    if (saved)
        RestoreDC(dc, saved);
    SelectObject(dc, old_font);
}

static int hex_edit(EditorControl *editor, size_t remove_length, const unsigned char *data, size_t length)
{
    EditorByteEdit edit;
    edit.offset = editor->hex_offset;
    edit.remove_length = remove_length;
    edit.data = data;
    edit.length = length;
    return editor_core_send(editor->core, EDITOR_REPLACE_BYTES, &edit);
}

static int handle_hex_jump(EditorControl *editor, size_t length, unsigned long key)
{
    size_t pending = strlen(editor->hex_command);
    if (pending && editor->hex_command[0] == 'g') {
        editor->hex_command[0] = 0;
        if (key == 'g') {
            editor->hex_offset = 0;
            editor->hex_low = 0;
            return 1;
        }
    } else if (pending && editor->hex_command[0] == 'x') {
        if (key == 'g') {
            char *end;
            unsigned long offset = strtoul(editor->hex_command + 1, &end, 16);
            int valid = end != editor->hex_command + 1 &&
                (!*end || (*end == ',' && !end[1]));
            editor->hex_command[0] = 0;
            if (!valid)
                return -1;
            editor->hex_offset = offset > length ? length : (size_t)offset;
            editor->hex_low = 0;
            return 1;
        }
        int digit = (key >= '0' && key <= '9') || (key >= 'a' && key <= 'f') ||
            (key >= 'A' && key <= 'F');
        if (digit) {
            if (pending >= 9 || editor->hex_command[pending - 1] == ',') {
                editor->hex_command[0] = 0;
                return -1;
            }
            editor->hex_command[pending] = (char)key;
            editor->hex_command[pending + 1] = 0;
            return 1;
        }
        if (key == ',' && pending > 1 && editor->hex_command[pending - 1] != ',') {
            editor->hex_command[pending] = ',';
            editor->hex_command[pending + 1] = 0;
            return 1;
        }
        if (key == 8) {
            editor->hex_command[pending - 1] = 0;
            return 1;
        }
        editor->hex_command[0] = 0;
    }
    if (key == 'G') {
        editor->hex_offset = length;
        editor->hex_low = 0;
        return 1;
    }
    if (key == 'g' || key == 'x') {
        editor->hex_command[0] = (char)key;
        editor->hex_command[1] = 0;
        return 1;
    }
    return 0;
}

static int handle_hex_key(HWND window, EditorControl *editor, unsigned long key, int control)
{
    EditorBuffer buffer;
    if (!hex_buffer(editor, &buffer))
        return 0;
    size_t length = buffer.length;
    size_t distance = hex_page(window, editor) * 16;
    if (!editor->hex_editing) {
        int jump = handle_hex_jump(editor, length, key);
        if (jump)
            return jump > 0;
    }
    if (!editor->hex_editing && (key == 'i' || key == 'r')) {
        editor->hex_insert = key == 'i';
        editor->hex_editing = 1;
        editor->hex_low = 0;
        return 1;
    }
    if (!editor->hex_editing) {
        switch (key) {
        case 'h':
            if (editor->hex_offset)
                --editor->hex_offset;
            editor->hex_low = 0;
            return 1;
        case 'l':
            if (editor->hex_offset < length)
                ++editor->hex_offset;
            editor->hex_low = 0;
            return 1;
        case 'j': key = EDITOR_KEY_DOWN; break;
        case 'k': key = EDITOR_KEY_UP; break;
        case '0': key = EDITOR_KEY_HOME; control = 0; break;
        case '$': key = EDITOR_KEY_END; control = 0; break;
        case 'u': key = 26; break;
        case 21:
        case 'd': case 4: {
            size_t rows = hex_page(window, editor) / 2;
            distance = (rows ? rows : 1) * 16;
            key = key == 21 ? EDITOR_KEY_PAGE_UP : EDITOR_KEY_PAGE_DOWN;
            break;
        }
        case 'b': case 2: key = EDITOR_KEY_PAGE_UP; break;
        case 'f': case 6: key = EDITOR_KEY_PAGE_DOWN; break;
        default: break;
        }
    }
    switch (key) {
    case EDITOR_KEY_LEFT:
        if (!editor->hex_ascii && editor->hex_low)
            editor->hex_low = 0;
        else if (editor->hex_offset) {
            --editor->hex_offset;
            editor->hex_low = !editor->hex_ascii;
        }
        return 1;
    case EDITOR_KEY_RIGHT:
        if (!editor->hex_ascii && !editor->hex_low && editor->hex_offset < length)
            editor->hex_low = 1;
        else if (editor->hex_offset < length) {
            ++editor->hex_offset;
            editor->hex_low = 0;
        }
        return 1;
    case EDITOR_KEY_UP: distance = 16; break;
    case EDITOR_KEY_DOWN: distance = 16; break;
    case EDITOR_KEY_PAGE_UP: case EDITOR_KEY_PAGE_DOWN: break;
    case EDITOR_KEY_HOME:
        editor->hex_offset = control ? 0 : editor->hex_offset / 16 * 16;
        editor->hex_low = 0;
        return 1;
    case EDITOR_KEY_END: {
        size_t end = editor->hex_offset / 16 * 16;
        end += length - end < 15 ? length - end : 15;
        editor->hex_offset = control ? length : end;
        editor->hex_low = 0;
        return 1;
    }
    case EDITOR_KEY_DELETE:
        editor->hex_low = 0;
        return editor->hex_offset == length || hex_edit(editor, 1, NULL, 0);
    case 8:
        editor->hex_low = 0;
        if (!editor->hex_offset)
            return 1;
        --editor->hex_offset;
        if (!hex_edit(editor, 1, NULL, 0)) {
            ++editor->hex_offset;
            return 0;
        }
        return 1;
    case 9:
        editor->hex_ascii = !editor->hex_ascii;
        editor->hex_low = 0;
        return 1;
    case 27:
        editor->hex_low = 0;
        editor->hex_editing = 0;
        return 1;
    case 26: case 25: case 18:
        editor->hex_low = 0;
        return editor_core_send(editor->core, key == 26 ? EDITOR_UNDO : EDITOR_REDO, NULL);
    default: {
        if (!editor->hex_editing)
            return 0;
        unsigned char byte;
        size_t remove_length = editor->hex_offset < length ? 1 : 0;
        if (editor->hex_ascii) {
            if (key < 32 || key > 126)
                return 0;
            byte = (unsigned char)key;
            if (editor->hex_insert)
                remove_length = 0;
        } else {
            int nibble = key >= '0' && key <= '9' ? (int)(key - '0') :
                key >= 'a' && key <= 'f' ? (int)(key - 'a') + 10 :
                key >= 'A' && key <= 'F' ? (int)(key - 'A') + 10 : -1;
            if (nibble < 0)
                return 0;
            if (editor->hex_insert && !editor->hex_low)
                remove_length = 0;
            byte = 0;
            if (remove_length) {
                EditorReadRange range;
                range.offset = editor->hex_offset;
                range.data = &byte;
                range.length = 1;
                if (!editor_core_send(editor->core, EDITOR_READ_RANGE, &range))
                    return 0;
            }
            byte = editor->hex_low ? (unsigned char)((byte & 0xF0) | nibble) :
                (unsigned char)((byte & 0x0F) | (nibble << 4));
        }
        if (!hex_edit(editor, remove_length, &byte, 1))
            return 0;
        if (editor->hex_ascii || editor->hex_low) {
            ++editor->hex_offset;
            editor->hex_low = 0;
        } else {
            editor->hex_low = 1;
        }
        return 1;
    }
    }
    if (key == EDITOR_KEY_UP || key == EDITOR_KEY_PAGE_UP)
        editor->hex_offset -= distance > editor->hex_offset ? editor->hex_offset : distance;
    else
        editor->hex_offset += distance > length - editor->hex_offset ? length - editor->hex_offset : distance;
    return 1;
}

static void update_editor_caret(HWND window, EditorControl *editor)
{
    if (window != active_editor(GetParent(window))) {
        InvalidateRect(window, NULL, FALSE);
        return;
    }
    EditorBufferSwitch request;
    if (editor_core_send(editor->core, EDITOR_TAKE_BUFFER_SWITCH, &request) && request.count) {
        SendMessageA(GetParent(window), WM_EDITOR_SWITCH_BUFFER, 0, (LPARAM)&request);
        return;
    }
    EditorMode mode;
    editor_core_send(editor->core, EDITOR_GET_MODE, &mode);
    if (editor->hex_mode) {
        EditorDirectory directory;
        editor_core_send(editor->core, EDITOR_GET_DIRECTORY, &directory);
        if (directory.active) {
            editor->hex_mode = 0;
            ShowScrollBar(window, SB_HORZ, FALSE);
        }
    }
    EditorStatus status;
    editor_core_send(editor->core, EDITOR_GET_STATUS, &status);
    if (editor->hex_mode)
        position_hex_caret(window, editor, 1);
    SendMessageA(GetParent(window), WM_EDITOR_MODE_CHANGED, (WPARAM)mode, (LPARAM)status);
    if (editor->hex_mode) {
        InvalidateRect(window, NULL, FALSE);
        return;
    }
    EditorCaret caret;
    editor_core_send(editor->core, EDITOR_GET_CARET, &caret);
    HDC dc = GetDC(window);
    HGDIOBJ old_font = SelectObject(dc, editor->font);
    int caret_width = layout_editor_line(dc, editor, caret.line, caret.column,
        0, 0, 0, NULL);
    SelectObject(dc, old_font);
    ReleaseDC(window, dc);

    RECT rect;
    GetClientRect(window, &rect);
    int visible_lines = (rect.bottom - 16) / editor->line_height;
    if (visible_lines < 1)
        visible_lines = 1;
    size_t page_lines = (size_t)visible_lines;
    editor_core_send(editor->core, EDITOR_SET_PAGE_SIZE, &page_lines);
    if (caret.line < editor->first_line)
        editor->first_line = caret.line;
    else if (caret.line - editor->first_line >= (size_t)visible_lines)
        editor->first_line = caret.line - visible_lines + 1;

    int visible_width = rect.right > editor->text_left + 10 ? rect.right - editor->text_left - 10 : 1;
    if (caret_width < editor->left_pixel)
        editor->left_pixel = caret_width;
    else if (caret_width - editor->left_pixel > visible_width)
        editor->left_pixel = caret_width - visible_width;
    if (GetFocus() == window)
        SetCaretPos(editor->text_left + caret_width - editor->left_pixel,
            8 + (int)(caret.line - editor->first_line) * editor->line_height);
    InvalidateRect(window, NULL, FALSE);
}

static void clear_line_cache(EditorControl *editor)
{
    for (size_t index = 0; index < editor->line_count; ++index)
        free(editor->lines[index].text);
    free(editor->lines);
    editor->lines = NULL;
    editor->line_count = 0;
    editor->bitmap_valid = 0;
}

static void paint_editor(HDC dc, EditorControl *editor, int width, int height, int buffered, int focused)
{
    size_t syntax_rows = height > 16 ? (size_t)((height - 17) / editor->line_height + 1) : 1;
    int syntax_ready = update_syntax_cache(editor->syntax, editor->core, editor->first_line, syntax_rows);
    if (!syntax_ready || syntax_ready != editor->syntax_ready)
        editor->bitmap_valid = 0;
    editor->syntax_ready = syntax_ready;
    EditorCaret primary;
    editor_core_send(editor->core, EDITOR_GET_CARET, &primary);
    EditorSettings settings;
    editor_core_send(editor->core, EDITOR_GET_SETTINGS, &settings);
    size_t rows = height > 16 ? (size_t)((height - 17) / editor->line_height + 1) : 0;
    if (editor->line_count != rows) {
        clear_line_cache(editor);
        if (rows) {
            editor->lines = calloc(rows, sizeof(*editor->lines));
            if (editor->lines)
                editor->line_count = rows;
        }
    }
    int reuse = buffered && editor->bitmap_valid && editor->line_count == rows;
    size_t distance = editor->first_line > editor->painted_first_line ?
        editor->first_line - editor->painted_first_line : editor->painted_first_line - editor->first_line;
    if (reuse && (editor->left_pixel != editor->painted_left_pixel || distance >= rows ||
        settings.maxcol != editor->painted_settings.maxcol ||
        settings.tabwidth != editor->painted_settings.tabwidth))
        reuse = 0;
    if (reuse && distance) {
        int shift = (int)distance * editor->line_height;
        int content_height = height - 16;
        BOOL moved;
        if (editor->first_line > editor->painted_first_line) {
            moved = BitBlt(dc, 0, 8, width, content_height - shift, dc, 0, 8 + shift, SRCCOPY);
            if (content_height % editor->line_height) {
                size_t partial = editor->painted_first_line + rows - 1;
                editor->lines[partial % rows].valid = 0;
            }
        } else {
            moved = BitBlt(dc, 0, 8 + shift, width, content_height - shift, dc, 0, 8, SRCCOPY);
        }
        if (!moved)
            reuse = 0;
    }
    if (!reuse) {
        RECT rect;
        rect.left = 0;
        rect.top = 0;
        rect.right = width;
        rect.bottom = height;
        FillRect(dc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
        for (size_t index = 0; index < editor->line_count; ++index)
            editor->lines[index].valid = 0;
    }

    HGDIOBJ old_font = SelectObject(dc, editor->font);
    SetBkMode(dc, TRANSPARENT);
    SIZE space_size = { 0, 0 };
    text_extent(dc, " ", 1, editor->character_width, &space_size);
    int guide = -1;
    if (space_size.cx > 0 && settings.maxcol &&
        settings.maxcol <= (size_t)((INT_MAX - editor->text_left) / space_size.cx)) {
        int position = editor->text_left + (int)settings.maxcol * space_size.cx - editor->left_pixel;
        if (position >= editor->text_left && position < width - 8)
            guide = position;
    }
    HBRUSH guide_brush = guide >= 0 ? CreateSolidBrush(RGB(64, 64, 64)) : NULL;
    size_t last_text_line = (size_t)-1;
    for (size_t row = 0; row < rows; ++row) {
        size_t index = editor->first_line + row;
        EditorLine line = {0};
        line.index = index;
        int exists = index <= last_text_line && editor_core_send(editor->core, EDITOR_GET_LINE, &line);
        if (exists && line.truncated)
            last_text_line = index;
        EditorSelection selection = {0};
        selection.line = index;
        if (exists)
            editor_core_send(editor->core, EDITOR_GET_SELECTION, &selection);
        EditorLineCaret caret = {0};
        caret.line = index;
        if (exists && focused && index != primary.line)
            editor_core_send(editor->core, EDITOR_GET_LINE_CARET, &caret);
        CachedLine *cached = editor->lines ? &editor->lines[index % rows] : NULL;
        size_t syntax_version = syntax_ready && editor->syntax && editor->syntax->lexer &&
            index >= editor->syntax->first_line &&
            index - editor->syntax->first_line < editor->syntax->line_count ?
            editor->syntax->lines[index - editor->syntax->first_line].version : 0;
        if (reuse && cached && cached->valid && cached->index == index && cached->exists == exists &&
            cached->syntax_version == syntax_version &&
            cached->length == line.length && cached->selection.start == selection.start &&
            cached->selection.end == selection.end && cached->selection.active == selection.active &&
            cached->selection.includes_newline == selection.includes_newline &&
            cached->caret.active == caret.active && cached->caret.column == caret.column &&
            cached->caret.advance == caret.advance &&
            (!line.length || memcmp(cached->text, line.text, line.length) == 0))
            continue;

        int top = 8 + (int)row * editor->line_height;
        RECT dirty;
        dirty.left = 0;
        dirty.top = top;
        dirty.right = width;
        dirty.bottom = top + editor->line_height;
        if (dirty.bottom > height - 8)
            dirty.bottom = height - 8;
        FillRect(dc, &dirty, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (guide_brush) {
            RECT marker;
            marker.left = guide;
            marker.top = dirty.top;
            marker.right = guide + 1;
            marker.bottom = dirty.bottom;
            FillRect(dc, &marker, guide_brush);
        }
        if (exists) {
            int saved_dc = SaveDC(dc);
            if (saved_dc) {
                IntersectClipRect(dc, 8, top, width - 8, dirty.bottom);
                SetTextColor(dc, RGB(0, 255, 255));
                char number[6] = "#####";
                if (index < 99999)
                    wsprintfA(number, "%5lu", (unsigned long)(index + 1));
                TextOutA(dc, 8, top, number, 5);
                IntersectClipRect(dc, editor->text_left, top, width - 8, dirty.bottom);
                SetTextColor(dc, RGB(225, 225, 225));
                layout_editor_line(dc, editor, index, line.length, top, 1, 0, NULL);
                if (caret.active) {
                    int offset = layout_editor_line(dc, editor, index, caret.column, 0, 0, 0, NULL);
                    SIZE space = { 0, 0 };
                    text_extent(dc, " ", 1, editor->character_width, &space);
                    int left = editor->text_left - editor->left_pixel + offset;
                    if (space.cx > 0 && caret.advance <= (size_t)((INT_MAX - offset) / space.cx)) {
                        left += (int)caret.advance * space.cx;
                        RECT cursor;
                        cursor.left = left;
                        cursor.top = top;
                        cursor.right = left + 2;
                        cursor.bottom = top + editor->line_height;
                        FillRect(dc, &cursor, (HBRUSH)GetStockObject(WHITE_BRUSH));
                    }
                }
                RestoreDC(dc, saved_dc);
            } else if (cached) {
                cached->valid = 0;
                continue;
            }
        }
        if (cached) {
            cached->valid = 0;
            char *text = realloc(cached->text, line.length ? line.length : 1);
            if (text) {
                cached->text = text;
                if (line.length)
                    memcpy(text, line.text, line.length);
                cached->length = line.length;
                cached->index = index;
                cached->exists = exists;
                cached->selection = selection;
                cached->caret = caret;
                cached->syntax_version = syntax_version;
                cached->valid = buffered;
            }
        }
    }
    SelectObject(dc, old_font);
    if (guide_brush)
        DeleteObject(guide_brush);
    editor->painted_settings = settings;
    editor->painted_first_line = editor->first_line;
    editor->painted_left_pixel = editor->left_pixel;
    editor->bitmap_valid = buffered && editor->line_count == rows;
}

static int send_editor_key(EditorControl *editor, unsigned long key)
{
    EditorMode mode;
    editor_core_send(editor->core, EDITOR_GET_MODE, &mode);
    int paging = key == EDITOR_KEY_PAGE_UP || key == EDITOR_KEY_PAGE_DOWN ||
        ((mode == EDITOR_MODE_COMMAND || mode == EDITOR_MODE_VISUAL ||
            mode == EDITOR_MODE_VISUAL_LINE || mode == EDITOR_MODE_VISUAL_BLOCK) &&
            (key == 2 || key == 6 || key == 4 || key == 21));
    EditorCaret before;
    if (paging)
        editor_core_send(editor->core, EDITOR_GET_CARET, &before);
    int success = editor_core_send(editor->core, EDITOR_HANDLE_KEY, &key);
    if (success && paging) {
        EditorCaret after;
        editor_core_send(editor->core, EDITOR_GET_CARET, &after);
        if (after.line >= before.line) {
            editor->first_line += after.line - before.line;
        } else {
            size_t distance = before.line - after.line;
            editor->first_line = distance > editor->first_line ? 0 : editor->first_line - distance;
        }
    }
    return success;
}

static int set_editor_font(HWND window, EditorControl *editor, int points)
{
    HDC dc = GetDC(window);
    HFONT font = CreateFontA(-MulDiv(points, GetDeviceCaps(dc, LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FIXED_PITCH, "Courier New");
    if (!font && editor->font) {
        ReleaseDC(window, dc);
        return 0;
    }
    if (!font)
        font = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
    HGDIOBJ old_font = SelectObject(dc, font);
    TEXTMETRICA metrics;
    GetTextMetricsA(dc, &metrics);
    SIZE number_size = { 0, 0 };
    int character_width = metrics.tmAveCharWidth > 0 ? metrics.tmAveCharWidth : 1;
    text_extent(dc, "00000", 5, 5 * character_width, &number_size);
    SelectObject(dc, old_font);
    ReleaseDC(window, dc);
    HFONT previous = editor->font;
    editor->font = font;
    editor->font_points = points;
    editor->line_height = metrics.tmHeight + metrics.tmExternalLeading;
    if (editor->line_height < 1)
        editor->line_height = 1;
    editor->text_left = 8 + number_size.cx + 16;
    editor->character_width = character_width;
    editor->bitmap_valid = 0;
    editor->pending_lead = 0;
    SendMessageA(GetDlgItem(GetParent(window), ID_COMMAND_LINE), WM_SETFONT, (WPARAM)font, TRUE);
    if (previous && previous != (HFONT)GetStockObject(SYSTEM_FIXED_FONT))
        DeleteObject(previous);
    if (GetFocus() == window) {
        DestroyCaret();
        CreateCaret(window, NULL, 2, editor->line_height);
        ShowCaret(window);
    }
    return 1;
}

LRESULT CALLBACK my_editor_wnd_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorControl *editor = (EditorControl *)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
    if (legacy_wheel_message && msg == legacy_wheel_message) {
        msg = WM_MOUSEWHEEL;
        wp = MAKEWPARAM(0, (WORD)(short)wp);
    }

    switch (msg) {
    case WM_CREATE: {
        if (!legacy_wheel_message)
            legacy_wheel_message = RegisterWindowMessageA("MSWHEEL_ROLLMSG");
        editor = calloc(1, sizeof(*editor));
        if (!editor)
            return -1;
        editor->core = ((LPCREATESTRUCTA)lp)->lpCreateParams;
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)editor);
        set_editor_font(hWnd, editor, 18);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
    case WM_EDITOR_REFRESH:
        update_editor_caret(hWnd, editor);
        return 0;
    case WM_EDITOR_BUFFER_CHANGED:
        editor->bitmap_valid = 0;
        editor->first_line = 0;
        editor->left_pixel = 0;
        editor->pending_lead = 0;
        editor->hex_core = NULL;
        update_editor_caret(hWnd, editor);
        return 0;
    case WM_SETFOCUS:
        SendMessageA(GetParent(hWnd), WM_EDITOR_FOCUS, (WPARAM)hWnd, 0);
        CreateCaret(hWnd, NULL, 2, editor->line_height);
        update_editor_caret(hWnd, editor);
        ShowCaret(hWnd);
        return 0;
    case WM_KILLFOCUS:
        editor->pending_lead = 0;
        editor->hex_command[0] = 0;
        DestroyCaret();
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_SIZE:
        if (editor)
            update_editor_caret(hWnd, editor);
        return 0;
    case WM_HSCROLL: {
        if (!editor || !editor->hex_mode)
            break;
        SCROLLINFO scroll = { 0 };
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_ALL;
        GetScrollInfo(hWnd, SB_HORZ, &scroll);
        int position = editor->hex_left;
        int maximum = scroll.nMax - (int)scroll.nPage + 1;
        if (maximum < 0)
            maximum = 0;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: position -= editor->character_width; break;
        case SB_LINERIGHT: position += editor->character_width; break;
        case SB_PAGELEFT: position -= (int)scroll.nPage; break;
        case SB_PAGERIGHT: position += (int)scroll.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: position = scroll.nTrackPos; break;
        case SB_LEFT: position = 0; break;
        case SB_RIGHT: position = maximum; break;
        default: return 0;
        }
        editor->hex_left = position < 0 ? 0 : position > maximum ? maximum : position;
        position_hex_caret(hWnd, editor, 0);
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!editor)
            return 0;
        editor->wheel_delta += (short)HIWORD(wp);
        int notches = editor->wheel_delta / WHEEL_DELTA;
        editor->wheel_delta %= WHEEL_DELTA;
        if (!notches)
            return 0;
        UINT scroll_lines = 3;
        if (!SystemParametersInfoA(SPI_GETWHEELSCROLLLINES, 0, &scroll_lines, 0))
            scroll_lines = 3;
        if (!scroll_lines)
            return 0;
        RECT rect;
        GetClientRect(hWnd, &rect);
        int visible_lines = (rect.bottom - 16) / editor->line_height;
        size_t page_lines = visible_lines > 0 ? (size_t)visible_lines : 1;
        size_t rows = scroll_lines == WHEEL_PAGESCROLL ? page_lines : (size_t)scroll_lines;
        size_t steps = (size_t)(notches < 0 ? -notches : notches);
        size_t distance = steps > (size_t)-1 / rows ? (size_t)-1 : steps * rows;
        if (editor->hex_mode) {
            EditorBuffer buffer;
            if (!hex_buffer(editor, &buffer))
                return 0;
            size_t count = buffer.length / 16 + 1;
            size_t last = count > page_lines ? count - page_lines : 0;
            size_t first = editor->hex_first > last ? last : editor->hex_first;
            if (notches > 0)
                first -= distance > first ? first : distance;
            else
                first += distance > last - first ? last - first : distance;
            editor->hex_first = first;
            position_hex_caret(hWnd, editor, 0);
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        EditorLineCount known;
        if (!editor_core_send(editor->core, EDITOR_GET_KNOWN_LINE_COUNT, &known))
            return 0;
        size_t count = known.count;
        if (!known.complete && count <= (size_t)-1 - page_lines)
            count += page_lines;
        size_t last = count > page_lines ? count - page_lines : 0;
        size_t first = editor->first_line > last ? last : editor->first_line;
        if (notches > 0)
            first = distance > first ? 0 : first - distance;
        else
            first += distance > last - first ? last - first : distance;
        if (first == editor->first_line)
            return 0;
        editor->first_line = first;
        if (GetFocus() == hWnd) {
            EditorCaret caret;
            editor_core_send(editor->core, EDITOR_GET_CARET, &caret);
            if (caret.line < first || caret.line - first >= page_lines) {
                SetCaretPos(-2, -editor->line_height);
            } else {
                HDC dc = GetDC(hWnd);
                HGDIOBJ old_font = SelectObject(dc, editor->font);
                int width = layout_editor_line(dc, editor, caret.line, caret.column, 0, 0, 0, NULL);
                SelectObject(dc, old_font);
                ReleaseDC(hWnd, dc);
                SetCaretPos(editor->text_left + width - editor->left_pixel,
                    8 + (int)(caret.line - first) * editor->line_height);
            }
        }
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        editor->pending_lead = 0;
        editor->hex_command[0] = 0;
        SetFocus(hWnd);
        int mouse_x = (short)LOWORD(lp);
        int mouse_y = (short)HIWORD(lp);
        if (editor->hex_mode) {
            EditorBuffer buffer;
            if (!hex_buffer(editor, &buffer))
                return 0;
            int column = (mouse_x - 8 + editor->hex_left) / editor->character_width;
            if (mouse_x < 8 + 10 * editor->character_width)
                column = 10;
            int slot = 0;
            editor->hex_ascii = column >= 60;
            if (editor->hex_ascii) {
                slot = column - 61;
                editor->hex_low = 0;
            } else {
                int best = INT_MAX;
                for (int index = 0; index < 16; ++index) {
                    int start = 10 + index * 3 + (index >= 8);
                    int delta = column < start ? start - column : column > start + 1 ? column - start - 1 : 0;
                    if (delta < best) {
                        best = delta;
                        slot = index;
                        editor->hex_low = column > start;
                    }
                }
            }
            if (slot < 0)
                slot = 0;
            if (slot > 15)
                slot = 15;
            size_t row = editor->hex_first + (mouse_y > 8 ? (mouse_y - 8) / editor->line_height : 0);
            size_t last = buffer.length / 16;
            if (row > last)
                row = last;
            size_t offset = row * 16;
            editor->hex_offset = offset + ((size_t)slot > buffer.length - offset ? buffer.length - offset : (size_t)slot);
            if (editor->hex_offset == buffer.length)
                editor->hex_low = 0;
            update_editor_caret(hWnd, editor);
            return 0;
        }
        EditorLineCount known;
        if (!editor_core_send(editor->core, EDITOR_GET_KNOWN_LINE_COUNT, &known))
            return 0;
        size_t count = known.count;
        EditorCaret caret = {0};
        caret.line = editor->first_line +
                (mouse_y > 8 ? (mouse_y - 8) / editor->line_height : 0);
        if (caret.line >= count)
            caret.line = count - 1;
        EditorLine line = {0};
        line.index = caret.line;
        editor_core_send(editor->core, EDITOR_GET_LINE, &line);
        int target = mouse_x >= editor->text_left ? mouse_x - editor->text_left + editor->left_pixel : 0;
        if (target > 0) {
            HDC dc = GetDC(hWnd);
            HGDIOBJ old_font = SelectObject(dc, editor->font);
            layout_editor_line(dc, editor, caret.line, line.length,
                0, 0, target, &caret.column);
            SelectObject(dc, old_font);
            ReleaseDC(hWnd, dc);
        }
        editor_core_send(editor->core, EDITOR_SET_CARET, &caret);
        if (msg == WM_LBUTTONDBLCLK) {
            EditorDirectory directory;
            editor_core_send(editor->core, EDITOR_GET_DIRECTORY, &directory);
            if (directory.active && !editor_core_send(editor->core, EDITOR_ACTIVATE_DIRECTORY_ENTRY, NULL))
                MessageBeep(MB_ICONWARNING);
        }
        update_editor_caret(hWnd, editor);
        return 0;
    }
    case WM_KEYDOWN: {
        if (GetKeyState(VK_CONTROL) < 0 &&
            (wp == VK_OEM_PLUS || wp == VK_ADD || wp == VK_OEM_MINUS || wp == VK_SUBTRACT)) {
            int direction = wp == VK_OEM_PLUS || wp == VK_ADD ? 1 : -1;
            int points = editor->font_points + direction * (int)LOWORD(lp);
            if (points < 6)
                points = 6;
            if (points > 72)
                points = 72;
            if (points != editor->font_points && !set_editor_font(hWnd, editor, points))
                MessageBeep(MB_ICONWARNING);
            SendMessageA(GetParent(hWnd), WM_SIZE, 0, 0);
            update_editor_caret(hWnd, editor);
            return 0;
        }
        unsigned long key;
        if (editor->hex_mode && wp == VK_INSERT) {
            editor->hex_command[0] = 0;
            editor->hex_insert = !editor->hex_insert;
            editor->hex_editing = 1;
            editor->hex_low = 0;
            update_editor_caret(hWnd, editor);
            return 0;
        }
        switch (wp) {
        case VK_ESCAPE: key = 27; break;
        case VK_DELETE: key = EDITOR_KEY_DELETE; break;
        case VK_LEFT: key = EDITOR_KEY_LEFT; break;
        case VK_RIGHT: key = EDITOR_KEY_RIGHT; break;
        case VK_UP: key = EDITOR_KEY_UP; break;
        case VK_DOWN: key = EDITOR_KEY_DOWN; break;
        case VK_HOME: key = EDITOR_KEY_HOME; break;
        case VK_END: key = EDITOR_KEY_END; break;
        case VK_PRIOR: key = EDITOR_KEY_PAGE_UP; break;
        case VK_NEXT: key = EDITOR_KEY_PAGE_DOWN; break;
        default: return DefWindowProcA(hWnd, msg, wp, lp);
        }
        if (SendMessageA(GetParent(hWnd), WM_EDITOR_WINDOW_KEY, (WPARAM)key, 0))
            return 0;
        editor->pending_lead = 0;
        for (unsigned int repeat = 0; repeat < LOWORD(lp); ++repeat) {
            int success = editor->hex_mode ?
                handle_hex_key(hWnd, editor, key, GetKeyState(VK_CONTROL) < 0) : send_editor_key(editor, key);
            if (!success) {
                MessageBeep(MB_ICONWARNING);
                break;
            }
        }
        update_editor_caret(hWnd, editor);
        return 0;
    }
    case WM_CHAR: {
        if (SendMessageA(GetParent(hWnd), WM_EDITOR_WINDOW_KEY, wp, 0))
            return 0;
        if (GetKeyState(VK_CONTROL) < 0 &&
            (wp == '+' || wp == '=' || wp == '-' || wp == '_' || wp == 0 || wp == 29 || wp == 31))
            return 0;
        if (editor->hex_mode) {
            for (unsigned int repeat = 0; repeat < LOWORD(lp); ++repeat) {
                if (!handle_hex_key(hWnd, editor, (unsigned long)wp, GetKeyState(VK_CONTROL) < 0)) {
                    MessageBeep(MB_ICONWARNING);
                    break;
                }
            }
            update_editor_caret(hWnd, editor);
            return 0;
        }
        if (wp >= ' ' && wp != 127) {
            if (!dispatch_ansi_key(editor, wp, LOWORD(lp)))
                MessageBeep(MB_ICONWARNING);
            update_editor_caret(hWnd, editor);
            return 0;
        }
        editor->pending_lead = 0;
        unsigned long key = (unsigned long)wp;
        for (unsigned int repeat = 0; repeat < LOWORD(lp); ++repeat) {
            if (!send_editor_key(editor, key)) {
                MessageBeep(MB_ICONWARNING);
                break;
            }
        }
        update_editor_caret(hWnd, editor);
        return 0;
    }
    case WM_PAINT: {
        RECT rect;
        int width, height;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hWnd, &ps);
        GetClientRect(hWnd, &rect);

        width = rect.right - rect.left;
        height = rect.bottom - rect.top;

        if (width > 0 && height > 0) {
            if (!editor->memdc)
                editor->memdc = CreateCompatibleDC(dc);

            if (editor->memdc && (!editor->back_bitmap ||
                width != editor->back_width || height != editor->back_height)) {
                HBITMAP replacement = CreateCompatibleBitmap(dc, width, height);
                if (replacement) {
                    HGDIOBJ previous = SelectObject(editor->memdc, replacement);
                    if (previous && previous != HGDI_ERROR) {
                        if (editor->back_bitmap)
                            DeleteObject(editor->back_bitmap);
                        else
                            editor->original_bitmap = previous;

                        editor->back_bitmap = replacement;
                        editor->back_width = width;
                        editor->back_height = height;
                        editor->bitmap_valid = 0;
                    } else {
                        DeleteObject(replacement);
                    }
                }
            }

            HDC draw_dc = editor->back_bitmap && editor->back_width == width &&
                editor->back_height == height ? editor->memdc : dc;
            if (editor->hex_mode)
                paint_hex_editor(draw_dc, editor, width, height, GetFocus() == hWnd);
            else
                paint_editor(draw_dc, editor, width, height, draw_dc == editor->memdc, GetFocus() == hWnd);

            if (draw_dc == editor->memdc)
                BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                    ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                    editor->memdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        if (editor) {
            clear_line_cache(editor);
            if (editor->back_bitmap) {
                SelectObject(editor->memdc, editor->original_bitmap);
                DeleteObject(editor->back_bitmap);
            }
            if (editor->memdc)
                DeleteDC(editor->memdc);
            if (editor->font && editor->font != (HFONT)GetStockObject(SYSTEM_FIXED_FONT))
                DeleteObject(editor->font);
            free(editor);
            SetWindowLongPtrA(hWnd, GWLP_USERDATA, 0);
        }
        break;
    }

    return DefWindowProcA(hWnd, msg, wp, lp);
}

static void update_document_title(HWND window, const MainWindowState *state)
{
    char title[MAX_PATH + 16];
    const char *path = state->buffers[state->active_buffer]->path;
    wsprintfA(title, "parapad - %s", path[0] ? path : "Untitled");
    SetWindowTextA(window, title);
}

static const char *buffer_filename(const TextBuffer *buffer)
{
    const char *name = buffer->path;
    for (const char *position = buffer->path; *position; ++position)
        if (*position == '\\' || *position == '/')
            name = position + 1;
    return *name ? name : *buffer->path ? buffer->path : "Untitled";
}

static void configure_document_syntax(TextBuffer *buffer)
{
    EditorDirectory directory;
    editor_core_send(buffer->core, EDITOR_GET_DIRECTORY, &directory);
    editor_core_send(buffer->core, EDITOR_SELECT_LEXER, directory.active ? "" : buffer->path);
}

static int directory_full_path(const char *current, const char *name, char path[MAX_PATH])
{
    if (!name || !*name)
        return 0;
    char joined[MAX_PATH];
    const char *input = name;
    if (current && name[0] != '\\' && name[0] != '/' && name[1] != ':') {
        size_t directory_length = strlen(current);
        size_t name_length = strlen(name);
        size_t separator = directory_length && current[directory_length - 1] != '\\' &&
            current[directory_length - 1] != '/';
        if (directory_length + separator + name_length >= MAX_PATH)
            return 0;
        memcpy(joined, current, directory_length);
        if (separator)
            joined[directory_length++] = '\\';
        memcpy(joined + directory_length, name, name_length + 1);
        input = joined;
    }
    DWORD copied = GetFullPathNameA(input, MAX_PATH, path, NULL);
    return copied && copied < MAX_PATH;
}

static int change_directory(void *context, const char *current, const char *requested,
    char **resolved)
{
    (void)context;
    char path[MAX_PATH];
    *resolved = NULL;
    if (!directory_full_path(current, requested, path))
        return 0;
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    size_t length = strlen(path);
    while (length > 3 && (path[length - 1] == '\\' || path[length - 1] == '/'))
        path[--length] = 0;
    char *copy = malloc(length + 1);
    if (!copy)
        return 0;
    memcpy(copy, path, length + 1);
    *resolved = copy;
    return 1;
}

static char *directory_display_name(const char *name)
{
    WCHAR wide[MAX_PATH];
    int count = MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH);
    if (!count)
        return NULL;
    char *text = malloc((size_t)count * 4);
    if (!text)
        return NULL;
    size_t length = 0;
    for (int index = 0; index + 1 < count; ++index) {
        unsigned long value = wide[index];
        if (value >= 0xD800 && value <= 0xDBFF && index + 2 < count &&
            wide[index + 1] >= 0xDC00 && wide[index + 1] <= 0xDFFF) {
            value = 0x10000 + ((value - 0xD800) << 10) + (wide[++index] - 0xDC00);
        } else if (value >= 0xD800 && value <= 0xDFFF) {
            value = 0xFFFD;
        }
        if (value < 0x80) {
            text[length++] = (char)value;
        } else if (value < 0x800) {
            text[length++] = (char)(0xC0 | (value >> 6));
            text[length++] = (char)(0x80 | (value & 0x3F));
        } else if (value < 0x10000) {
            text[length++] = (char)(0xE0 | (value >> 12));
            text[length++] = (char)(0x80 | ((value >> 6) & 0x3F));
            text[length++] = (char)(0x80 | (value & 0x3F));
        } else {
            text[length++] = (char)(0xF0 | (value >> 18));
            text[length++] = (char)(0x80 | ((value >> 12) & 0x3F));
            text[length++] = (char)(0x80 | ((value >> 6) & 0x3F));
            text[length++] = (char)(0x80 | (value & 0x3F));
        }
    }
    text[length] = 0;
    return text;
}

static int list_directory(void *context, const char *path, EditorDirectoryEmit emit,
    void *emit_context)
{
    (void)context;
    char pattern[MAX_PATH];
    if (!directory_full_path(path, "*", pattern))
        return 0;
    char *parent = NULL;
    if (change_directory(NULL, path, "..", &parent) && lstrcmpiA(path, parent)) {
        EditorDirectoryEntry entry;
        entry.name = "..";
        entry.path = parent;
        entry.is_directory = 1;
        if (!emit(emit_context, &entry)) {
            free(parent);
            return 0;
        }
    }
    free(parent);
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    int success = 1;
    do {
        if (!lstrcmpA(found.cFileName, ".") || !lstrcmpA(found.cFileName, ".."))
            continue;
        char entry_path[MAX_PATH];
        char *name = directory_display_name(found.cFileName);
        if (!name || !directory_full_path(path, found.cFileName, entry_path)) {
            free(name);
            success = 0;
            break;
        }
        EditorDirectoryEntry entry;
        entry.name = name;
        entry.path = entry_path;
        entry.is_directory =
                (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        success = emit(emit_context, &entry);
        free(name);
        if (!success)
            break;
    } while (FindNextFileA(search, &found));
    if (success && GetLastError() != ERROR_NO_MORE_FILES)
        success = 0;
    FindClose(search);
    return success;
}

static FILE *create_temporary_file(void *context)
{
    (void)context;
    char directory[MAX_PATH];
    DWORD length = GetTempPathA(MAX_PATH, directory);
    if (!length || length >= MAX_PATH - 64)
        return NULL;
    static LONG sequence;
    for (unsigned int attempt = 0; attempt < 256; ++attempt) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sparapad-%08lX-%08lX-%08lX.tmp", directory,
            (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(),
            (unsigned long)InterlockedIncrement(&sequence));
        HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                continue;
            return tmpfile();
        }
        int descriptor = _open_osfhandle((intptr_t)file, _O_BINARY | _O_RDWR);
        if (descriptor == -1) {
            CloseHandle(file);
            return tmpfile();
        }
        FILE *stream = _fdopen(descriptor, "w+b");
        if (!stream) {
            _close(descriptor);
            return tmpfile();
        }
        return stream;
    }
    return tmpfile();
}

static int map_file_readonly(void *context, FILE *file, size_t length,
    const unsigned char **data, void **mapping)
{
    (void)context;
    *data = NULL;
    *mapping = NULL;
    if (!length)
        return 0;
    intptr_t descriptor = _get_osfhandle(_fileno(file));
    if (descriptor == -1)
        return 0;
    HANDLE section = CreateFileMappingA((HANDLE)descriptor, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!section)
        return 0;
    const unsigned char *view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, length);
    if (!view) {
        CloseHandle(section);
        return 0;
    }
    *data = view;
    *mapping = section;
    return 1;
}

static void unmap_file(void *context, const unsigned char *data, void *mapping)
{
    (void)context;
    if (data)
        UnmapViewOfFile(data);
    if (mapping)
        CloseHandle((HANDLE)mapping);
}

static TextBuffer *create_document(const char *path)
{
    if (path && strlen(path) >= MAX_PATH)
        return NULL;
    TextBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        return NULL;
    buffer->core = editor_core_create("");
    EditorFileFunctions files = { NULL, create_temporary_file, map_file_readonly, unmap_file };
    EditorDirectoryFunctions functions = { NULL, list_directory, change_directory };
    DWORD attributes = path ? GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;
    EditorMessage load = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
        ? EDITOR_OPEN_DIRECTORY : EDITOR_LOAD_FILE;
    if (!buffer->core || !editor_core_send(buffer->core, EDITOR_REGISTER_FILE_FUNCTIONS, &files) ||
        !editor_core_send(buffer->core, EDITOR_REGISTER_DIRECTORY_FUNCTIONS, &functions) ||
        (path && !editor_core_send(buffer->core, load, (void *)path))) {
        editor_core_destroy(buffer->core);
        free(buffer);
        return NULL;
    }
    if (path)
        lstrcpynA(buffer->path, path, MAX_PATH);
    configure_document_syntax(buffer);
    return buffer;
}

static void layout_main_children(HWND window);

static void synchronize_directory_path(HWND window, MainWindowState *state)
{
    EditorDirectory directory;
    if (!editor_core_send(state->core, EDITOR_TAKE_DIRECTORY_CHANGE, &directory) || !directory.path)
        return;
    TextBuffer *buffer = state->buffers[state->active_buffer];
    lstrcpynA(buffer->path, directory.path, MAX_PATH);
    buffer->first_line = 0;
    buffer->left_pixel = 0;
    configure_document_syntax(buffer);
    HWND list = GetDlgItem(window, ID_BUFFER_LIST);
    LRESULT item = SendMessageA(list, LB_INSERTSTRING, (WPARAM)state->active_buffer,
        (LPARAM)buffer_filename(buffer));
    if (item != LB_ERR && item != LB_ERRSPACE)
        SendMessageA(list, LB_DELETESTRING, (WPARAM)(state->active_buffer + 1), 0);
    SendMessageA(list, LB_SETCURSEL, (WPARAM)state->active_buffer, 0);
    InvalidateRect(list, NULL, FALSE);
    update_document_title(window, state);
    HWND editor = active_editor(window);
    EditorControl *control = (EditorControl *)GetWindowLongPtrA(editor, GWLP_USERDATA);
    if (control) {
        control->first_line = 0;
        control->left_pixel = 0;
        control->pending_lead = 0;
        control->bitmap_valid = 0;
    }
}

static void update_buffer_status(HWND window, EditorCore *core)
{
    HWND view = active_editor(window);
    EditorControl *control = view ? (EditorControl *)GetWindowLongPtrA(view, GWLP_USERDATA) : NULL;
    if (control && control->hex_mode && control->core == core) {
        EditorBuffer buffer;
        if (hex_buffer(control, &buffer)) {
            char text[192];
            snprintf(text, sizeof(text), "OFFSET %08lX    %zu / %zu bytes    %s",
                (unsigned long)control->hex_offset, control->hex_offset, buffer.length,
                control->hex_ascii ? "ASCII" : "HEX");
            HWND status = GetDlgItem(window, ID_STATUS);
            SendMessageA(status, SB_SETTEXTA, 1, (LPARAM)text);
            SendMessageA(status, SB_SETTEXTA, 2, (LPARAM)"BYTES");
            return;
        }
    }
    EditorLineEnding line_ending = EDITOR_LINE_ENDING_LF;
    editor_core_send(core, EDITOR_GET_LINE_ENDING, &line_ending);
    EditorPosition position = { 0 };
    position.line_count = 1;
    editor_core_send(core, EDITOR_GET_POSITION, &position);
    unsigned int percent = position.length
        ? (unsigned int)(100.0 * ((double)position.offset / (double)position.length)) : 100;
    char text[192];
    snprintf(text, sizeof(text), "LN %zu/%zu%s, Col %zu    %u%% (%zu / %zu)",
        position.line + 1, position.line_count, position.line_count_complete ? "" : "+", position.column + 1, percent,
        position.offset, position.length);
    HWND status = GetDlgItem(window, ID_STATUS);
    SendMessageA(status, SB_SETTEXTA, 1, (LPARAM)text);
    SendMessageA(status, SB_SETTEXTA, 2,
        (LPARAM)(line_ending == EDITOR_LINE_ENDING_CRLF ? "CRLF" : "LF"));
}

static void activate_document(HWND window, MainWindowState *state, size_t index)
{
    if (index >= state->buffer_count)
        return;
    HWND editor = active_editor(window);
    EditorControl *control = (EditorControl *)GetWindowLongPtrA(editor, GWLP_USERDATA);
    if (control && state->core) {
        TextBuffer *previous = state->buffers[state->active_buffer];
        previous->first_line = control->first_line;
        previous->left_pixel = control->left_pixel;
    }
    if (state->command_active) {
        unsigned long escape = 27;
        editor_core_send(state->core, EDITOR_HANDLE_KEY, &escape);
        state->command_active = 0;
        HWND command = GetDlgItem(window, ID_COMMAND_LINE);
        ShowWindow(command, SW_HIDE);
        EnableWindow(command, FALSE);
        SetWindowTextA(command, "");
    }
    state->active_buffer = index;
    state->window_pending = 0;
    TextBuffer *buffer = state->buffers[index];
    state->pane_buffers[state->active_pane] = buffer;
    editor_core_send(buffer->core, EDITOR_GET_CARET, &state->pane_carets[state->active_pane]);
    state->core = buffer->core;
    update_buffer_status(window, state->core);
    SendMessageA(GetDlgItem(window, ID_BUFFER_LIST), LB_SETCURSEL, (WPARAM)index, 0);
    InvalidateRect(GetDlgItem(window, ID_BUFFER_LIST), NULL, FALSE);
    update_document_title(window, state);
    if (control) {
        control->core = buffer->core;
        control->syntax = &buffer->syntax;
        control->first_line = buffer->first_line;
        control->left_pixel = buffer->left_pixel;
        control->pending_lead = 0;
        control->bitmap_valid = 0;
        layout_main_children(window);
        SendMessageA(editor, WM_EDITOR_REFRESH, 0, 0);
        SetFocus(editor);
    }
}

static int add_document(HWND window, MainWindowState *state, TextBuffer *buffer)
{
    if (state->buffer_count >= INT_MAX ||
        state->buffer_count >= (size_t)-1 / sizeof(*state->buffers) - 1)
        return 0;
    TextBuffer **buffers = realloc(state->buffers,
        (state->buffer_count + 1) * sizeof(*buffers));
    if (!buffers)
        return 0;
    state->buffers = buffers;
    LRESULT item = SendMessageA(GetDlgItem(window, ID_BUFFER_LIST), LB_ADDSTRING,
        0, (LPARAM)buffer_filename(buffer));
    if (item == LB_ERR || item == LB_ERRSPACE)
        return 0;
    state->buffers[state->buffer_count++] = buffer;
    activate_document(window, state, state->buffer_count - 1);
    return 1;
}

static int open_document(HWND window, MainWindowState *state, const char *path)
{
    TextBuffer *buffer = create_document(path);
    if (buffer && add_document(window, state, buffer))
        return 1;
    if (buffer) {
        editor_core_destroy(buffer->core);
        free(buffer);
    }
    MessageBoxA(window, "The document could not be opened. Check the path, access permissions, and available memory.",
        "parapad", MB_OK | MB_ICONERROR);
    return 0;
}

static int choose_file(HWND window, char *path, int save)
{
    OPENFILENAMEA dialog = { 0 };
    dialog.lStructSize = sizeof(OPENFILENAMEA);// OPENFILENAME_SIZE_VERSION_400A;
    dialog.hwndOwner = window;
    dialog.lpstrFilter = "All Files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
        OFN_HIDEREADONLY | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL result = save ? GetSaveFileNameA(&dialog) : GetOpenFileNameA(&dialog);
    if (!result && CommDlgExtendedError())
        MessageBoxA(window, "The file dialog could not be opened.", "parapad", MB_OK | MB_ICONERROR);
    return result != FALSE;
}

static int write_document(HWND window, MainWindowState *state, const char *path)
{
    TextBuffer *buffer = state->buffers[state->active_buffer];
    if (!editor_core_send(state->core, EDITOR_SAVE_FILE, (void *)path)) {
        MessageBoxA(window, "The file could not be saved. Check the path, permissions, and available disk space.",
            "parapad", MB_OK | MB_ICONERROR);
        return 0;
    }
    lstrcpynA(buffer->path, path, MAX_PATH);
    configure_document_syntax(buffer);
    HWND editor = active_editor(window);
    EditorControl *control = (EditorControl *)GetWindowLongPtrA(editor, GWLP_USERDATA);
    if (control) {
        control->syntax = &buffer->syntax;
        control->bitmap_valid = 0;
        InvalidateRect(editor, NULL, FALSE);
    }
    if (state->second_editor && state->pane_buffers[0] == state->pane_buffers[1]) {
        HWND other = state->active_pane ? GetDlgItem(window, ID_EDITOR) : state->second_editor;
        EditorControl *other_control = (EditorControl *)GetWindowLongPtrA(other, GWLP_USERDATA);
        other_control->bitmap_valid = 0;
        InvalidateRect(other, NULL, FALSE);
    }
    HWND list = GetDlgItem(window, ID_BUFFER_LIST);
    LRESULT item = SendMessageA(list, LB_INSERTSTRING, (WPARAM)state->active_buffer,
        (LPARAM)buffer_filename(buffer));
    if (item != LB_ERR && item != LB_ERRSPACE)
        SendMessageA(list, LB_DELETESTRING, (WPARAM)(state->active_buffer + 1), 0);
    SendMessageA(list, LB_SETCURSEL, (WPARAM)state->active_buffer, 0);
    InvalidateRect(list, NULL, FALSE);
    update_document_title(window, state);
    return 1;
}

static int save_document(HWND window, MainWindowState *state, int save_as)
{
    EditorDirectory directory;
    editor_core_send(state->core, EDITOR_GET_DIRECTORY, &directory);
    if (directory.active) {
        MessageBoxA(window, "Directory buffers cannot be saved.", "parapad", MB_OK | MB_ICONWARNING);
        return 0;
    }
    char path[MAX_PATH];
    lstrcpynA(path, state->buffers[state->active_buffer]->path, MAX_PATH);
    if ((save_as || !path[0]) && !choose_file(window, path, 1))
        return 0;
    return write_document(window, state, path);
}

static int confirm_document_close(HWND window, MainWindowState *state)
{
    int modified = 0;
    editor_core_send(state->core, EDITOR_GET_MODIFIED, &modified);
    if (!modified)
        return 1;
    char prompt[MAX_PATH + 64];
    wsprintfA(prompt, "Save changes to %s before closing?", buffer_filename(state->buffers[state->active_buffer]));
    int answer = MessageBoxA(window, prompt, "parapad",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL)
        return 0;
    return answer != IDYES || save_document(window, state, 0);
}

static int remove_document(HWND window, MainWindowState *state, int quit_last)
{
    if (quit_last && state->buffer_count == 1) {
        DestroyWindow(window);
        return 1;
    }
    size_t closing = state->active_buffer;
    if (state->buffer_count == 1) {
        if (!open_document(window, state, NULL))
            return 0;
    } else {
        activate_document(window, state, closing + 1 < state->buffer_count ? closing + 1 : closing - 1);
    }
    TextBuffer *buffer = state->buffers[closing];
    for (int pane = 0; pane < 2; ++pane) {
        if (state->pane_buffers[pane] != buffer)
            continue;
        TextBuffer *replacement = state->buffers[state->active_buffer];
        state->pane_buffers[pane] = replacement;
        HWND view = pane ? state->second_editor : GetDlgItem(window, ID_EDITOR);
        EditorControl *control = view ? (EditorControl *)GetWindowLongPtrA(view, GWLP_USERDATA) : NULL;
        if (control) {
            control->core = replacement->core;
            control->syntax = &replacement->syntax;
            control->first_line = replacement->first_line;
            control->left_pixel = replacement->left_pixel;
            control->bitmap_valid = 0;
            InvalidateRect(view, NULL, FALSE);
        }
        editor_core_send(replacement->core, EDITOR_GET_CARET, &state->pane_carets[pane]);
    }
    SendMessageA(GetDlgItem(window, ID_BUFFER_LIST), LB_DELETESTRING, (WPARAM)closing, 0);
    memmove(state->buffers + closing, state->buffers + closing + 1,
        (state->buffer_count - closing - 1) * sizeof(*state->buffers));
    --state->buffer_count;
    if (state->active_buffer > closing)
        --state->active_buffer;
    editor_core_destroy(buffer->core);
    clear_syntax_cache(&buffer->syntax);
    free(buffer);
    SendMessageA(GetDlgItem(window, ID_BUFFER_LIST), LB_SETCURSEL, (WPARAM)state->active_buffer, 0);
    InvalidateRect(GetDlgItem(window, ID_BUFFER_LIST), NULL, FALSE);
    return 1;
}

static void close_document(HWND window, MainWindowState *state)
{
    if (confirm_document_close(window, state))
        remove_document(window, state, 0);
}

static void layout_main_children(HWND window)
{
    MainWindowState *state = (MainWindowState *)GetWindowLongPtrA(window, GWLP_USERDATA);
    HWND editor = active_editor(window);
    if (!state || !editor)
        return;
    RECT rect;
    GetClientRect(window, &rect);
    HWND status = GetDlgItem(window, ID_STATUS);
    SendMessageA(status, WM_SIZE, 0, 0);
    RECT status_client;
    GetClientRect(status, &status_client);
    int grip_width = GetSystemMetrics(SM_CXVSCROLL) + 4;
    int text_right = status_client.right > grip_width ? status_client.right - grip_width : 0;
    int status_parts[3];
    status_parts[0] = text_right > 544 ? text_right - 544 : 0;
    status_parts[1] = text_right > 64 ? text_right - 64 : 0;
    status_parts[2] = text_right;
    SendMessageA(status, SB_SETPARTS, 3, (LPARAM)status_parts);
    update_buffer_status(window, state->core);
    RECT status_rect = { 0 };
    GetWindowRect(status, &status_rect);
    int available_height = rect.bottom - (status_rect.bottom - status_rect.top);
    if (available_height < 0)
        available_height = 0;
    EditorControl *control = (EditorControl *)GetWindowLongPtrA(editor, GWLP_USERDATA);
    int command_height = state->command_active ? (control ? control->line_height + 8 : 28) : 0;
    if (command_height > available_height)
        command_height = available_height;
    int content_height = available_height - command_height;
    int sidebar_width = rect.right >= 600 ? 180 : rect.right / 3;
    int editor_left = sidebar_width + 12;
    int width = rect.right > editor_left + 12 ? rect.right - editor_left - 12 : 0;
    int height = content_height > 24 ? content_height - 24 : 0;
    MoveWindow(GetDlgItem(window, ID_BUFFER_LIST), 0, 0, sidebar_width, available_height, TRUE);
    HWND first_editor = GetDlgItem(window, ID_EDITOR);
    if (state->second_editor) {
        int gap = state->split_vertical ? (width < 4 ? width : 4) : (height < 4 ? height : 4);
        int first = state->split_vertical ? (width - gap) / 2 : (height - gap) / 2;
        if (state->split_vertical) {
            MoveWindow(first_editor, editor_left, 12, first, height, TRUE);
            MoveWindow(state->second_editor, editor_left + first + gap, 12,
                width - first - gap, height, TRUE);
        } else {
            MoveWindow(first_editor, editor_left, 12, width, first, TRUE);
            MoveWindow(state->second_editor, editor_left, 12 + first + gap,
                width, height - first - gap, TRUE);
        }
    } else {
        MoveWindow(first_editor, editor_left, 12, width, height, TRUE);
    }
    MoveWindow(GetDlgItem(window, ID_COMMAND_LINE), editor_left, content_height, width, command_height, TRUE);
}

static void cancel_window_command(HWND window, MainWindowState *state)
{
    if (!state->command_active)
        return;
    unsigned long escape = 27;
    editor_core_send(state->core, EDITOR_HANDLE_KEY, &escape);
    state->command_active = 0;
    HWND command = GetDlgItem(window, ID_COMMAND_LINE);
    ShowWindow(command, SW_HIDE);
    EnableWindow(command, FALSE);
    SetWindowTextA(command, "");
}

static int set_editor_view(HWND window, MainWindowState *state, int hex)
{
    HWND view = active_editor(window);
    EditorControl *editor = (EditorControl *)GetWindowLongPtrA(view, GWLP_USERDATA);
    if (!editor || editor->hex_mode == hex)
        return 1;
    EditorDirectory directory;
    editor_core_send(editor->core, EDITOR_GET_DIRECTORY, &directory);
    if (hex && directory.active)
        return 0;
    EditorBuffer buffer;
    if (!hex_buffer(editor, &buffer) || buffer.length > 0xFFFFFFFFUL)
        return 0;
    cancel_window_command(window, state);
    state->window_pending = 0;
    if (hex) {
        EditorPosition position;
        editor_core_send(editor->core, EDITOR_GET_POSITION, &position);
        unsigned long escape = 27;
        editor_core_send(editor->core, EDITOR_HANDLE_KEY, &escape);
        editor->hex_offset = position.offset;
        editor->hex_first = editor->hex_offset / 16;
        editor->hex_low = 0;
        editor->hex_left = 0;
    } else {
        if (!editor_core_send(editor->core, EDITOR_SET_OFFSET, &editor->hex_offset))
            return 0;
    }
    editor->hex_mode = hex;
    editor->hex_editing = 0;
    editor->hex_command[0] = 0;
    ShowScrollBar(view, SB_HORZ, hex ? TRUE : FALSE);
    editor->pending_lead = 0;
    editor->bitmap_valid = 0;
    SetFocus(view);
    layout_main_children(window);
    update_editor_caret(view, editor);
    return 1;
}

static void select_pane(HWND window, MainWindowState *state, int pane)
{
    if (pane == state->active_pane || (pane && !state->second_editor))
        return;
    editor_core_send(state->core, EDITOR_GET_CARET, &state->pane_carets[state->active_pane]);
    cancel_window_command(window, state);
    state->active_pane = pane;
    state->window_pending = 0;
    TextBuffer *buffer = state->pane_buffers[pane];
    state->core = buffer->core;
    for (size_t index = 0; index < state->buffer_count; ++index) {
        if (state->buffers[index] == buffer) {
            state->active_buffer = index;
            break;
        }
    }
    editor_core_send(state->core, EDITOR_SET_CARET, &state->pane_carets[pane]);
    HWND editor = active_editor(window);
    EditorControl *control = (EditorControl *)GetWindowLongPtrA(editor, GWLP_USERDATA);
    SendMessageA(GetDlgItem(window, ID_COMMAND_LINE), WM_SETFONT, (WPARAM)control->font, TRUE);
    SendMessageA(GetDlgItem(window, ID_BUFFER_LIST), LB_SETCURSEL, (WPARAM)state->active_buffer, 0);
    InvalidateRect(GetDlgItem(window, ID_BUFFER_LIST), NULL, FALSE);
    update_document_title(window, state);
    layout_main_children(window);
}

static int split_document(HWND window, MainWindowState *state, int vertical, const char *path)
{
    if (state->second_editor)
        return 0;
    TextBuffer *target = NULL;
    size_t target_index = 0;
    int owned = 0;
    if (path) {
        for (size_t index = 0; index < state->buffer_count; ++index) {
            if (!lstrcmpiA(state->buffers[index]->path, path)) {
                target = state->buffers[index];
                target_index = index;
                break;
            }
        }
        if (!target) {
            DWORD attributes = GetFileAttributesA(path);
            int new_file = attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
            target = create_document(new_file ? NULL : path);
            if (!target)
                return 0;
            if (new_file) {
                lstrcpynA(target->path, path, MAX_PATH);
                configure_document_syntax(target);
            }
            owned = 1;
        }
    }
    HWND first_editor = GetDlgItem(window, ID_EDITOR);
    EditorControl *first = (EditorControl *)GetWindowLongPtrA(first_editor, GWLP_USERDATA);
    HWND second = CreateWindowExA(WS_EX_CLIENTEDGE, "PARAPAD_EDITOR", NULL,
        WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_SECOND_EDITOR,
        (HINSTANCE)GetWindowLongPtrA(window, GWLP_HINSTANCE), state->core);
    if (!second) {
        if (owned) {
            editor_core_destroy(target->core);
            clear_syntax_cache(&target->syntax);
            free(target);
        }
        return 0;
    }
    EditorControl *other = (EditorControl *)GetWindowLongPtrA(second, GWLP_USERDATA);
    if (other->font_points != first->font_points)
        set_editor_font(second, other, first->font_points);
    other->syntax = first->syntax;
    other->first_line = first->first_line;
    other->left_pixel = first->left_pixel;
    state->second_editor = second;
    state->split_vertical = vertical;
    state->pane_buffers[1] = state->pane_buffers[0];
    editor_core_send(state->core, EDITOR_GET_CARET, &state->pane_carets[0]);
    state->pane_carets[1] = state->pane_carets[0];
    if (target) {
        if (owned && !add_document(window, state, target)) {
            state->second_editor = NULL;
            state->pane_buffers[1] = NULL;
            DestroyWindow(second);
            editor_core_destroy(target->core);
            clear_syntax_cache(&target->syntax);
            free(target);
            SendMessageA(GetDlgItem(window, ID_COMMAND_LINE), WM_SETFONT, (WPARAM)first->font, TRUE);
            layout_main_children(window);
            return 0;
        }
        if (!owned)
            activate_document(window, state, target_index);
    }
    cancel_window_command(window, state);
    ShowWindow(second, SW_SHOW);
    layout_main_children(window);
    SendMessageA(GetDlgItem(window, ID_COMMAND_LINE), WM_SETFONT, (WPARAM)first->font, TRUE);
    SetFocus(first_editor);
    SendMessageA(first_editor, WM_EDITOR_REFRESH, 0, 0);
    return 1;
}

static int retain_pane(HWND window, MainWindowState *state, int pane)
{
    if (!state->second_editor)
        return 0;
    EditorCaret caret = state->pane_carets[pane];
    if (pane == state->active_pane)
        editor_core_send(state->core, EDITOR_GET_CARET, &caret);
    cancel_window_command(window, state);
    HWND first_editor = GetDlgItem(window, ID_EDITOR);
    HWND second = state->second_editor;
    EditorControl *first = (EditorControl *)GetWindowLongPtrA(first_editor, GWLP_USERDATA);
    EditorControl *other = (EditorControl *)GetWindowLongPtrA(second, GWLP_USERDATA);
    TextBuffer *buffer = state->pane_buffers[pane];
    if (pane) {
        first->first_line = other->first_line;
        first->left_pixel = other->left_pixel;
        if (first->font_points != other->font_points)
            set_editor_font(first_editor, first, other->font_points);
    }
    first->core = buffer->core;
    first->syntax = &buffer->syntax;
    first->pending_lead = 0;
    first->bitmap_valid = 0;
    state->second_editor = NULL;
    state->active_pane = 0;
    state->pane_buffers[0] = buffer;
    state->pane_buffers[1] = NULL;
    state->pane_carets[0] = caret;
    state->core = buffer->core;
    for (size_t index = 0; index < state->buffer_count; ++index) {
        if (state->buffers[index] == buffer) {
            state->active_buffer = index;
            break;
        }
    }
    SendMessageA(GetDlgItem(window, ID_COMMAND_LINE), WM_SETFONT, (WPARAM)first->font, TRUE);
    DestroyWindow(second);
    editor_core_send(state->core, EDITOR_SET_CARET, &caret);
    SendMessageA(GetDlgItem(window, ID_BUFFER_LIST), LB_SETCURSEL, (WPARAM)state->active_buffer, 0);
    InvalidateRect(GetDlgItem(window, ID_BUFFER_LIST), NULL, FALSE);
    update_document_title(window, state);
    layout_main_children(window);
    SetFocus(first_editor);
    SendMessageA(first_editor, WM_EDITOR_REFRESH, 0, 0);
    return 1;
}

static int swap_panes(HWND window, MainWindowState *state)
{
    if (!state->second_editor)
        return 0;
    HWND first = GetDlgItem(window, ID_EDITOR);
    HWND second = state->second_editor;
    HWND focused = active_editor(window);
    if (!SetWindowLongPtrA(first, GWLP_ID, ID_SECOND_EDITOR))
        return 0;
    if (!SetWindowLongPtrA(second, GWLP_ID, ID_EDITOR)) {
        SetWindowLongPtrA(first, GWLP_ID, ID_EDITOR);
        return 0;
    }
    editor_core_send(state->core, EDITOR_GET_CARET, &state->pane_carets[state->active_pane]);
    cancel_window_command(window, state);
    TextBuffer *buffer = state->pane_buffers[0];
    state->pane_buffers[0] = state->pane_buffers[1];
    state->pane_buffers[1] = buffer;
    EditorCaret caret = state->pane_carets[0];
    state->pane_carets[0] = state->pane_carets[1];
    state->pane_carets[1] = caret;
    state->second_editor = first;
    state->active_pane = 1 - state->active_pane;
    state->window_pending = 0;
    layout_main_children(window);
    SetFocus(focused);
    SendMessageA(focused, WM_EDITOR_REFRESH, 0, 0);
    EditorControl *first_control = (EditorControl *)GetWindowLongPtrA(first, GWLP_USERDATA);
    EditorControl *second_control = (EditorControl *)GetWindowLongPtrA(second, GWLP_USERDATA);
    first_control->bitmap_valid = 0;
    second_control->bitmap_valid = 0;
    RedrawWindow(first, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    RedrawWindow(second, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    return 1;
}

static int execute_window_key(HWND window, MainWindowState *state, unsigned long key)
{
    if (key == 'x' || key == 24)
        return swap_panes(window, state);
    if (key == 's' || key == 19)
        return split_document(window, state, 0, NULL);
    if (key == 'v' || key == 22)
        return split_document(window, state, 1, NULL);
    if (key == 'c' || key == 3)
        return retain_pane(window, state, 1 - state->active_pane);
    if (key == 'o' || key == 15)
        return !state->second_editor || retain_pane(window, state, state->active_pane);
    int pane = state->active_pane;
    if (key == 'w' || key == 'W' || key == 23)
        pane = 1 - pane;
    else if (key == 'h' || key == 8 || key == EDITOR_KEY_LEFT)
        pane = state->split_vertical ? 0 : pane;
    else if (key == 'l' || key == 12 || key == EDITOR_KEY_RIGHT)
        pane = state->split_vertical ? 1 : pane;
    else if (key == 'k' || key == 11 || key == EDITOR_KEY_UP)
        pane = state->split_vertical ? pane : 0;
    else if (key == 'j' || key == 10 || key == EDITOR_KEY_DOWN)
        pane = state->split_vertical ? pane : 1;
    else if (key != 27)
        return 0;
    if (state->second_editor) {
        cancel_window_command(window, state);
        SetFocus(pane ? state->second_editor : GetDlgItem(window, ID_EDITOR));
    }
    return 1;
}

static int execute_ansi_command(EditorCore *core, const char *text)
{
    WCHAR units[1024];
    int count = MultiByteToWideChar(CP_ACP, 0, text, -1, units, 1024);
    if (!count)
        return editor_core_send(core, EDITOR_EXECUTE_COMMAND, NULL);
    char utf8[4096];
    size_t length = 0;
    for (int index = 0; index < count - 1; ++index) {
        unsigned long value = units[index];
        if (value >= 0xD800 && value <= 0xDBFF && index + 1 < count - 1 &&
            units[index + 1] >= 0xDC00 && units[index + 1] <= 0xDFFF) {
            value = 0x10000 + ((value - 0xD800) << 10) + units[++index] - 0xDC00;
        } else if (value >= 0xD800 && value <= 0xDFFF) {
            return editor_core_send(core, EDITOR_EXECUTE_COMMAND, NULL);
        }
        if (value < 0x80) {
            utf8[length++] = (char)value;
        } else if (value < 0x800) {
            utf8[length++] = (char)(0xC0 | (value >> 6));
            utf8[length++] = (char)(0x80 | (value & 0x3F));
        } else if (value < 0x10000) {
            utf8[length++] = (char)(0xE0 | (value >> 12));
            utf8[length++] = (char)(0x80 | ((value >> 6) & 0x3F));
            utf8[length++] = (char)(0x80 | (value & 0x3F));
        } else {
            utf8[length++] = (char)(0xF0 | (value >> 18));
            utf8[length++] = (char)(0x80 | ((value >> 12) & 0x3F));
            utf8[length++] = (char)(0x80 | ((value >> 6) & 0x3F));
            utf8[length++] = (char)(0x80 | (value & 0x3F));
        }
    }
    utf8[length] = 0;
    return editor_core_send(core, EDITOR_EXECUTE_COMMAND, utf8);
}

static int window_command_error(MainWindowState *state, const char *message)
{
    state->command_error = message;
    return 0;
}

static int execute_window_command(HWND window, MainWindowState *state, const char *text)
{
    state->command_error = NULL;
    char prefix;
    editor_core_send(state->core, EDITOR_GET_COMMAND_PREFIX, &prefix);
    if (prefix != ':')
        return execute_ansi_command(state->core, text);
    const char *position = text;
    while (*position == ' ' || *position == '\t')
        ++position;
    if (*position == ':')
        ++position;
    while (*position == ' ' || *position == '\t')
        ++position;
    const char *start = position;
    while (*position && *position != ' ' && *position != '\t')
        ++position;
    size_t length = (size_t)(position - start);
    char name[8];
    if (!length || length >= sizeof(name))
        return execute_ansi_command(state->core, text);
    memcpy(name, start, length);
    name[length] = 0;
    int horizontal = length >= 2 && length <= 5 && !strncmp(name, "split", length);
    int vertical = length >= 2 && length <= 6 && !strncmp(name, "vsplit", length);
    size_t plain_length = length - (name[length - 1] == '!');
    int close = plain_length >= 3 && plain_length <= 5 && !strncmp(name, "close", plain_length);
    int only = plain_length >= 2 && plain_length <= 4 && !strncmp(name, "only", plain_length);
    int window_command = !strcmp(name, "wincmd");
    if (!horizontal && !vertical && !close && !only && !window_command &&
        strcmp(name, "tabe") && strcmp(name, "n") && strcmp(name, "w") &&
        strcmp(name, "q") && strcmp(name, "q!") && strcmp(name, "qa") && strcmp(name, "qa!"))
        return execute_ansi_command(state->core, text);
    while (*position == ' ' || *position == '\t')
        ++position;
    const char *end = position + strlen(position);
    while (end > position && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    int has_argument = end != position;
    if (window_command) {
        if (end - position != 1)
            return window_command_error(state, "wincmd requires one window command");
        if (!execute_window_key(window, state, (unsigned char)*position))
            return window_command_error(state, "window command failed (two panes maximum)");
        cancel_window_command(window, state);
        layout_main_children(window);
        SetFocus(active_editor(window));
        return 1;
    }
    char path[MAX_PATH] = "";
    if (has_argument) {
        if (!horizontal && !vertical && strcmp(name, "tabe") && strcmp(name, "w"))
            return window_command_error(state, "invalid command arguments");
        if (*position == '"') {
            if (end - position < 2 || end[-1] != '"')
                return window_command_error(state, "invalid file name");
            ++position;
            --end;
        }
        length = (size_t)(end - position);
        if (!length || length >= MAX_PATH)
            return window_command_error(state, "invalid file name");
        char argument[MAX_PATH];
        memcpy(argument, position, length);
        argument[length] = 0;
        EditorDirectory directory;
        editor_core_send(state->core, EDITOR_GET_DIRECTORY, &directory);
        if (!directory_full_path(directory.active ? directory.path : NULL, argument, path))
            return window_command_error(state, "invalid file name");
    }
    if (horizontal || vertical) {
        if (state->second_editor)
            return window_command_error(state, "two panes maximum; use :only first");
        if (!split_document(window, state, vertical, has_argument ? path : NULL))
            return window_command_error(state, "split could not be opened");
        return 1;
    }
    if (close || only) {
        if (!state->second_editor) {
            if (close)
                return window_command_error(state, "cannot close the last window");
            cancel_window_command(window, state);
            layout_main_children(window);
            SetFocus(active_editor(window));
            return 1;
        }
        return retain_pane(window, state, only ? state->active_pane : 1 - state->active_pane);
    }
    if (!strcmp(name, "tabe")) {
        if (has_argument && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND) {
            TextBuffer *buffer = create_document(NULL);
            if (!buffer)
                return window_command_error(state, "not enough memory");
            lstrcpynA(buffer->path, path, MAX_PATH);
            configure_document_syntax(buffer);
            if (add_document(window, state, buffer))
                return 1;
            editor_core_destroy(buffer->core);
            free(buffer);
            return window_command_error(state, "not enough memory");
        }
        if (!open_document(window, state, has_argument ? path : NULL))
            return window_command_error(state, "file could not be opened");
        return 1;
    }
    if (!strcmp(name, "n")) {
        activate_document(window, state, (state->active_buffer + 1) % state->buffer_count);
        return 1;
    }
    if (!strcmp(name, "w")) {
        EditorDirectory directory;
        editor_core_send(state->core, EDITOR_GET_DIRECTORY, &directory);
        if (directory.active)
            return window_command_error(state, "directory buffers cannot be saved");
        const char *current = state->buffers[state->active_buffer]->path;
        if (!has_argument) {
            if (!*current)
                return window_command_error(state, "no file name; use :w <file>");
            lstrcpynA(path, current, MAX_PATH);
        } else if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && lstrcmpiA(path, current)) {
            return window_command_error(state, "file already exists");
        }
        if (!write_document(window, state, path))
            return window_command_error(state, "file could not be saved");
        unsigned long escape = 27;
        return editor_core_send(state->core, EDITOR_HANDLE_KEY, &escape);
    }
    int all = name[1] == 'a';
    int force = name[strlen(name) - 1] == '!';
    if (!all && state->second_editor)
        return retain_pane(window, state, 1 - state->active_pane);
    if (!force) {
        size_t first = all ? 0 : state->active_buffer;
        size_t limit = all ? state->buffer_count : first + 1;
        for (size_t index = first; index < limit; ++index) {
            int modified = 0;
            editor_core_send(state->buffers[index]->core, EDITOR_GET_MODIFIED, &modified);
            if (modified)
                return window_command_error(state, all ?
                    "unsaved changes; use :qa! to discard" : "unsaved changes; use :q! to discard");
        }
    }
    if (all) {
        DestroyWindow(window);
        return 1;
    }
    return remove_document(window, state, 1);
}

static LRESULT CALLBACK command_line_wnd_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    HWND parent = GetParent(window);
    MainWindowState *state = (MainWindowState *)GetWindowLongPtrA(parent, GWLP_USERDATA);
    if (!state || !state->command_proc)
        return DefWindowProcA(window, message, wp, lp);
    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;
    if (message == WM_KEYDOWN) {
        if (wp == VK_RETURN || wp == VK_ESCAPE)
            return 0;
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageA(window, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    if (message == WM_CHAR && (wp == '\r' || wp == 27)) {
        if (wp == 27) {
            unsigned long key = 27;
            editor_core_send(state->core, EDITOR_HANDLE_KEY, &key);
        } else {
            char text[1024];
            int length = GetWindowTextLengthA(window);
            int success;
            if (length >= (int)sizeof(text)) {
                success = editor_core_send(state->core, EDITOR_EXECUTE_COMMAND, NULL);
            } else {
                GetWindowTextA(window, text, sizeof(text));
                success = execute_window_command(parent, state, text);
            }
            if (!IsWindow(parent))
                return 0;
            if (!success)
                MessageBeep(MB_ICONWARNING);
        }
        SendMessageA(active_editor(parent), WM_EDITOR_REFRESH, 0, 0);
        return 0;
    }
    return CallWindowProcA(state->command_proc, window, message, wp, lp);
}

LRESULT CALLBACK main_wnd_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (legacy_wheel_message && msg == legacy_wheel_message)
        return SendMessageA(active_editor(hWnd), msg, wp, lp);
    MainWindowState *state = (MainWindowState *)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        state = calloc(1, sizeof(*state));
        if (!state)
            return -1;
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)state);
        HMENU menu = CreateMenu();
        HMENU file_menu = CreatePopupMenu();
        if (!menu || !file_menu ||
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_NEW, "&New\tCtrl+N") ||
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_OPEN, "&Open...\tCtrl+O") ||
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_SAVE, "&Save\tCtrl+S") ||
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_SAVE_AS, "Save &As...") ||
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_CLOSE, "&Close\tCtrl+F4") ||
            !AppendMenuA(file_menu, MF_SEPARATOR, 0, NULL) ||
            !AppendMenu(file_menu, MF_STRING, ID_FILE_EXIT, "E&xit") ||
            !AppendMenu(menu, MF_POPUP, (UINT_PTR)file_menu, "&File")) {
            if (file_menu)
                DestroyMenu(file_menu);
            if (menu)
                DestroyMenu(menu);
            return -1;
        }
        HMENU view_menu = CreatePopupMenu();
        if (!view_menu ||
            !AppendMenuA(view_menu, MF_STRING | MF_CHECKED, ID_VIEW_TEXT, "&Text Mode") ||
            !AppendMenuA(view_menu, MF_STRING, ID_VIEW_HEX, "&Hex Mode") ||
            !AppendMenuA(menu, MF_POPUP, (UINT_PTR)view_menu, "&View")) {
            if (view_menu)
                DestroyMenu(view_menu);
            DestroyMenu(menu);
            return -1;
        }
        if (!SetMenu(hWnd, menu)) {
            DestroyMenu(menu);
            return -1;
        }

        HWND status = CreateWindowExA(0, STATUSCLASSNAMEA, NULL,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_STATUS,
            ((LPCREATESTRUCTA)lp)->hInstance, NULL);
        if (!status)
            return -1;
        int status_parts[3] = { 0, 0, -1 };
        SendMessageA(status, SB_SETPARTS, 3, (LPARAM)status_parts);
        SendMessageA(status, SB_SETTEXTA, 0, (LPARAM)"COMMAND");
        update_buffer_status(hWnd, state->core);

        HWND list = CreateWindowExA(0, "LISTBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
            LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_BUFFER_LIST,
            ((LPCREATESTRUCTA)lp)->hInstance, NULL);
        if (!list)
            return -1;
        SendMessageA(list, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), FALSE);
        if (!open_document(hWnd, state, NULL))
            return -1;

        HWND editor = CreateWindowExA(WS_EX_CLIENTEDGE, "PARAPAD_EDITOR", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_EDITOR,
            ((LPCREATESTRUCTA)lp)->hInstance, state->core);
        if (!editor)
            return -1;
        HWND command = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_DISABLED | ES_AUTOHSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_COMMAND_LINE,
            ((LPCREATESTRUCTA)lp)->hInstance, NULL);
        if (!command)
            return -1;
        EditorControl *control = (EditorControl *)GetWindowLongPtrA(editor, GWLP_USERDATA);
        control->syntax = &state->buffers[state->active_buffer]->syntax;
        SendMessageA(command, WM_SETFONT, (WPARAM)control->font, FALSE);
        SendMessageA(command, EM_LIMITTEXT, 1023, 0);
        state->command_proc = (WNDPROC)SetWindowLongPtrA(command, GWLP_WNDPROC, (LONG_PTR)command_line_wnd_proc);
        if (!state->command_proc)
            return -1;
        update_document_title(hWnd, state);
        return 0;
    }
    case WM_SIZE:
        layout_main_children(hWnd);
        return 0;
    case WM_INITMENUPOPUP: {
        HWND view = active_editor(hWnd);
        EditorControl *editor = view ? (EditorControl *)GetWindowLongPtrA(view, GWLP_USERDATA) : NULL;
        if (editor && (HMENU)wp == GetSubMenu(GetMenu(hWnd), 1)) {
            CheckMenuItem((HMENU)wp, ID_VIEW_HEX, editor->hex_mode ? MF_CHECKED : MF_UNCHECKED);
            CheckMenuItem((HMENU)wp, ID_VIEW_TEXT, editor->hex_mode ? MF_UNCHECKED : MF_CHECKED);
            EditorDirectory directory;
            editor_core_send(editor->core, EDITOR_GET_DIRECTORY, &directory);
            EnableMenuItem((HMENU)wp, ID_VIEW_HEX,
                MF_BYCOMMAND | (directory.active ? MF_GRAYED : MF_ENABLED));
        }
        return 0;
    }
    case WM_EDITOR_FOCUS:
        if (state)
            select_pane(hWnd, state, (HWND)wp == state->second_editor ? 1 : 0);
        return 0;
    case WM_EDITOR_WINDOW_KEY: {
        if (!state)
            return 0;
        EditorMode mode;
        editor_core_send(state->core, EDITOR_GET_MODE, &mode);
        if (!state->window_pending) {
            if (wp != 23 || mode != EDITOR_MODE_COMMAND)
                return 0;
            unsigned long escape = 27;
            editor_core_send(state->core, EDITOR_HANDLE_KEY, &escape);
            state->window_pending = 1;
            return 1;
        }
        state->window_pending = 0;
        if (mode != EDITOR_MODE_COMMAND)
            return 0;
        if (!execute_window_key(hWnd, state, (unsigned long)wp))
            MessageBeep(MB_ICONWARNING);
        return 1;
    }
    case WM_EDITOR_SWITCH_BUFFER: {
        const EditorBufferSwitch *request = (const EditorBufferSwitch *)lp;
        if (!state || !request || !request->count || !state->buffer_count)
            return 0;
        size_t index;
        if (request->absolute) {
            if (request->count > state->buffer_count)
                return 0;
            index = request->count - 1;
        } else {
            size_t offset = request->count % state->buffer_count;
            index = (state->active_buffer + (request->backwards ? state->buffer_count - offset : offset)) %
                state->buffer_count;
        }
        activate_document(hWnd, state, index);
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *item = (MEASUREITEMSTRUCT *)lp;
        if (item->CtlID != ID_BUFFER_LIST)
            break;
        HDC dc = GetDC(hWnd);
        HGDIOBJ font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICA metrics;
        GetTextMetricsA(dc, &metrics);
        item->itemHeight = metrics.tmHeight + 12;
        SelectObject(dc, font);
        ReleaseDC(hWnd, dc);
        return TRUE;
    }
    case WM_CTLCOLORLISTBOX:
        if ((HWND)lp == GetDlgItem(hWnd, ID_BUFFER_LIST)) {
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lp;
        if (item->CtlID != ID_BUFFER_LIST)
            break;
        FillRect(item->hDC, &item->rcItem, GetSysColorBrush(COLOR_BTNFACE));
        if (!state || item->itemID >= state->buffer_count)
            return TRUE;
        RECT bounds = item->rcItem;
        InflateRect(&bounds, -3, -2);
        if (item->itemID == state->active_buffer)
            FrameRect(item->hDC, &bounds, GetSysColorBrush(COLOR_BTNTEXT));
        bounds.left += 6;
        bounds.right -= 6;
        int modified = 0;
        TextBuffer *buffer = state->buffers[item->itemID];
        editor_core_send(buffer->core, EDITOR_GET_MODIFIED, &modified);
        char name[MAX_PATH + 4];
        wsprintfA(name, "%s%s", modified ? "* " : "", buffer_filename(buffer));
        HGDIOBJ font = SelectObject(item->hDC, GetStockObject(DEFAULT_GUI_FONT));
        int background = SetBkMode(item->hDC, TRANSPARENT);
        COLORREF color = SetTextColor(item->hDC, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(item->hDC, name, -1, &bounds,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(item->hDC, color);
        SetBkMode(item->hDC, background);
        SelectObject(item->hDC, font);
        return TRUE;
    }
    case WM_EDITOR_MODE_CHANGED: {
        synchronize_directory_path(hWnd, state);
        if (state->second_editor && state->pane_buffers[0] == state->pane_buffers[1]) {
            HWND other = state->active_pane ? GetDlgItem(hWnd, ID_EDITOR) : state->second_editor;
            InvalidateRect(other, NULL, FALSE);
        }
        if ((EditorMode)wp != EDITOR_MODE_COMMAND_LINE)
            state->command_error = NULL;
        RECT item_rect;
        HWND list = GetDlgItem(hWnd, ID_BUFFER_LIST);
        if (SendMessageA(list, LB_GETITEMRECT, (WPARAM)state->active_buffer, (LPARAM)&item_rect) != LB_ERR)
            InvalidateRect(list, &item_rect, FALSE);
        const char *mode_text;
        switch ((EditorMode)wp) {
        case EDITOR_MODE_INSERT: mode_text = "INSERT"; break;
        case EDITOR_MODE_REPLACE: mode_text = "REPLACE"; break;
        case EDITOR_MODE_VISUAL: mode_text = "VISUAL"; break;
        case EDITOR_MODE_VISUAL_LINE: mode_text = "VISUAL LINE"; break;
        case EDITOR_MODE_VISUAL_BLOCK: mode_text = "VISUAL BLOCK"; break;
        case EDITOR_MODE_COMMAND_LINE: {
            char prefix;
            editor_core_send(state->core, EDITOR_GET_COMMAND_PREFIX, &prefix);
            mode_text = prefix == '/' ? "SEARCH" : "COMMAND LINE";
            break;
        }
        default: {
            EditorDirectory directory;
            editor_core_send(state->core, EDITOR_GET_DIRECTORY, &directory);
            mode_text = directory.active ? "DIRECTORY" : "COMMAND";
            break;
        }
        }
        if ((EditorStatus)lp == EDITOR_STATUS_NO_CHANGE)
            mode_text = "no change";
        else if ((EditorStatus)lp == EDITOR_STATUS_INVALID_COMMAND)
            mode_text = "invalid command";
        else if ((EditorStatus)lp == EDITOR_STATUS_PATTERN_NOT_FOUND)
            mode_text = "pattern not found";
        else if ((EditorStatus)lp == EDITOR_STATUS_NO_PREVIOUS_SEARCH)
            mode_text = "no previous search";
        else if ((EditorStatus)lp == EDITOR_STATUS_INVALID_PATTERN)
            mode_text = "invalid regex";
        else if ((EditorStatus)lp == EDITOR_STATUS_REGEX_LIMIT)
            mode_text = "regex limit exceeded";
        else if ((EditorStatus)lp == EDITOR_STATUS_SUBSTITUTION_FAILED)
            mode_text = "substitution failed";
        if (state->command_error)
            mode_text = state->command_error;
        char hex_status[48];
        EditorControl *editor = (EditorControl *)GetWindowLongPtrA(active_editor(hWnd), GWLP_USERDATA);
        if (editor && editor->hex_mode) {
            mode_text = !editor->hex_editing ? "HEX COMMAND" : editor->hex_insert ? "HEX INSERT" : "HEX REPLACE";
            if (!editor->hex_editing && editor->hex_command[0]) {
                snprintf(hex_status, sizeof(hex_status), "HEX %s", editor->hex_command);
                mode_text = hex_status;
            }
        }
        SendMessageA(GetDlgItem(hWnd, ID_STATUS), SB_SETTEXTA, 0,
            (LPARAM)mode_text);
        update_buffer_status(hWnd, state->core);
        HWND command = GetDlgItem(hWnd, ID_COMMAND_LINE);
        if ((EditorMode)wp == EDITOR_MODE_COMMAND_LINE && !state->command_active) {
            state->command_active = 1;
            const char *prompt = ":";
            editor_core_send(state->core, EDITOR_GET_COMMAND_PROMPT, (void *)&prompt);
            SetWindowTextA(command, prompt);
            EnableWindow(command, TRUE);
            ShowWindow(command, SW_SHOW);
            layout_main_children(hWnd);
            SetFocus(command);
            size_t prompt_length = strlen(prompt);
            SendMessageA(command, EM_SETSEL, (WPARAM)prompt_length, (LPARAM)prompt_length);
        } else if ((EditorMode)wp != EDITOR_MODE_COMMAND_LINE && state->command_active) {
            state->command_active = 0;
            ShowWindow(command, SW_HIDE);
            EnableWindow(command, FALSE);
            SetWindowTextA(command, "");
            layout_main_children(hWnd);
            SetFocus(active_editor(hWnd));
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        return SendMessageA(active_editor(hWnd), msg, wp, lp);
    case WM_SETFOCUS:
        SetFocus(state && state->command_active ? GetDlgItem(hWnd, ID_COMMAND_LINE) : active_editor(hWnd));
        return 0;
    case WM_COMMAND: {
        if (state)
            state->window_pending = 0;
        if (LOWORD(wp) == ID_BUFFER_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            LRESULT index = SendMessageA(GetDlgItem(hWnd, ID_BUFFER_LIST), LB_GETCURSEL, 0, 0);
            if (index != LB_ERR)
                activate_document(hWnd, state, (size_t)index);
            return 0;
        }
        switch (LOWORD(wp)) {
        case ID_VIEW_TEXT:
        case ID_VIEW_HEX:
            if (!set_editor_view(hWnd, state, LOWORD(wp) == ID_VIEW_HEX))
                MessageBeep(MB_ICONWARNING);
            return 0;
        case ID_FILE_NEW: {
            open_document(hWnd, state, NULL);
            return 0;
        }
        case ID_FILE_OPEN: {
            char path[MAX_PATH] = "";
            if (choose_file(hWnd, path, 0))
                open_document(hWnd, state, path);
            return 0;
        }
        case ID_FILE_CLOSE:
            close_document(hWnd, state);
            return 0;
        case ID_FILE_SAVE:
        case ID_FILE_SAVE_AS:
            save_document(hWnd, state, LOWORD(wp) == ID_FILE_SAVE_AS);
            return 0;
        case ID_FILE_EXIT:
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (state) {
            for (size_t index = 0; index < state->buffer_count; ++index) {
                int modified = 0;
                editor_core_send(state->buffers[index]->core, EDITOR_GET_MODIFIED, &modified);
                if (modified) {
                    activate_document(hWnd, state, index);
                    if (!confirm_document_close(hWnd, state))
                        return 0;
                }
            }
        }
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        if (state) {
            for (size_t index = 0; index < state->buffer_count; ++index) {
                editor_core_destroy(state->buffers[index]->core);
                clear_syntax_cache(&state->buffers[index]->syntax);
                free(state->buffers[index]);
            }
            free(state->buffers);
        }
        free(state);
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, 0);
        break;
    }

    return DefWindowProcA(hWnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, INT nCmdShow)
{
    InitCommonControls();
    WNDCLASSA my_editor_cls;

    memset(&my_editor_cls, 0, sizeof(my_editor_cls));
    my_editor_cls.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    my_editor_cls.lpfnWndProc = my_editor_wnd_proc;
    my_editor_cls.hbrBackground = NULL;
    my_editor_cls.hCursor = LoadCursor(NULL, IDC_IBEAM);
    my_editor_cls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    my_editor_cls.hInstance = hInstance;
    my_editor_cls.lpszClassName = "PARAPAD_EDITOR";

    if (!RegisterClassA(&my_editor_cls))
        return 1;

    WNDCLASSA main_cls = { 0 };
    main_cls.lpfnWndProc = main_wnd_proc;
    main_cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    main_cls.hCursor = LoadCursor(NULL, IDC_ARROW);
    main_cls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    main_cls.hInstance = hInstance;
    main_cls.lpszClassName = "PARAPAD_CLASS";

    if (!RegisterClassA(&main_cls))
        return 1;

    HWND g_hwnd = CreateWindowA("PARAPAD_CLASS", "parapad",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 480, NULL, NULL, hInstance, NULL);
    if (!g_hwnd)
        return 1;

    ACCEL shortcuts[] = {
        { FVIRTKEY | FCONTROL, 'N', ID_FILE_NEW },
        { FVIRTKEY | FCONTROL, 'O', ID_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'S', ID_FILE_SAVE },
        { FVIRTKEY | FCONTROL, VK_F4, ID_FILE_CLOSE }
    };
    HACCEL accelerators = CreateAcceleratorTableA(shortcuts, sizeof(shortcuts) / sizeof(shortcuts[0]));
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    SetFocus(GetDlgItem(g_hwnd, ID_EDITOR));

    MSG msg;
    int result;
    while ((result = GetMessageA(&msg, NULL, 0, 0)) > 0) {
        MainWindowState *state = (MainWindowState *)GetWindowLongPtrA(g_hwnd, GWLP_USERDATA);
        int window_command = state && state->window_pending && msg.hwnd == active_editor(g_hwnd);
        if (window_command || !accelerators || !TranslateAcceleratorA(g_hwnd, accelerators, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (accelerators)
        DestroyAcceleratorTable(accelerators);
    return result == -1 ? 1 : (int)msg.wParam;
}
