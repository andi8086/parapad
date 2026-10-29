#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor_core.h"
#include "lexer.h"

static UINT legacy_wheel_message;

typedef struct SyntaxLine {
    LexerLine parsed;
    size_t version;
    int valid;
} SyntaxLine;

typedef struct SyntaxCache {
    SyntaxLine *lines;
    size_t line_count;
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
#define WM_EDITOR_MODE_CHANGED (WM_APP + 1)
#define WM_EDITOR_BUFFER_CHANGED (WM_APP + 2)
#define WM_EDITOR_REFRESH (WM_APP + 3)
#define WM_EDITOR_SWITCH_BUFFER (WM_APP + 4)

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
} MainWindowState;

static void clear_syntax_cache(SyntaxCache *cache)
{
    for (size_t index = 0; index < cache->line_count; ++index)
        cache->lexer->free_line(&cache->lines[index].parsed);
    free(cache->lines);
    memset(cache, 0, sizeof(*cache));
}

static int update_syntax_cache(SyntaxCache *cache, EditorCore *core)
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
    if (cache->valid && change.first_line == (size_t)-1 && change.revision == cache->revision)
        return 1;
    size_t count;
    editor_core_send(core, EDITOR_GET_LINE_COUNT, &count);
    size_t first = cache->valid && change.first_line != (size_t)-1 ? change.first_line : 0;
    size_t last = cache->valid ? change.last_line : (size_t)-1;
    if (count != cache->line_count) {
        if (count > (size_t)-1 / sizeof(*cache->lines)) {
            cache->valid = 0;
            return 0;
        }
        SyntaxLine *lines = calloc(count, sizeof(*lines));
        if (!lines) {
            cache->valid = 0;
            return 0;
        }
        size_t retained = count < cache->line_count ? count : cache->line_count;
        if (retained)
            memcpy(lines, cache->lines, retained * sizeof(*lines));
        for (size_t index = retained; index < cache->line_count; ++index)
            lexer->free_line(&cache->lines[index].parsed);
        free(cache->lines);
        cache->lines = lines;
        cache->line_count = count;
        last = (size_t)-1;
    }
    if (first >= count)
        first = count - 1;
    if (first && !cache->lines[first - 1].valid)
        first = 0;
    const void *state = NULL;
    if (first)
        state = cache->lines[first - 1].parsed.end_state;
    EditorBuffer buffer;
    editor_core_send(core, EDITOR_GET_TEXT_BUFFER, &buffer);
    size_t offset = 0;
    for (size_t index = 0; index < first; ++index) {
        const unsigned char *newline = memchr(buffer.data + offset, '\n', buffer.length - offset);
        if (!newline) {
            cache->valid = 0;
            return 0;
        }
        offset = (size_t)(newline - buffer.data) + 1;
    }
    for (size_t index = first; index < count; ++index) {
        const unsigned char *text = buffer.data + offset;
        const unsigned char *newline = memchr(text, '\n', buffer.length - offset);
        size_t length = newline ? (size_t)(newline - text) : buffer.length - offset;
        SyntaxLine *line = &cache->lines[index];
        LexerLine parsed = { 0 };
        if (!lexer->lex_line((const char *)text, length, state, &parsed)) {
            cache->valid = 0;
            return 0;
        }
        int converged = line->valid && lexer->states_equal(line->parsed.end_state, parsed.end_state);
        lexer->free_line(&line->parsed);
        line->parsed = parsed;
        line->valid = 1;
        if (!++cache->serial)
            ++cache->serial;
        line->version = cache->serial;
        state = parsed.end_state;
        offset += length + (newline != NULL);
        if (index >= last && converged)
            break;
    }
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
    if (!count)
        return 0;
    unsigned long codepoint = units[0];
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

