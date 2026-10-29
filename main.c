#define WIN32_LEAN_AND_MEAN
#include <windows.h>


LRESULT my_editor_wnd_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
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
        PostQuitMessage(0);
        break;
    }

    return DefWindowProc(hWnd, msg, wp, lp);
}

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, INT nCmdShow)
{
    WNDCLASS my_editor_cls;

    memset(&my_editor_cls, 0, sizeof(my_editor_cls));
    my_editor_cls.style = CS_HREDRAW | CS_VREDRAW;
    my_editor_cls.lpfnWndProc = (WNDPROC)my_editor_wnd_proc;
    my_editor_cls.cbClsExtra = CS_HREDRAW | CS_VREDRAW;
    my_editor_cls.hbrBackground = NULL;
    my_editor_cls.hCursor = LoadCursor(NULL, IDC_ARROW);
    my_editor_cls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    my_editor_cls.hInstance = hInstance;
    my_editor_cls.lpszClassName = "PARAPAD_CLASS";

    RegisterClass(&my_editor_cls);

    HWND g_hwnd = CreateWindow("PARAPAD_CLASS", "parapad", WS_OVERLAPPEDWINDOW,
                               0, 0, 640, 480, HWND_DESKTOP, NULL, hInstance, NULL);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
