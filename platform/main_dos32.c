#include <conio.h>
#include <stdio.h>

#include "version.h"
#include "dos32_tui.h"
#include "dos32_msg.h"

#define APP_NAME "ParaPad"

#define ID_MENU_BAR     1000
#define ID_MENU_FILE    1100
#define ID_MENU_FILE_NEW     1101
#define ID_MENU_FILE_OPEN    1102
#define ID_MENU_FILE_SAVE    1103
#define ID_MENU_FILE_SAVE_AS 1104
#define ID_MENU_FILE_CLOSE   1105
#define ID_MENU_FILE_EXIT    1106
#define ID_MENU_VIEW    1200
#define ID_MENU_VIEW_TEXT    1201
#define ID_MENU_VIEW_HEX     1202
#define ID_MENU_HELP    1300
#define ID_MENU_HELP_CONTENTS 1301
#define ID_MENU_HELP_SEARCH   1302
#define ID_MENU_HELP_ABOUT    1303

#define MSG_FILE_NEW     101
#define MSG_FILE_OPEN    102
#define MSG_FILE_SAVE    103
#define MSG_FILE_SAVE_AS 104
#define MSG_FILE_CLOSE   105
#define MSG_FILE_EXIT    106

#define MSG_VIEW_TEXT   110
#define MSG_VIEW_HEX    111

#define MSG_HELP_CONTENTS 120
#define MSG_HELP_SEARCH   121
#define MSG_HELP_ABOUT    122

#define KEY_ESC         1
#define KEY_ALT         56
#define KEY_ALTGR       1056

#define KEY_DOWN        1080
#define KEY_RIGHT       1077
#define KEY_UP          1072
#define KEY_LEFT        1075

#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_F11         87
#define KEY_F12         88

#define KEY_A           30
#define KEY_B           48
#define KEY_C           46
#define KEY_D           32
#define KEY_E           18
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_I           23
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_M           50
#define KEY_N           49
#define KEY_O           24
#define KEY_P           25
#define KEY_Q           16
#define KEY_R           19
#define KEY_S           31
#define KEY_T           20
#define KEY_U           22
#define KEY_V           47
#define KEY_W           17
#define KEY_X           45
#define KEY_Y           21
#define KEY_Z           44

#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11

#define KEY_PAD_1       79
#define KEY_PAD_2       80
#define KEY_PAD_3       81
#define KEY_PAD_4       75
#define KEY_PAD_5       76
#define KEY_PAD_6       77
#define KEY_PAD_7       71
#define KEY_PAD_8       72
#define KEY_PAD_9       73
#define KEY_PAD_0       82

#define KEY_BACKSPACE   14
#define KEY_DEL         1083
#define KEY_INS         1082
#define KEY_PGUP        1073
#define KEY_PGDN        1081
#define KEY_HOME        1071
#define KEY_END         1079

#define KEY_ENTER       28
#define KEY_PAD_ENTER   1028

int msg_handler(int msg, int iarg1, int iarg2, void *parg1, void *parg2)
{
        switch (msg) {
        case DM_KEY_DOWN:
                switch (iarg1) {
                case KEY_ENTER:
                        if (tui_menu_handle_enter()) {
                                dos_msg_push(DM_MENU_CLOSE, ID_MENU_BAR, 0, NULL, NULL);
                                tui_draw();
                                break;
                        }

                        break;
                case KEY_DOWN:
                        if (tui_menu_handle_down()) {
                                tui_draw();
                                break;
                        }
                        break;
                case KEY_UP:
                        if (tui_menu_handle_up()) {
                                tui_draw();
                                break;
                        }
                        break;
                case KEY_RIGHT:
                        if (tui_menu_handle_right()) {
                                tui_draw();
                                break;
                        }
                        break;
                case KEY_LEFT:
                        if (tui_menu_handle_left()) {
                                tui_draw();
                                break;
                        }
                        break;
                case KEY_F12:
                        dos_msg_push(DM_QUIT, 0, 0, NULL, NULL);
                        break;
                case KEY_ESC:
                        dos_msg_push(DM_MENU_CLOSE, ID_MENU_BAR, 0, NULL, NULL);
                        break;
                case KEY_ALT:
                        if (!tui_current_menu()) {
                                dos_msg_push(DM_MENU_OPEN,
                                             ID_MENU_BAR, 0, NULL, NULL);
                        } else {
                                dos_msg_push(DM_MENU_CLOSE,
                                             ID_MENU_BAR, 0, NULL, NULL);
                        }
                        break;
                default:
                        printf("%d\n", iarg1);
                        break;
                }
                break;
        case DM_KEY_UP:
                switch (iarg1) {
                case KEY_ALT:
                        if (tui_current_menu() == 1) {
                                dos_msg_push(DM_MENU_OPEN_POPUP,
                                             ID_MENU_BAR,
                                             MENU_SELECTION_CURRENT,
                                             NULL, NULL);
                        }
                        break;
                }
                break;
        case DM_COMMAND:
                switch (iarg1) {
                case MSG_FILE_EXIT:
                        dos_msg_push(DM_QUIT, 0, 0, NULL, NULL);
                        break;
                }

                break;
        }

        return 0;
}


