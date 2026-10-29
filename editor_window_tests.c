#include "main.c"

#include <assert.h>
#include <stdio.h>

static HHOOK dialog_hook;
static int dialog_answer = IDCANCEL;
static int dialog_count;

static LRESULT CALLBACK answer_dialog(int code, WPARAM wp, LPARAM lp)
{
    if (code == HCBT_ACTIVATE) {
        char name[32];
        GetClassNameA((HWND)wp, name, sizeof(name));
        if (lstrcmpA(name, "#32770") == 0) {
            ++dialog_count;
            PostMessageA((HWND)wp, WM_COMMAND, MAKEWPARAM(dialog_answer, BN_CLICKED), 0);
        }
    }
    return CallNextHookEx(dialog_hook, code, wp, lp);
}

static void window_keys(HWND window, const char *keys)
{
    HWND editor = GetDlgItem(window, ID_EDITOR);
    while (*keys)
        SendMessageA(editor, WM_CHAR, (unsigned char)*keys++, 1);
}

static void expect_window_buffer(EditorCore *core, const void *text, size_t length)
{
    EditorBuffer buffer;
    assert(editor_core_send(core, EDITOR_GET_BUFFER, &buffer));
    assert(buffer.length == length);
    assert(!length || memcmp(buffer.data, text, length) == 0);
}

static void expect_saved_file(const char *path, const void *bytes, DWORD length)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    unsigned char actual[1024];
    DWORD read;
    assert(ReadFile(file, actual, sizeof(actual), &read, NULL));
    assert(read == length && memcmp(actual, bytes, length) == 0);
    assert(CloseHandle(file));
}

static RECT child_bounds(HWND window, int id)
{
    RECT bounds;
    assert(GetWindowRect(GetDlgItem(window, id), &bounds));
    MapWindowPoints(NULL, window, (POINT *)&bounds, 2);
    return bounds;
}

static void expect_window_layout(HWND window, int command_active)
{
    RECT list = child_bounds(window, ID_BUFFER_LIST);
    RECT editor = child_bounds(window, ID_EDITOR);
    RECT status = child_bounds(window, ID_STATUS);
    assert(list.left == 0 && list.top == 0 && list.right > 0);
    assert(editor.left > list.right && editor.right > editor.left);
    assert(list.bottom <= status.top && editor.bottom <= status.top);
    if (command_active) {
        RECT command = child_bounds(window, ID_COMMAND_LINE);
        assert(command.left == editor.left && command.right == editor.right);
        assert(command.top >= editor.bottom && command.bottom <= status.top);
        assert(GetWindowLongA(GetDlgItem(window, ID_COMMAND_LINE), GWL_STYLE) & WS_VISIBLE);
    }
}

static void expect_active_border(HWND window, MainWindowState *state)
{
    HWND list = GetDlgItem(window, ID_BUFFER_LIST);
    int height = (int)SendMessageA(list, LB_GETITEMHEIGHT, 0, 0);
    HDC screen = GetDC(list);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 180, height);
    ReleaseDC(list, screen);
    assert(dc && bitmap);
    HGDIOBJ original = SelectObject(dc, bitmap);
    DRAWITEMSTRUCT item = { 0 };
    item.CtlID = ID_BUFFER_LIST;
    item.hDC = dc;
    item.rcItem.right = 180;
    item.rcItem.bottom = height;
    item.itemID = (UINT)state->active_buffer;
    assert(SendMessageA(window, WM_DRAWITEM, ID_BUFFER_LIST, (LPARAM)&item));
    assert(GetPixel(dc, 3, 2) == GetSysColor(COLOR_BTNTEXT));
    item.itemID = state->active_buffer ? 0 : 1;
    assert(SendMessageA(window, WM_DRAWITEM, ID_BUFFER_LIST, (LPARAM)&item));
    assert(GetPixel(dc, 3, 2) == GetSysColor(COLOR_BTNFACE));
    SelectObject(dc, original);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

