#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "editor_core.h"

#define ID_EDITOR 100
#define ID_FILE_EXIT 101
#define ID_STATUS 102
#define ID_FILE_NEW 103
#define ID_FILE_OPEN 104
#define ID_FILE_SAVE 105
#define ID_FILE_SAVE_AS 106
#define WM_EDITOR_MODE_CHANGED (WM_APP + 1)
#define WM_EDITOR_BUFFER_CHANGED (WM_APP + 2)

typedef struct CachedLine {
    char *text;
    size_t index;
    size_t length;
    EditorSelection selection;
    EditorLineCaret caret;
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
    int line_height;
    int text_left;
    size_t first_line;
    int left_pixel;
    char pending_lead;
    CachedLine *lines;
    size_t line_count;
    size_t painted_first_line;
    int painted_left_pixel;
    int bitmap_valid;
} EditorControl;

typedef struct MainWindowState {
    EditorCore *core;
    char path[MAX_PATH];
} MainWindowState;

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
    int width = 0;
    if (hit_column)
        *hit_column = 0;
    while (character.column < byte_limit &&
        editor_core_send(editor->core, EDITOR_GET_CHARACTER, &character)) {
        char text[8];
        int length = character_to_ansi(character.value, text, sizeof(text));
        SIZE extent = { 0, 0 };
        GetTextExtentPoint32A(dc, text, length, &extent);
        if (extent.cx > INT_MAX - width)
            break;
        if (hit_column) {
            if (hit_x < width + (extent.cx + 1) / 2)
                break;
            *hit_column = character.column + character.length;
        }
        if (paint) {
            int left = editor->text_left - editor->left_pixel + width;
            if (selection.active && character.column >= selection.start && character.column < selection.end) {
                RECT highlight = { left, top, left + extent.cx, top + editor->line_height };
                SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
                SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
                ExtTextOutA(dc, left, top, ETO_OPAQUE, &highlight, text, length, NULL);
                SetTextColor(dc, normal_color);
            } else {
                TextOutA(dc, left, top, text, length);
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
    return width;
}

static void update_editor_caret(HWND window, EditorControl *editor)
{
    EditorMode mode;
    editor_core_send(editor->core, EDITOR_GET_MODE, &mode);
    SendMessageA(GetParent(window), WM_EDITOR_MODE_CHANGED, (WPARAM)mode, 0);
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
    EditorCaret primary;
    editor_core_send(editor->core, EDITOR_GET_CARET, &primary);
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
    if (reuse && (editor->left_pixel != editor->painted_left_pixel || distance >= rows))
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
        if (reuse && cached && cached->valid && cached->index == index && cached->exists == exists &&
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
                SetTextColor(dc, RGB(255, 0, 0));
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
                cached->valid = buffered;
            }
        }
    }
    SelectObject(dc, old_font);
    editor->painted_first_line = editor->first_line;
    editor->painted_left_pixel = editor->left_pixel;
    editor->bitmap_valid = buffered && editor->line_count == rows;
}

