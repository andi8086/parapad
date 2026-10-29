#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdlib.h>

#include "editor_core.h"

#define ID_EDITOR 100
#define ID_FILE_EXIT 101

typedef struct EditorControl {
    EditorCore *core;
    HDC memdc;
    HBITMAP back_bitmap;
    HGDIOBJ original_bitmap;
    int back_width;
    int back_height;
    HFONT font;
    int line_height;
    size_t first_line;
    int left_pixel;
    char pending_lead;
} EditorControl;

static int character_to_ansi(unsigned long value, char *text, int capacity)
{
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

static int insert_ansi_character(EditorControl *editor, WPARAM value, unsigned int repeats)
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
        if (!editor_core_send(editor->core, EDITOR_ADD_CODEPOINT, &codepoint))
            return 0;
    return 1;
}

static int layout_editor_line(HDC dc, EditorControl *editor, size_t index,
    size_t byte_limit, int top, int paint, int hit_x, size_t *hit_column)
{
    EditorCharacter character = { index, 0, 0, 0 };
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
        if (paint)
            TextOutA(dc, 8 - editor->left_pixel + width, top, text, length);
        width += extent.cx;
        character.column += character.length;
    }
    return width;
}

static void update_editor_caret(HWND window, EditorControl *editor)
{
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

    int visible_width = rect.right > 18 ? rect.right - 18 : 1;
    if (caret_width < editor->left_pixel)
        editor->left_pixel = caret_width;
    else if (caret_width - editor->left_pixel > visible_width)
        editor->left_pixel = caret_width - visible_width;
    if (GetFocus() == window)
        SetCaretPos(8 + caret_width - editor->left_pixel,
            8 + (int)(caret.line - editor->first_line) * editor->line_height);
    InvalidateRect(window, NULL, FALSE);
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
        SelectObject(dc, old_font);
        ReleaseDC(hWnd, dc);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
    case WM_SETFOCUS:
        CreateCaret(hWnd, NULL, 2, editor->line_height);
        update_editor_caret(hWnd, editor);
        ShowCaret(hWnd);
        return 0;
    case WM_KILLFOCUS:
        editor->pending_lead = 0;
        DestroyCaret();
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
        int target = mouse_x - 8 + editor->left_pixel;
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
        EditorMessage message;
        switch (wp) {
        case VK_DELETE: message = EDITOR_DELETE_CHAR; break;
        case VK_LEFT: message = EDITOR_MOVE_LEFT; break;
        case VK_RIGHT: message = EDITOR_MOVE_RIGHT; break;
        case VK_UP: message = EDITOR_MOVE_UP; break;
        case VK_DOWN: message = EDITOR_MOVE_DOWN; break;
        case VK_HOME: message = EDITOR_MOVE_HOME; break;
        case VK_END: message = EDITOR_MOVE_END; break;
        default: return DefWindowProcA(hWnd, msg, wp, lp);
        }
        editor->pending_lead = 0;
        for (unsigned int repeat = 0; repeat < LOWORD(lp); ++repeat)
            editor_core_send(editor->core, message, NULL);
        update_editor_caret(hWnd, editor);
        return 0;
    }
    case WM_CHAR: {
        if (wp >= ' ' && wp != 127) {
            if (!insert_ansi_character(editor, wp, LOWORD(lp)))
                MessageBeep(MB_ICONWARNING);
            update_editor_caret(hWnd, editor);
            return 0;
        }
        for (unsigned int repeat = 0; repeat < LOWORD(lp); ++repeat) {
            int success = 1;
            if (wp == '\b') {
                editor->pending_lead = 0;
                success = editor_core_send(editor->core, EDITOR_BACKSPACE, NULL);
            } else if (wp == '\t') {
                editor->pending_lead = 0;
                success = editor_core_send(editor->core, EDITOR_INSERT_TEXT, "    ");
            } else if (wp == '\r') {
                editor->pending_lead = 0;
                success = editor_core_send(editor->core, EDITOR_ADD_CHAR, "\n");
            }
            if (!success) {
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
                    } else {
                        DeleteObject(replacement);
                    }
                }
            }

            HDC draw_dc = editor->back_bitmap && editor->back_width == width &&
                editor->back_height == height ? editor->memdc : dc;
            FillRect(draw_dc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
            SetBkMode(draw_dc, TRANSPARENT);
            SetTextColor(draw_dc, RGB(255, 0, 0));

            HGDIOBJ old_font = SelectObject(draw_dc, editor->font);
            int saved_dc = SaveDC(draw_dc);
            IntersectClipRect(draw_dc, 8, 8, width - 8, height - 8);
            size_t index = editor->first_line;
            for (int top = 8; top < height - 8; top += editor->line_height, ++index) {
                EditorLine line = { index, NULL, 0 };
                if (!editor_core_send(editor->core, EDITOR_GET_LINE, &line))
                    break;
                layout_editor_line(draw_dc, editor, index, line.length,
                    top, 1, 0, NULL);
            }
            if (saved_dc)
                RestoreDC(draw_dc, saved_dc);
            if (old_font && old_font != HGDI_ERROR)
                SelectObject(draw_dc, old_font);

            if (draw_dc == editor->memdc)
                BitBlt(dc, 0, 0, width, height, editor->memdc, 0, 0, SRCCOPY);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        if (editor) {
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

LRESULT CALLBACK main_wnd_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HMENU menu = CreateMenu();
        HMENU file_menu = CreatePopupMenu();
        if (!menu || !file_menu ||
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

        HWND editor = CreateWindowExA(WS_EX_CLIENTEDGE, "PARAPAD_EDITOR", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_EDITOR,
            ((LPCREATESTRUCTA)lp)->hInstance, ((LPCREATESTRUCTA)lp)->lpCreateParams);
        if (!editor)
            return -1;
        return 0;
    }
    case WM_SIZE: {
        RECT rect;
        GetClientRect(hWnd, &rect);
        int width = rect.right > 24 ? rect.right - 24 : 0;
        int height = rect.bottom > 24 ? rect.bottom - 24 : 0;
        MoveWindow(GetDlgItem(hWnd, ID_EDITOR), 12, 12, width, height, TRUE);
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(GetDlgItem(hWnd, ID_EDITOR));
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_FILE_EXIT) {
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hWnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, INT nCmdShow)
{
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

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    SetFocus(GetDlgItem(g_hwnd, ID_EDITOR));

    MSG msg;
    int result;
    while ((result = GetMessageA(&msg, NULL, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    editor_core_destroy(core);
    return result == -1 ? 1 : (int)msg.wParam;
}
