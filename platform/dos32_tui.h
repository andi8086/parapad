#ifndef DOS32_TUI_H
#define DOS32_TUI_H

#define WINDOW_FRAME_SINGLE 1
#define WINDOW_FRAME_DOUBLE 2

#define MENU_BAR          1
#define MENU_POPUP        2
#define MENU_POPUP_WINDOW 3
#define MENU_ITEM         4

#define MENU_STATE_OPEN   1

void tui_init();
void tui_deinit();
void tui_draw(void);

int tui_window_create(int x, int y, int w, int h, int flags, char *title);

typedef struct tui_menu tui_menu_t;

tui_menu_t *tui_menu_new(int x, int y, int w, int h, char *text, int signal, int flags);
void tui_set_main_menu(tui_menu_t *menu);
void tui_menu_append(tui_menu_t *parent, tui_menu_t *child);
void tui_menu_open(tui_menu_t *menu);
void tui_menu_close(tui_menu_t *menu);

#endif