static int layout_editor_line(HDC dc, EditorControl *editor, size_t index,
    size_t byte_limit, int top, int paint, int hit_x, size_t *hit_column)
{
    EditorCharacter character = { index, 0, 0, 0 };
    EditorSelection selection = { index, 0, 0, 0, 0 };
    if (paint)
        editor_core_send(editor->core, EDITOR_GET_SELECTION, &selection);
    COLORREF normal_color = GetTextColor(dc);
    COLORREF normal_background = GetBkColor(dc);
    const LexerLine *syntax = paint && editor->syntax && editor->syntax_ready &&
        editor->syntax->lexer && index < editor->syntax->line_count ?
        &editor->syntax->lines[index].parsed : NULL;
    size_t token_index = 0;
    EditorSettings settings;
    editor_core_send(editor->core, EDITOR_GET_SETTINGS, &settings);
    SIZE space_size = { 0, 0 };
    GetTextExtentPoint32A(dc, " ", 1, &space_size);
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
        GetTextExtentPoint32A(dc, text, length, &extent);
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
                RECT highlight = { left, top, left + extent.cx, top + editor->line_height };
                SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
                SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
                ExtTextOutA(dc, left, top, ETO_OPAQUE, &highlight, text, length, NULL);
                SetTextColor(dc, normal_color);
            } else {
                TextOutA(dc, left, top, text, length);
                if (column_limit >= 0 && width + extent.cx > column_limit) {
                    int overflow = column_limit > width ? column_limit - width : 0;
                    RECT highlight = { left + overflow, top, left + extent.cx, top + editor->line_height };
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
        GetTextExtentPoint32A(dc, " ", 1, &space);
        int left = editor->text_left - editor->left_pixel + width;
        RECT highlight = { left, top, left + space.cx, top + editor->line_height };
        SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
        ExtTextOutA(dc, left, top, ETO_OPAQUE, &highlight, "", 0, NULL);
    }
    SetTextColor(dc, normal_color);
    SetBkColor(dc, normal_background);
    return width;
}

