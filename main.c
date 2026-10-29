#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ID_EDITOR 100
#define ID_FILE_EXIT 101

LRESULT CALLBACK my_editor_wnd_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static HDC memdc;
    static HBITMAP back_bitmap;
    static HGDIOBJ original_bitmap;
    static int back_width;
    static int back_height;
    static HFONT font;

    switch (msg) {
    case WM_PAINT: {
        RECT rect;
        int width, height;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hWnd, &ps);
        GetClientRect(hWnd, &rect);

        width = rect.right - rect.left;
        height = rect.bottom - rect.top;

        if (width > 0 && height > 0) {
            if (!memdc)
                memdc = CreateCompatibleDC(dc);

            if (memdc && (!back_bitmap || width != back_width || height != back_height)) {
                HBITMAP replacement = CreateCompatibleBitmap(dc, width, height);
                if (replacement) {
                    HGDIOBJ previous = SelectObject(memdc, replacement);
                    if (previous && previous != HGDI_ERROR) {
                        if (back_bitmap)
                            DeleteObject(back_bitmap);
                        else
                            original_bitmap = previous;

                        back_bitmap = replacement;
                        back_width = width;
                        back_height = height;
                    } else {
                        DeleteObject(replacement);
                    }
                }
            }

            if (!font) {
                font = CreateFont(
                    -MulDiv(18, GetDeviceCaps(dc, LOGPIXELSY), 72),
                    0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    FIXED_PITCH, TEXT("Consolas")
                );
            }

            HDC draw_dc = back_bitmap && back_width == width && back_height == height
                ? memdc : dc;
            FillRect(draw_dc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
            SetBkMode(draw_dc, TRANSPARENT);
            SetTextColor(draw_dc, RGB(255, 0, 0));

            HGDIOBJ old_font = SelectObject(draw_dc,
                font ? (HGDIOBJ)font : GetStockObject(SYSTEM_FIXED_FONT));
            DrawTextEx(draw_dc, "foo", -1, &rect, DT_TOP | DT_LEFT, NULL);
            DrawTextEx(draw_dc, "foo", -1, &rect, DT_BOTTOM | DT_RIGHT | DT_SINGLELINE, NULL);
            if (old_font && old_font != HGDI_ERROR)
                SelectObject(draw_dc, old_font);

            if (draw_dc == memdc)
                BitBlt(dc, 0, 0, width, height, memdc, 0, 0, SRCCOPY);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        if (back_bitmap) {
            SelectObject(memdc, original_bitmap);
            DeleteObject(back_bitmap);
            back_bitmap = NULL;
            original_bitmap = NULL;
            back_width = 0;
            back_height = 0;
        }
        if (memdc) {
            DeleteDC(memdc);
            memdc = NULL;
        }
        if (font) {
            DeleteObject(font);
            font = NULL;
        }
        return 0;
    }

    return DefWindowProc(hWnd, msg, wp, lp);
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

        HWND editor = CreateWindowEx(WS_EX_CLIENTEDGE, "PARAPAD_EDITOR", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)ID_EDITOR,
            ((LPCREATESTRUCT)lp)->hInstance, NULL);
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

    return DefWindowProc(hWnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, INT nCmdShow)
{
    WNDCLASS my_editor_cls;

    memset(&my_editor_cls, 0, sizeof(my_editor_cls));
    my_editor_cls.style = CS_HREDRAW | CS_VREDRAW;
    my_editor_cls.lpfnWndProc = my_editor_wnd_proc;
    my_editor_cls.hbrBackground = NULL;
    my_editor_cls.hCursor = LoadCursor(NULL, IDC_ARROW);
    my_editor_cls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    my_editor_cls.hInstance = hInstance;
    my_editor_cls.lpszClassName = "PARAPAD_EDITOR";

    if (!RegisterClass(&my_editor_cls))
        return 1;

    WNDCLASS main_cls = { 0 };
    main_cls.lpfnWndProc = main_wnd_proc;
    main_cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    main_cls.hCursor = LoadCursor(NULL, IDC_ARROW);
    main_cls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    main_cls.hInstance = hInstance;
    main_cls.lpszClassName = "PARAPAD_CLASS";

    if (!RegisterClass(&main_cls))
        return 1;

    HWND g_hwnd = CreateWindow("PARAPAD_CLASS", "parapad",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL, hInstance, NULL);
    if (!g_hwnd)
        return 1;

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
