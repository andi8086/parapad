#include <conio.h>
#include <stdio.h>

#include "version.h"
#include "dos32_tui.h"

#define APP_NAME "ParaPad"



int main(int argc, char **argv)
{
        printf("\n%s, %s\n%s\n\n", APP_NAME, VERSION_STR, COPYRIGHT_STR_ASCII);

        tui_init();

        tui_window_create(1, 2, 80, 23, WINDOW_FRAME_DOUBLE, "");

        tui_menu_t *g_menu = tui_menu_new(1, 1, 80, 1, "", 0, MENU_BAR);
        tui_set_main_menu(g_menu);


        tui_menu_t *g_menu_file = tui_menu_new(3, 1, 6, 1, "&File", 0, MENU_POPUP);
        tui_menu_t *g_menu_view = tui_menu_new(11, 1, 6, 1, "&View", 0, MENU_POPUP);
        tui_menu_t *g_menu_help = tui_menu_new(19, 1, 6, 1, "&Help", 0, MENU_POPUP);

        tui_menu_append(g_menu, g_menu_file);
        tui_menu_append(g_menu, g_menu_view);
        tui_menu_append(g_menu, g_menu_help);

        tui_menu_t *g_menu_file_popup = tui_menu_new(2, 2, 20, 8, "", 0, MENU_POPUP_WINDOW);
        tui_menu_append(g_menu_file, g_menu_file_popup);

        tui_menu_t *g_menu_file_new = tui_menu_new(3, 3, 18, 1, "&New", 0, MENU_ITEM);
        tui_menu_append(g_menu_file_popup, g_menu_file_new);
        tui_menu_t *g_menu_file_open = tui_menu_new(3, 4, 18, 1, "&Open...", 0, MENU_ITEM);
        tui_menu_append(g_menu_file_popup, g_menu_file_open);
        tui_menu_t *g_menu_file_save = tui_menu_new(3, 5, 18, 1, "&Save", 0, MENU_ITEM);
        tui_menu_append(g_menu_file_popup, g_menu_file_save);
        tui_menu_t *g_menu_file_saveas = tui_menu_new(3, 6, 18, 1, "Save &As...", 0, MENU_ITEM);
        tui_menu_append(g_menu_file_popup, g_menu_file_saveas);
        tui_menu_t *g_menu_file_close = tui_menu_new(3, 7, 18, 1, "&Close", 0, MENU_ITEM);
        tui_menu_append(g_menu_file_popup, g_menu_file_close);
        tui_menu_t *g_menu_file_exit = tui_menu_new(3, 8, 18, 1, "E&xit", 0, MENU_ITEM);
        tui_menu_append(g_menu_file_popup, g_menu_file_exit);

        tui_menu_open(g_menu);
        tui_menu_open(g_menu_file);

        tui_draw();

        _getch();

        tui_deinit();

        return 0;
}