static void update_editor_caret(HWND window, EditorControl *editor)
{
    EditorBufferSwitch request;
    if (editor_core_send(editor->core, EDITOR_TAKE_BUFFER_SWITCH, &request) && request.count) {
        SendMessageA(GetParent(window), WM_EDITOR_SWITCH_BUFFER, 0, (LPARAM)&request);
        return;
    }
    EditorMode mode;
    editor_core_send(editor->core, EDITOR_GET_MODE, &mode);
    EditorStatus status;
    editor_core_send(editor->core, EDITOR_GET_STATUS, &status);
    SendMessageA(GetParent(window), WM_EDITOR_MODE_CHANGED, (WPARAM)mode, (LPARAM)status);
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
    int syntax_ready = update_syntax_cache(editor->syntax, editor->core);
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
        RECT rect = { 0, 0, width, height };
        FillRect(dc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
        for (size_t index = 0; index < editor->line_count; ++index)
            editor->lines[index].valid = 0;
    }

    HGDIOBJ old_font = SelectObject(dc, editor->font);
    SetBkMode(dc, TRANSPARENT);
    SIZE space_size = { 0, 0 };
    GetTextExtentPoint32A(dc, " ", 1, &space_size);
    int guide = -1;
    if (space_size.cx > 0 && settings.maxcol &&
        settings.maxcol <= (size_t)((INT_MAX - editor->text_left) / space_size.cx)) {
        int position = editor->text_left + (int)settings.maxcol * space_size.cx - editor->left_pixel;
        if (position >= editor->text_left && position < width - 8)
            guide = position;
    }
    HBRUSH guide_brush = guide >= 0 ? CreateSolidBrush(RGB(64, 64, 64)) : NULL;
    for (size_t row = 0; row < rows; ++row) {
        size_t index = editor->first_line + row;
        EditorLine line = { index, NULL, 0 };
        int exists = editor_core_send(editor->core, EDITOR_GET_LINE, &line);
        EditorSelection selection = { index, 0, 0, 0, 0 };
        if (exists)
            editor_core_send(editor->core, EDITOR_GET_SELECTION, &selection);
        EditorLineCaret caret = { index, 0, 0, 0 };
        if (exists && focused && index != primary.line)
            editor_core_send(editor->core, EDITOR_GET_LINE_CARET, &caret);
        CachedLine *cached = editor->lines ? &editor->lines[index % rows] : NULL;
        size_t syntax_version = syntax_ready && editor->syntax && editor->syntax->lexer &&
            index < editor->syntax->line_count ? editor->syntax->lines[index].version : 0;
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
        RECT dirty = { 0, top, width, top + editor->line_height };
        if (dirty.bottom > height - 8)
            dirty.bottom = height - 8;
        FillRect(dc, &dirty, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (guide_brush) {
            RECT marker = { guide, dirty.top, guide + 1, dirty.bottom };
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
                    GetTextExtentPoint32A(dc, " ", 1, &space);
                    int left = editor->text_left - editor->left_pixel + offset;
                    if (space.cx > 0 && caret.advance <= (size_t)((INT_MAX - offset) / space.cx)) {
                        left += (int)caret.advance * space.cx;
                        RECT cursor = { left, top, left + 2, top + editor->line_height };
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
    GetTextExtentPoint32A(dc, "00000", 5, &number_size);
    SelectObject(dc, old_font);
    ReleaseDC(window, dc);
    HFONT previous = editor->font;
    editor->font = font;
    editor->font_points = points;
    editor->line_height = metrics.tmHeight + metrics.tmExternalLeading;
    if (editor->line_height < 1)
        editor->line_height = 1;
    editor->text_left = 8 + number_size.cx + 16;
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
        update_editor_caret(hWnd, editor);
        return 0;
    case WM_SETFOCUS:
        CreateCaret(hWnd, NULL, 2, editor->line_height);
        update_editor_caret(hWnd, editor);
        ShowCaret(hWnd);
        return 0;
    case WM_KILLFOCUS:
        editor->pending_lead = 0;
        DestroyCaret();
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_SIZE:
        if (editor)
            update_editor_caret(hWnd, editor);
        return 0;
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
        size_t count;
        editor_core_send(editor->core, EDITOR_GET_LINE_COUNT, &count);
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
        SetFocus(hWnd);
        int mouse_x = (short)LOWORD(lp);
        int mouse_y = (short)HIWORD(lp);
        size_t count;
        editor_core_send(editor->core, EDITOR_GET_LINE_COUNT, &count);
        EditorCaret caret = {
            editor->first_line + (mouse_y > 8 ? (mouse_y - 8) / editor->line_height : 0), 0
        };
        if (caret.line >= count)
            caret.line = count - 1;
        EditorLine line = { caret.line, NULL, 0 };
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
        editor->pending_lead = 0;
        for (unsigned int repeat = 0; repeat < LOWORD(lp); ++repeat)
            send_editor_key(editor, key);
        update_editor_caret(hWnd, editor);
        return 0;
    }
    case WM_CHAR: {
        if (GetKeyState(VK_CONTROL) < 0 &&
            (wp == '+' || wp == '=' || wp == '-' || wp == '_' || wp == 0 || wp == 29 || wp == 31))
            return 0;
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
        EditorDirectoryEntry entry = { "..", parent, 1 };
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
        EditorDirectoryEntry entry = { name, entry_path,
            (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 };
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

static TextBuffer *create_document(const char *path)
{
    if (path && strlen(path) >= MAX_PATH)
        return NULL;
    TextBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        return NULL;
    buffer->core = editor_core_create("");
    EditorDirectoryFunctions functions = { NULL, list_directory, change_directory };
    DWORD attributes = path ? GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;
    EditorMessage load = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
        ? EDITOR_OPEN_DIRECTORY : EDITOR_LOAD_FILE;
    if (!buffer->core || !editor_core_send(buffer->core, EDITOR_REGISTER_DIRECTORY_FUNCTIONS, &functions) ||
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
    HWND editor = GetDlgItem(window, ID_EDITOR);
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
    EditorLineEnding line_ending = EDITOR_LINE_ENDING_LF;
    editor_core_send(core, EDITOR_GET_LINE_ENDING, &line_ending);
    EditorPosition position = { 0 };
    position.line_count = 1;
    editor_core_send(core, EDITOR_GET_POSITION, &position);
    unsigned int percent = position.length
        ? (unsigned int)(100.0 * ((double)position.offset / (double)position.length)) : 100;
    char text[192];
    snprintf(text, sizeof(text), "LN %zu/%zu, Col %zu    %u%% (%zu / %zu)",
        position.line + 1, position.line_count, position.column + 1, percent,
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
    HWND editor = GetDlgItem(window, ID_EDITOR);
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
    TextBuffer *buffer = state->buffers[index];
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
    dialog.lStructSize = OPENFILENAME_SIZE_VERSION_400A;
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
    HWND editor = GetDlgItem(window, ID_EDITOR);
    EditorControl *control = (EditorControl *)GetWindowLongPtrA(editor, GWLP_USERDATA);
    if (control) {
        control->syntax = &buffer->syntax;
        control->bitmap_valid = 0;
        InvalidateRect(editor, NULL, FALSE);
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
    HWND editor = GetDlgItem(window, ID_EDITOR);
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
    int status_parts[3] = { text_right > 544 ? text_right - 544 : 0,
        text_right > 64 ? text_right - 64 : 0, text_right };
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
    MoveWindow(editor, editor_left, 12, width, height, TRUE);
    MoveWindow(GetDlgItem(window, ID_COMMAND_LINE), editor_left, content_height, width, command_height, TRUE);
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
    if (strcmp(name, "tabe") && strcmp(name, "n") && strcmp(name, "w") &&
        strcmp(name, "q") && strcmp(name, "q!") && strcmp(name, "qa") && strcmp(name, "qa!"))
        return execute_ansi_command(state->core, text);
    while (*position == ' ' || *position == '\t')
        ++position;
    const char *end = position + strlen(position);
    while (end > position && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    int has_argument = end != position;
    char path[MAX_PATH] = "";
    if (has_argument) {
        if (strcmp(name, "tabe") && strcmp(name, "w"))
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
        SendMessageA(GetDlgItem(parent, ID_EDITOR), WM_EDITOR_REFRESH, 0, 0);
        return 0;
    }
    return CallWindowProcA(state->command_proc, window, message, wp, lp);
}

LRESULT CALLBACK main_wnd_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (legacy_wheel_message && msg == legacy_wheel_message)
        return SendMessageA(GetDlgItem(hWnd, ID_EDITOR), msg, wp, lp);
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
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_CLOSE, "&Close\tCtrl+W") ||
            !AppendMenuA(file_menu, MF_SEPARATOR, 0, NULL) ||
            !AppendMenu(file_menu, MF_STRING, ID_FILE_EXIT, "E&xit") ||
            !AppendMenu(menu, MF_POPUP, (UINT_PTR)file_menu, "&File")) {
            if (file_menu)
                DestroyMenu(file_menu);
            if (menu)
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
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
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
            SetFocus(GetDlgItem(hWnd, ID_EDITOR));
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        return SendMessageA(GetDlgItem(hWnd, ID_EDITOR), msg, wp, lp);
    case WM_SETFOCUS:
        SetFocus(GetDlgItem(hWnd, state && state->command_active ? ID_COMMAND_LINE : ID_EDITOR));
        return 0;
    case WM_COMMAND: {
        if (LOWORD(wp) == ID_BUFFER_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            LRESULT index = SendMessageA(GetDlgItem(hWnd, ID_BUFFER_LIST), LB_GETCURSEL, 0, 0);
            if (index != LB_ERR)
                activate_document(hWnd, state, (size_t)index);
            return 0;
        }
        switch (LOWORD(wp)) {
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
        { FVIRTKEY | FCONTROL, 'W', ID_FILE_CLOSE }
    };
    HACCEL accelerators = CreateAcceleratorTableA(shortcuts, sizeof(shortcuts) / sizeof(shortcuts[0]));
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    SetFocus(GetDlgItem(g_hwnd, ID_EDITOR));

    MSG msg;
    int result;
    while ((result = GetMessageA(&msg, NULL, 0, 0)) > 0) {
        if (!accelerators || !TranslateAcceleratorA(g_hwnd, accelerators, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (accelerators)
        DestroyAcceleratorTable(accelerators);
    return result == -1 ? 1 : (int)msg.wParam;
}
