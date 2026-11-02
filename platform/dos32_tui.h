#ifndef DOS32_TUI_H
#define DOS32_TUI_H

#define WINDOW_FRAME_SINGLE 1
#define WINDOW_FRAME_DOUBLE 2

#define MENU_BAR          1
#define MENU_POPUP        2
#define MENU_POPUP_WINDOW 3
#define MENU_ITEM         4

#define MENU_STATE_OPEN   1

#define MENU_SELECTION_CURRENT -1

void tui_init(int cols, int rows);
void tui_deinit();
void tui_draw(void);

int tui_window_create(int x, int y, int w, int h, int flags, char *title);

typedef struct tui_menu tui_menu_t;

tui_menu_t *tui_menu_new(int x, int y, int w, int h, char *text, int signal,
                         int flags, int id);
void tui_set_main_menu(tui_menu_t *menu);
void tui_menu_append(tui_menu_t *parent, tui_menu_t *child);
void tui_menu_open(tui_menu_t *menu);
void tui_menu_close(tui_menu_t *menu);
int tui_current_menu(void);
int tui_menu_handle_down(void);
int tui_menu_handle_up(void);
int tui_menu_handle_right(void);
int tui_menu_handle_left(void);
int tui_menu_handle_enter(void);

typedef int (*msg_handler_t)(int, int, int, void *, void *);

void tui_run(msg_handler_t msg_func);
void tui_get_screen_size(int *cols, int *rows);

#endif
