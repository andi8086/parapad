# Platform adaptation mechanisms


## Text update mechanism

 * GTK4 Custom Widget
        * Overrides `snapshot` to record abstract font commands to Pango Layout
          arrays
        * Natively processes internal UTF-8 buffers
        * `gtk_widget_queue_draw` upon mutation

* Win32 Custom Class
        * Intercepts `WM_PAINT` to execute immediate bitmap draws using GDI
          function `ExtTextOutW`
        * Converts line on-the-fly from UTF-8 to UTF-16 using
          `MultiByteToWideChar` for Win95 support
        * `InvalidateRect(hwnd, NULL, FALSE)` upon mutation


## Principal Implementation in GTK4

```C
#include <gtk/gtk.h>

#define TYPE_MY_EDITOR(my_editor_get_type())
G_DECLARE_FINAL_TYPE(MyEditor, my_editor, MY, EDITOR, GtkWidget)

struct _MyEditor {
        GtkWidget parent_instance;
        void *core_engine;
};

G_DEFINE_TYPE(MyEditor, my_editor, GTK_TYPE_WIDGET)
```


```C
static void my_editor_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
        MyEditor *self = MY_EDITOR(widget);
        GtkStyleContext *context = gtk_widget_get_style_context(widget);

        int width = gtk_widget_get_width(widget);
        int height = gtk_widget_get_height(widget);

        GdkRGBA bg_color = {0.1f, 0.1f, 0.1f, 1.0f};
        graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
        gtk_snapshot_append_color(snapshot, &bg_color, &bounds);

        int y_offset = 0;
        int line_height = 18; // calculate based on font metrics

        for (int row = 0; row < visible_line_count; row++) {
                const char *line_text = ... retrieve from core

                PangoContext *pango_ctx = gtk_widget_get_pango_context(widget);
                PangoLayout *layout = pango_layout_new(pango_ctx);

                pango_layout_set_text(layout, line_text, -1);

                PangoAttrList *attrs = pango_attr_list_new();

                // apply keyword color to text (get info from core), here
                // 16-bit red
                PangoAttribute *fg = pango_attr_foreground_new(65535, 0, 0);
                fg->start_index = 0;
                fg->end_index = 4;

                pango_attr_list_insert(attrs, fg);
                pango_layout_set_attributes(layout, attrs);

                // render the styled layout into the snapshot command hierarchy
                gtk_snapshot_save(snapshot);
                gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(10,
                                       y_offset));
                gtk_snapshot_append_layout(snapshot, layout,
                                           &(GdkRGBA){1.0f, 1.0f, 1.0f, 1.0f});
                gtk_snapshot_restore(snapshot);

                // cleanup row resource allocations
                pango_attr_list_unref(attrs);
                g_object_unref(layout);
                y_offset += line_height;
        }
}


static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer data)
{
        MyEditor *self = MY_EDITOR(data);

        // map keyval to abstract internal EditorEvent
        // Pass event to editor_core_handle_event(self->core_engine, &ev)

        // Force widget queue to schedule a fresh snapshot step
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return TRUE;
}


static void my_editor_init(MyEditor *self)
{
        gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
        GtkEventController *key_ctrl = gtk_event_controller_key_new();
        g_signal_conect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed),
                        self);

        gtk_widget_add_controller(GTK_WIDGET(self), key_ctrl);
}


static void my_editor_class_init(MyEditorClass *klass)
{
        GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
        widget_class->snapshot = my_editor_snapshot;
}

```


## Principal implementation in Win32


```C

LRESULT CALLBACK MyEditorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
        void *core = (void *)GetWindowLongPtr(hwnd, GWL_USERDATA);
        void *new_core;
        PAINTSTRUCT ps;
        HDC hdc, memDC;
        HBITMAP memBitmap;
        HBRUSH bgBrush;
        int y_pos, line_height, row;
        RECT rc;

        switch (msg)
        {
        case WM_CREATE:
                new_core = malloc(...);
                SetWindowLongPtr(hwnd, GWL_USERDATA, (LONG_PTR)new_core);
                return 0;
        case WM_PAINT:
                hdc = BeginPaint(hwnd, &ps);
                GetClientRect(hwnd, &rc);
                // double buffering
                memDC = CreateCompatibleDC(hdc);
                memBitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
                SelectObject(memDC, memBitmap);

                bgBrush = CreateSolidBrush(RGB(26, 26, 26));
                FillRect(memDC, &rc, bgBrush);
                DeleteObject(bgBrush);

                SetBkMode(memDC, TRANSPARENT);


                y_pos = 0;
                line_height = 18;

                for (row = 0; row < 20; row++) {
                        const char *text_utf8 = "..."; // get from core
                        int byte_len = strlen(text_utf8);
                        wchar_t wbuf[512];
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, text_utf8,
                                                       byte_len, wbuf, 512);
                        SetTextColor(memDC, RGB(255, 85, 85));
                        ExtTextOutW(memDC, 10, y_pos, 0, NULL, wbuf, 4, NULL);
                        SetTextColor(memDC, RGB(255, 255, 255));
                        ExtTextOutW(memDC, 50, y_pos, 0, NULL, wbuf+4, wlen-4,
                                    NULL);

                        y_pos += line_height;
                }
                BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

                DeleteObject(memBitmap);
                DeleteObject(memDC);
                EndPaint(hwnd, &ps);
                return 0;
        }

        case WM_CHAR:
                // handle in core - handle_input(core, wp);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
        case WM_DESTROY:
                // call destructor of core and free
                return 0;

        }

        return DefWindowProc(hwnd, msg, wp, lp);
}



void RegisterMyEditorClass(HINSTANCE hInst)
{
        WNDCLASS wc = {0};

        wc.lpfnWndProc = MyEditorWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
        wc.hbrBackground = NULL; // prevent GDI bg erase flicker
        wc.lpszClassName = "CustomEditorWidget";

        RegisterClass(&wc);
}

```