LRESULT CALLBACK my_editor_wnd_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    EditorControl *editor = (EditorControl *)GetWindowLongPtrA(hWnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        editor = calloc(1, sizeof(*editor));
        if (!editor)
            return -1;
        editor->core = ((LPCREATESTRUCTA)lp)->lpCreateParams;
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)editor);
        HDC dc = GetDC(hWnd);
        editor->font = CreateFontA(-MulDiv(18, GetDeviceCaps(dc, LOGPIXELSY), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH, "Courier New");
        if (!editor->font)
            editor->font = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
        HGDIOBJ old_font = SelectObject(dc, editor->font);
        TEXTMETRICA metrics;
        GetTextMetricsA(dc, &metrics);
        editor->line_height = metrics.tmHeight + metrics.tmExternalLeading;
        SIZE number_size = { 0, 0 };
        GetTextExtentPoint32A(dc, "00000", 5, &number_size);
        editor->text_left = 8 + number_size.cx + 16;
        SelectObject(dc, old_font);
        ReleaseDC(hWnd, dc);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
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
    case WM_LBUTTONDOWN: {
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
        update_editor_caret(hWnd, editor);
        return 0;
    }
    case WM_KEYDOWN: {
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
        default: return DefWindowProcA(hWnd, msg, wp, lp);
        }
        editor->pending_lead = 0;
        for (unsigned int repeat = 0; repeat < LOWORD(lp); ++repeat)
            editor_core_send(editor->core, EDITOR_HANDLE_KEY, &key);
        update_editor_caret(hWnd, editor);
        return 0;
    }
    case WM_CHAR: {
        if (wp >= ' ' && wp != 127) {
            if (!dispatch_ansi_key(editor, wp, LOWORD(lp)))
                MessageBeep(MB_ICONWARNING);
            update_editor_caret(hWnd, editor);
            return 0;
        }
        editor->pending_lead = 0;
        unsigned long key = (unsigned long)wp;
        for (unsigned int repeat = 0; repeat < LOWORD(lp); ++repeat) {
            if (!editor_core_send(editor->core, EDITOR_HANDLE_KEY, &key)) {
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
    wsprintfA(title, "parapad - %s", state->path[0] ? state->path : "Untitled");
    SetWindowTextA(window, title);
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

static int save_document(HWND window, MainWindowState *state, int save_as)
{
    char path[MAX_PATH];
    lstrcpynA(path, state->path, MAX_PATH);
    if ((save_as || !path[0]) && !choose_file(window, path, 1))
        return 0;
    if (!editor_core_send(state->core, EDITOR_SAVE_FILE, path)) {
        MessageBoxA(window, "The file could not be saved. Check the path, permissions, and available disk space.",
            "parapad", MB_OK | MB_ICONERROR);
        return 0;
    }
    lstrcpynA(state->path, path, MAX_PATH);
    update_document_title(window, state);
    return 1;
}

static int confirm_document_close(HWND window, MainWindowState *state)
{
    int modified = 0;
    editor_core_send(state->core, EDITOR_GET_MODIFIED, &modified);
    if (!modified)
        return 1;
    int answer = MessageBoxA(window, "Save changes before continuing?", "parapad",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL)
        return 0;
    return answer != IDYES || save_document(window, state, 0);
}

LRESULT CALLBACK main_wnd_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MainWindowState *state = (MainWindowState *)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        state = calloc(1, sizeof(*state));
        if (!state)
            return -1;
        state->core = ((LPCREATESTRUCTA)lp)->lpCreateParams;
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, (LONG_PTR)state);
        HMENU menu = CreateMenu();
        HMENU file_menu = CreatePopupMenu();
        if (!menu || !file_menu ||
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_NEW, "&New\tCtrl+N") ||
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_OPEN, "&Open...\tCtrl+O") ||
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_SAVE, "&Save\tCtrl+S") ||
            !AppendMenuA(file_menu, MF_STRING, ID_FILE_SAVE_AS, "Save &As...") ||
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
        SendMessageA(status, SB_SETTEXTA, 0, (LPARAM)"COMMAND");

        HWND editor = CreateWindowExA(WS_EX_CLIENTEDGE, "PARAPAD_EDITOR", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_EDITOR,
            ((LPCREATESTRUCTA)lp)->hInstance, ((LPCREATESTRUCTA)lp)->lpCreateParams);
        if (!editor)
            return -1;
        update_document_title(hWnd, state);
        return 0;
    }
    case WM_SIZE: {
        RECT rect;
        GetClientRect(hWnd, &rect);
        HWND status = GetDlgItem(hWnd, ID_STATUS);
        SendMessageA(status, WM_SIZE, 0, 0);
        RECT status_rect = { 0 };
        GetWindowRect(status, &status_rect);
        int available_height = rect.bottom - (status_rect.bottom - status_rect.top);
        int width = rect.right > 24 ? rect.right - 24 : 0;
        int height = available_height > 24 ? available_height - 24 : 0;
        MoveWindow(GetDlgItem(hWnd, ID_EDITOR), 12, 12, width, height, TRUE);
        return 0;
    }
    case WM_EDITOR_MODE_CHANGED: {
        const char *mode_text;
        switch ((EditorMode)wp) {
        case EDITOR_MODE_INSERT: mode_text = "INSERT"; break;
        case EDITOR_MODE_REPLACE: mode_text = "REPLACE"; break;
        case EDITOR_MODE_VISUAL: mode_text = "VISUAL"; break;
        case EDITOR_MODE_VISUAL_LINE: mode_text = "VISUAL LINE"; break;
        case EDITOR_MODE_VISUAL_BLOCK: mode_text = "VISUAL BLOCK"; break;
        default: mode_text = "COMMAND"; break;
        }
        SendMessageA(GetDlgItem(hWnd, ID_STATUS), SB_SETTEXTA, 0,
            (LPARAM)mode_text);
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(GetDlgItem(hWnd, ID_EDITOR));
        return 0;
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case ID_FILE_NEW: {
            EditorBuffer empty = { NULL, 0 };
            if (!confirm_document_close(hWnd, state))
                return 0;
            if (!editor_core_send(state->core, EDITOR_SET_BUFFER, &empty)) {
                MessageBoxA(hWnd, "The new document could not be created.", "parapad", MB_OK | MB_ICONERROR);
                return 0;
            }
            state->path[0] = 0;
            update_document_title(hWnd, state);
            SendMessageA(GetDlgItem(hWnd, ID_EDITOR), WM_EDITOR_BUFFER_CHANGED, 0, 0);
            SetFocus(GetDlgItem(hWnd, ID_EDITOR));
            return 0;
        }
        case ID_FILE_OPEN: {
            char path[MAX_PATH] = "";
            if (!choose_file(hWnd, path, 0) || !confirm_document_close(hWnd, state))
                return 0;
            if (!editor_core_send(state->core, EDITOR_LOAD_FILE, path)) {
                MessageBoxA(hWnd, "The file could not be loaded. Check access permissions and available memory.",
                    "parapad", MB_OK | MB_ICONERROR);
                return 0;
            }
            lstrcpynA(state->path, path, MAX_PATH);
            update_document_title(hWnd, state);
            SendMessageA(GetDlgItem(hWnd, ID_EDITOR), WM_EDITOR_BUFFER_CHANGED, 0, 0);
            SetFocus(GetDlgItem(hWnd, ID_EDITOR));
            return 0;
        }
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
        if (!state || confirm_document_close(hWnd, state))
            DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
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
    my_editor_cls.style = CS_HREDRAW | CS_VREDRAW;
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

    EditorCore *core = editor_core_create("");
    if (!core)
        return 1;

    HWND g_hwnd = CreateWindowA("PARAPAD_CLASS", "parapad",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL, hInstance, core);
    if (!g_hwnd) {
        editor_core_destroy(core);
        return 1;
    }

    ACCEL shortcuts[] = {
        { FVIRTKEY | FCONTROL, 'N', ID_FILE_NEW },
        { FVIRTKEY | FCONTROL, 'O', ID_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'S', ID_FILE_SAVE }
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
    editor_core_destroy(core);
    return result == -1 ? 1 : (int)msg.wParam;
}