int main(void)
{
    HINSTANCE instance = GetModuleHandleA(NULL);
    InitCommonControls();
    WNDCLASSA editor_class = { 0 };
    editor_class.lpfnWndProc = my_editor_wnd_proc;
    editor_class.hInstance = instance;
    editor_class.lpszClassName = "PARAPAD_EDITOR";
    assert(RegisterClassA(&editor_class));
    WNDCLASSA main_class = { 0 };
    main_class.lpfnWndProc = main_wnd_proc;
    main_class.hInstance = instance;
    main_class.lpszClassName = "ParapadBuffersTest";
    assert(RegisterClassA(&main_class));
    dialog_hook = SetWindowsHookExA(WH_CBT, answer_dialog, NULL, GetCurrentThreadId());
    assert(dialog_hook);
    HWND window = CreateWindowA(main_class.lpszClassName, "", WS_OVERLAPPEDWINDOW,
        0, 0, 800, 480, NULL, NULL, instance, NULL);
    assert(window);
    MainWindowState *state = (MainWindowState *)GetWindowLongPtrA(window, GWLP_USERDATA);
    HWND list = GetDlgItem(window, ID_BUFFER_LIST);
    HWND editor_window = GetDlgItem(window, ID_EDITOR);
    EditorControl *editor = (EditorControl *)GetWindowLongPtrA(editor_window, GWLP_USERDATA);
    assert(state->buffer_count == 1 && state->core == editor->core);
    window_keys(window, "iinitial\033");
    EditorCore *first = state->core;
    SendMessageA(window, WM_COMMAND, ID_FILE_NEW, 0);
    assert(state->buffer_count == 2 && state->core != first && dialog_count == 0);
    assert(SendMessageA(list, LB_GETCOUNT, 0, 0) == 2);
    assert(SendMessageA(list, LB_GETCURSEL, 0, 0) == 1);
    expect_window_buffer(first, "initial", 7);
    window_keys(window, "isecond\033");
    EditorCore *second = state->core;
    SendMessageA(list, LB_SETCURSEL, 0, 0);
    SendMessageA(window, WM_COMMAND, MAKEWPARAM(ID_BUFFER_LIST, LBN_SELCHANGE), (LPARAM)list);
    assert(state->core == first && editor->core == first);
    window_keys(window, "u");
    expect_window_buffer(first, "", 0);
    expect_window_buffer(second, "second", 6);
    window_keys(window, "\022");
    expect_window_buffer(first, "initial", 7);
    expect_active_border(window, state);
    expect_window_layout(window, 0);
    assert(SetWindowPos(window, NULL, 0, 0, 320, 240, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    expect_window_layout(window, 0);
    window_keys(window, ":");
    expect_window_layout(window, 1);
    SendMessageA(GetDlgItem(window, ID_COMMAND_LINE), WM_CHAR, 27, 1);
    assert(SetWindowPos(window, NULL, 0, 0, 900, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    expect_window_layout(window, 0);

    char long_text[380];
    memset(long_text, '\n', 80);
    memset(long_text + 80, 'a', 300);
    EditorBuffer contents = { (const unsigned char *)long_text, sizeof(long_text) };
    assert(editor_core_send(first, EDITOR_SET_BUFFER, &contents));
    EditorCaret caret = { 80, 250 };
    assert(editor_core_send(first, EDITOR_SET_CARET, &caret));
    SendMessageA(editor_window, WM_EDITOR_REFRESH, 0, 0);
    size_t top = editor->first_line;
    int left = editor->left_pixel;
    assert(top > 0 && left > 0);
    window_keys(window, "i");
    activate_document(window, state, 1);
    EditorMode mode;
    assert(editor_core_send(second, EDITOR_GET_MODE, &mode) && mode == EDITOR_MODE_COMMAND);
    activate_document(window, state, 0);
    assert(editor->first_line == top && editor->left_pixel == left);
    assert(editor_core_send(first, EDITOR_GET_CARET, &caret) && caret.line == 80 && caret.column == 250);
    assert(editor_core_send(first, EDITOR_GET_MODE, &mode) && mode == EDITOR_MODE_INSERT);
    window_keys(window, "\033:");
    assert(state->command_active);
    activate_document(window, state, 1);
    assert(!state->command_active &&
        !(GetWindowLongA(GetDlgItem(window, ID_COMMAND_LINE), GWL_STYLE) & WS_VISIBLE));
    assert(editor_core_send(first, EDITOR_GET_MODE, &mode) && mode == EDITOR_MODE_COMMAND);

    const unsigned char raw[] = { 'A', 0, 0xFF, '\r', '\n', 'B' };
    char directory[MAX_PATH];
    char paths[2][MAX_PATH];
    assert(GetTempPathA(MAX_PATH, directory));
    for (int index = 0; index < 2; ++index) {
        assert(GetTempFileNameA(directory, "ppd", 0, paths[index]));
        HANDLE file = CreateFileA(paths[index], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        assert(file != INVALID_HANDLE_VALUE);
        DWORD written;
        assert(WriteFile(file, raw, sizeof(raw), &written, NULL) && written == sizeof(raw));
        assert(CloseHandle(file));
        assert(open_document(window, state, paths[index]));
        assert(state->buffer_count == (size_t)index + 3);
        expect_window_buffer(state->core, raw, sizeof(raw));
    }
    window_keys(window, "iX\033");
    SendMessageA(window, WM_COMMAND, ID_FILE_SAVE, 0);
    const unsigned char changed[] = { 'X', 'A', 0, 0xFF, '\r', '\n', 'B' };
    expect_saved_file(paths[1], changed, sizeof(changed));
    expect_saved_file(paths[0], raw, sizeof(raw));
    char label[MAX_PATH];
    SendMessageA(list, LB_GETTEXT, (WPARAM)state->active_buffer, (LPARAM)label);
    assert(lstrcmpA(label, buffer_filename(state->buffers[state->active_buffer])) == 0);
    SendMessageA(window, WM_COMMAND, ID_FILE_CLOSE, 0);
    assert(state->buffer_count == 3 && state->active_buffer == 2 && dialog_count == 0);
    expect_window_buffer(state->core, raw, sizeof(raw));
    EditorCore *before_failure = state->core;
    char missing[MAX_PATH + 16];
    wsprintfA(missing, "%s.missing", paths[0]);
    dialog_answer = IDOK;
    assert(!open_document(window, state, missing));
    assert(state->buffer_count == 3 && state->core == before_failure);

    window_keys(window, "i!\033");
    dialog_answer = IDCANCEL;
    SendMessageA(window, WM_COMMAND, ID_FILE_CLOSE, 0);
    assert(state->buffer_count == 3 && state->core == before_failure);
    dialog_answer = IDYES;
    SendMessageA(window, WM_COMMAND, ID_FILE_CLOSE, 0);
    assert(state->buffer_count == 2 && state->core == second);
    const unsigned char saved[] = { '!', 'A', 0, 0xFF, '\r', '\n', 'B' };
    expect_saved_file(paths[0], saved, sizeof(saved));
    dialog_answer = IDNO;
    SendMessageA(window, WM_COMMAND, ID_FILE_CLOSE, 0);
    assert(state->buffer_count == 1 && state->core == first);
    SendMessageA(window, WM_COMMAND, ID_FILE_CLOSE, 0);
    assert(state->buffer_count == 1 && state->buffers[0]->path[0] == 0);
    expect_window_buffer(state->core, "", 0);
    assert(SendMessageA(list, LB_GETCOUNT, 0, 0) == 1);

    window_keys(window, "ifirst\033");
    SendMessageA(window, WM_COMMAND, ID_FILE_NEW, 0);
    window_keys(window, "isecond\033");
    dialog_answer = IDCANCEL;
    SendMessageA(window, WM_CLOSE, 0, 0);
    assert(IsWindow(window) && state->buffer_count == 2 && state->active_buffer == 0);
    int before_exit = dialog_count;
    dialog_answer = IDNO;
    SendMessageA(window, WM_CLOSE, 0, 0);
    assert(!IsWindow(window) && dialog_count == before_exit + 2);
    assert(UnhookWindowsHookEx(dialog_hook));
    assert(DeleteFileA(paths[0]) && DeleteFileA(paths[1]));
    puts("Window buffer tests passed: independent state, additive open/new, active border, save, close/cancel, failed open, and all-buffer exit prompts.");
    return 0;
}