int main(int argc, char **argv)
{
        printf("\n%s, %s\n%s\n\n", APP_NAME, VERSION_STR, COPYRIGHT_STR_ASCII);

        int rows, cols;

        tui_init(132, 43);
        tui_get_screen_size(&cols, &rows);
        tui_window_create(1, 2, cols, rows - 2, WINDOW_FRAME_DOUBLE, "");

        tui_menu_t *g_menu = tui_menu_new(1, 1, cols, 1, "", 0, MENU_BAR, ID_MENU_BAR);
        tui_set_main_menu(g_menu);


        tui_menu_t *g_menu_file = tui_menu_new(3, 1, 6, 1, "&File",
                0, MENU_POPUP, ID_MENU_FILE);
        tui_menu_t *g_menu_view = tui_menu_new(11, 1, 6, 1, "&View",
                0, MENU_POPUP, ID_MENU_VIEW);
        tui_menu_t *g_menu_help = tui_menu_new(19, 1, 6, 1, "&Help",
                0, MENU_POPUP, ID_MENU_HELP);

        tui_menu_append(g_menu, g_menu_file);
        tui_menu_append(g_menu, g_menu_view);
        tui_menu_append(g_menu, g_menu_help);

        tui_menu_t *g_menu_file_popup = tui_menu_new(2, 2, 20, 8, "",
                0, MENU_POPUP_WINDOW, 0);
        tui_menu_append(g_menu_file, g_menu_file_popup);

        tui_menu_t *g_menu_file_new = tui_menu_new(3, 3, 18, 1, "&New",
                MSG_FILE_NEW, MENU_ITEM, ID_MENU_FILE_NEW);
        tui_menu_append(g_menu_file_popup, g_menu_file_new);
        tui_menu_t *g_menu_file_open = tui_menu_new(3, 4, 18, 1, "&Open...",
                MSG_FILE_OPEN, MENU_ITEM, ID_MENU_FILE_OPEN);
        tui_menu_append(g_menu_file_popup, g_menu_file_open);
        tui_menu_t *g_menu_file_save = tui_menu_new(3, 5, 18, 1, "&Save",
                MSG_FILE_SAVE, MENU_ITEM, ID_MENU_FILE_SAVE);
        tui_menu_append(g_menu_file_popup, g_menu_file_save);
        tui_menu_t *g_menu_file_saveas = tui_menu_new(3, 6, 18, 1, "Save &As...",
                MSG_FILE_SAVE_AS, MENU_ITEM, ID_MENU_FILE_SAVE_AS);
        tui_menu_append(g_menu_file_popup, g_menu_file_saveas);
        tui_menu_t *g_menu_file_close = tui_menu_new(3, 7, 18, 1, "&Close",
                MSG_FILE_CLOSE, MENU_ITEM, ID_MENU_FILE_CLOSE);
        tui_menu_append(g_menu_file_popup, g_menu_file_close);
        tui_menu_t *g_menu_file_exit = tui_menu_new(3, 8, 18, 1, "E&xit",
                MSG_FILE_EXIT, MENU_ITEM, ID_MENU_FILE_EXIT);
        tui_menu_append(g_menu_file_popup, g_menu_file_exit);

        tui_menu_t *g_menu_view_popup = tui_menu_new(10, 2, 12, 4, "",
                0, MENU_POPUP_WINDOW, 0);
        tui_menu_append(g_menu_view, g_menu_view_popup);

        tui_menu_t *g_menu_view_text = tui_menu_new(11, 3, 10, 1, "&Text",
                MSG_VIEW_TEXT, MENU_ITEM, ID_MENU_VIEW_TEXT);
        tui_menu_append(g_menu_view_popup, g_menu_view_text);
        tui_menu_t *g_menu_view_hex = tui_menu_new(11, 4, 10, 1, "&Hex",
                MSG_VIEW_HEX, MENU_ITEM, ID_MENU_VIEW_HEX);
        tui_menu_append(g_menu_view_popup, g_menu_view_hex);

        tui_menu_t *g_menu_help_popup = tui_menu_new(18, 2, 25, 5, "",
                0, MENU_POPUP_WINDOW, 0);
        tui_menu_append(g_menu_help, g_menu_help_popup);

        tui_menu_t *g_menu_help_contents = tui_menu_new(19, 3, 23, 1, "&Contents",
                MSG_HELP_CONTENTS, MENU_ITEM, ID_MENU_HELP_CONTENTS);
        tui_menu_append(g_menu_help_popup, g_menu_help_contents);
        tui_menu_t *g_menu_help_search = tui_menu_new(19, 4, 23, 1, "&Search for Help on...",
                MSG_HELP_SEARCH, MENU_ITEM, ID_MENU_HELP_SEARCH);
        tui_menu_append(g_menu_help_popup, g_menu_help_search);
        tui_menu_t *g_menu_help_about = tui_menu_new(19, 5, 23, 1, "&About...",
                MSG_HELP_ABOUT, MENU_ITEM, ID_MENU_HELP_ABOUT);
        tui_menu_append(g_menu_help_popup, g_menu_help_about);

        tui_draw();

        tui_run(msg_handler);

        tui_deinit();

        return 0;
}