## Principal ncurses implementation


```C

#include <ncurses.h>

enum {
        COLOR_PAIR_DEFAULT = 1,
        COLOR_PAIR_KEYWORD,
        COLOR_PAIR_STRING,
        COLOR_PAIR_COMMENT
};

void init_ncurses_display()
{
        initscr();
        raw();
        noecho();
        keypad(stdscr, TRUE);
        start_color();

        init_pair(COLOR_PAIR_DEFAULT, COLOR_WHITE, COLOR_BLACK);
        init_pair(COLOR_PAIR_KEYWORD, COLOR_RED,   COLOR_BLACK);
        init_pair(COLOR_PAIR_STRING,  COLOR_GREEN, COLOR_BLACK);
        init_pair(COLOR_PAIR_COMMENT, COLOR_CYAN,  COLOR_BLACK);
}


void ncurses_draw_line(int visual_row, const char *text_utf8,
                       const Token *tokens, int token_count)
{
        wmove(stdscr, visual_row, 0);
        wclrtoeol(stdscr);

        int current_byte = 0;

        for (int t = 0; t < token-count; t++) {
                Token tok = tokens[t];

                int pair = COLOR_PAIR_DEFAULT;

                if (tok.type == TOKEN_KEYWORD) {
                        pair = COLOR_PAIR_KEYWORD;
                }
                // ...

                for (int i = 0; i < tok.length; i++) {
                        waddch(stdscr, text_utf8[current_byte++]);
                }


                attroff(COLOR_PAIR(pair));
        }

}


void run_ncurses_editor_loop(void *core_engine)
{
        while (editor_is_running(core_engine)) {
                render_core_to_ncurses(core_engine);

                wmove(stdscr, get_core_cursor_row(core_engine),
                              get_core_cursor_col(core_engine));
                wrefresh(stdscr);

                int ch = wgetch(stdscr);

                EditorEvent ev;
                if (ch == KEY_LEFT) {
                        ev = (EditorEvent){
                                EVENT_KEY_PRESS,
                                .data.key.key_code = KEY_LEFT
                             };
                } else if (ch == 127 || ch == KEY_BACKSPACE) {
                        ev = (EditorEvent){
                                EVENT_KEY_PRESS,
                                .data.key.key_code = KEY_BACKSPACE_
                             };
                } else {
                        ev = (EditorEvent) {
                                EVENT_KEY_PRESS,
                                .data.key.key_code = ch
                             };
                }


                editor_core_handle_event(core_engine, &ev);
        }
}
```


## Principal DOS implementation


```C
#include <conio.h>

#define DOS_COLOR_DEFAULT (WHITE | (BLACK << 4))
#define DOS_COLOR_KEYWORD (LIGHTRED | (BLACK << 4))
#define DOS_COLOR_STRING (LIGHTGREEN | (BLACK << 4))
#define DOS_COLOR_COMMENT (LIGHTCYAN | (BLACK << 4))


void dos_draw_line(int visual_row, const char *text_utf8, const Token *tokens,
                   int token_count)
{
        gotoxy(1, visual_row + 1);

        int current_byte = 0;

        for (int t = 0; t < token_count; t++) {
                Token tok = tokens[t];

                int attr = DOS_COLOR_DEFAULT;

                if (tok.type == TOKEN_KEYWORD) attr = DOS_COLOR_KEYWORD;
                else if (tok.type == TOKEN_STRING) attr = DOS_COLOR_STRING;
                else if (tok.type == TOKEN_COMMENT) attr = DOS_COLOR_COMMENT;

                texattr(attr);

                int bytes_processed = 0;

                while (bytes_processed < tok.length) {
                        unsigned char c (unsigned char)text_utf8[current_byte];

                        if (c < 0x80) {
                                putch(c);
                                current_byte++;
                                bytes_processed++;

                        } else if (c == 0xC3 &&
                                   (unsigned char)text_utf9[current_byte + 1] ==
                                        0xBC) {
                                // multibyte thunking, example umlaut u matching

                                putch(0x81);
                                current_byte += 2;
                                bytes_processed += 2;
                        } else {
                                putch('?');
                                int char_len = utf8_char_len(&text_utf8[current_byte]);
                                current_byte += char_len;
                                bytes_processed += char_len;
                        }
                }
        }

        clreol();
}


void run_dos_editor_loop(void *core_engine)
{
        while (editor_is_running(core_engine)) {
                render_core_to_dos_screen(core_engine);

                gotoxy(get_core_cursor_col(core_engine) + 1,
                       get_core_cursor_row(core_engine) + 1);
                int ch = getch();

                EditorEvent ev;
                ev.type = EVENT_KEY_PRESS;

                if (ch == 0 || ch == 0xE0) {
                        // extended scan-code
                        int scan_code = getch();

                        if (scan_code == 75) {
                                ev.data.key.key_code = KEY_LEFT;
                        } else if (scan_code == 77) {
                                ev.data.key.key_code = KEY_RIGHT;
                        } else {
                                ev.data.key.key_code = KEY_UNKNOWN;
                        }
                } else {
                        if (ch == 8) ev.data.key.key_code = KEY_BACKSPACE;
                        else         ev.data.key.key_code = ch;

                }

                editor_core_handle_event(core_engine, &ev);
        }
}
```
