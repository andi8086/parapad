#include "dos32_tui.h"

#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <conio.h>


static uint16_t *vga_mem = (uint16_t *)0xB8000;
static int cols = 80;
static int rows = 25;

static void vga_locate(int x, int y)
{
        uint16_t pos = (y - 1) * cols + x;
        outp(0x3D4, 0x0F);
        outp(0x3D5, (unsigned char)(pos & 0xFF));
        outp(0x3D4, 0x0E);
        outp(0x3D5, (unsigned char)((pos >> 8) & 0xFF));

        unsigned short *bios_cursor_pos = (unsigned short *)(0x400 + 0x50);
        *bios_cursor_pos = ((y - 1) << 8) | (x - 1);
}



uint16_t bg_color = 0x17;
uint16_t a_color = 0x71;

#define WIN_MAX  16
#define MENU_MAX 16

typedef struct {
        int flags;
        int x, y, w, h;
        char *title;
} tui_window_t;


typedef struct tui_menu tui_menu_t;


struct tui_menu {
        int flags;
        int x, y, w, h;
        char *text;
        int accel;
        int signal;
        tui_menu_t *submenus[MENU_MAX];
        int nsub;
        int state;
        int selection;
};


static tui_window_t windows[WIN_MAX];
static int n_win = 0;
static tui_menu_t *main_menu = NULL;


tui_menu_t *tui_menu_new(int x, int y, int w, int h, char *text, int signal,
                         int flags)
{
        tui_menu_t *m;

        m = malloc(sizeof(tui_menu_t));
        m->x = x - 1;
        m->y = y - 1;
        m->w = w;
        m->h = h;
        m->flags = flags;
        m->signal = signal;
        m->nsub = 0;
        m->accel = -1;
        m->state = 0;
        m->selection = 0;
        if (text) {
                m->text = strdup(text);
                char *p = m->text;
                if (p) while (*p) {
                        if (*p == '&') {
                                m->accel = p - m->text;
                        }
                        p++;
                }
                if (m->accel != -1) {
                        for (int i = m->accel + 1; i < strlen(m->text) + 1; i++) {
                                m->text[i-1] = m->text[i];
                        }
                }
        }
        return m;
}


void tui_init(void)
{
        for (int i = 0; i < cols * rows; i++) {
                vga_mem[i] = bg_color << 8;
        }
        n_win = 0;

        vga_locate(2, 10);
}


void tui_set_main_menu(tui_menu_t *menu)
{
        main_menu = menu;
}


void tui_deinit(void)
{


}


static void tui_win_draw_framed(int x, int y, int w, int h)
{
        for (int r = y; r < y + h; r++) {
                for (int c = x; c < x + w; c++) {
                        vga_mem[r * cols + c] = bg_color << 8;
                        if ((c > x && c < x + w - 1) &&
                            (r == y || r == y + h - 1)) {
                                vga_mem[r * cols + c] |= 0xC4;
                        }
                        if ((r > y && r < y + h - 1) &&
                            (c == x || c == x + w - 1)) {
                                vga_mem[r * cols + c] |= 0xB3;
                        }
                }
        }

        vga_mem[y * cols + x] |= 0xDA;
        vga_mem[y * cols + x + w - 1] |= 0xBF;
        vga_mem[(y + h - 1) * cols + x] |= 0xC0;
        vga_mem[(y + h - 1) * cols + x + w - 1] |= 0xD9;
}


static void tui_win_draw(tui_window_t *w)
{
        for (int r = w->y; r < w->y + w->h; r++) {
                for (int c = w->x; c < w->x + w->w; c++) {
                        vga_mem[r * cols + c] = bg_color << 8;
                        if ((c > w->x && c < w->x + w->w - 1) &&
                            (r == w->y || r == w->y + w->h - 1)) {
                                vga_mem[r * cols + c] |= 205;
                        }
                        if ((r > w->y && r < w->y + w->h - 1) &&
                            (c == w->x || c == w->x + w->w - 1)) {
                                vga_mem[r * cols + c] |= 186;
                        }
                }
        }

        vga_mem[w->y * cols + w->x] |= 201;
        vga_mem[w->y * cols + w->x + w->w - 1] |= 187;
        vga_mem[(w->y + w->h - 1) * cols + w->x] |= 200;
        vga_mem[(w->y + w->h - 1) * cols + w->x + w->w - 1] |= 188;
}


int vga_printf(int x, int y, char *text)
{
        if (!text) return 0;
        int x0 = x;
        while (*text) {
                vga_mem[y * cols + x] |= *text;
                x++;
                text++;
        }
        return x - x0;
}


static void vga_color_invert(int x, int y, int w, int h)
{
        for (int r = y; r < y + h; r++) {
                for (int c = x; c < x + w; c++) {
                        vga_mem[r * cols + c] ^= 0x7700;
                }
        }
}


static void tui_menu_draw_selection(tui_menu_t *m)
{
        if (m->flags == MENU_ITEM || m->flags == MENU_POPUP) {
                printf("draw selection (%d,%d)x(%d,%d)\n", m->x, m->y, m->w, m->h);

                vga_color_invert(m->x, m->y, m->w, m->h);
        }
}


static void tui_menu_draw(tui_menu_t *m)
{
        if (m->flags == MENU_BAR) {
                for (int c = m->x; c < m->x + m->w - 1; c++) {
                        vga_mem[m->y * cols + c] = bg_color << 8;
                }

                int x = m->x + 2;
                for (int i = 0; i < m->nsub; i++) {
                        x += vga_printf(x, m->y, " ");
                        int xa = x + m->submenus[i]->accel;
                        x += vga_printf(x, m->y, m->submenus[i]->text);
                        x += vga_printf(x, m->y, " ");
                        x += 2;

                        if (m->state & MENU_STATE_OPEN) {
                                if (xa != -1) {
                                        vga_mem[m->y * cols + xa] &= 0xFF;
                                        vga_mem[m->y * cols + xa] |= a_color << 8;
                                }

                                /* current selection */
                                tui_menu_draw_selection(m->submenus[m->selection]);
                        }
                }
        }

        if (m->flags == MENU_POPUP) {
                /* get popup window for this popup menu */
                if (m->nsub == 0) {
                        return;
                }
                tui_menu_t *pw = m->submenus[0];
                printf("Drawing popup (%d, %d)x(%d, %d)\n", pw->x, pw->y, pw->w, pw->h);
                tui_win_draw_framed(pw->x, pw->y, pw->w, pw->h);


                /* this only gets called if open, so draw all children */
                for (int i = 0; i < pw->nsub; i++) {
                        tui_menu_draw(pw->submenus[i]);
                }

                tui_menu_draw_selection(pw->submenus[pw->selection]);

                return;
        }


        if (m->flags == MENU_ITEM) {
                int x = m->x;
                x += vga_printf(x, m->y, " ");
                int xa = x + m->accel;
                x += vga_printf(x, m->y, m->text);
                x += vga_printf(x, m->y, " ");
                x += 2;

                if (xa != -1) {
                        vga_mem[m->y * cols + xa] &= 0xFF;
                        vga_mem[m->y * cols + xa] |= a_color << 8;
                }
                return;
        }

        for (int i = 0; i < m->nsub; i++) {
                if (m->submenus[i]->state & MENU_STATE_OPEN) {
                        tui_menu_draw(m->submenus[i]);
                }
        }
}


void tui_menu_open(tui_menu_t *menu)
{
        menu->state |= MENU_STATE_OPEN;
}


void tui_menu_close(tui_menu_t *menu)
{
        menu->state &= ~MENU_STATE_OPEN;
}


void tui_draw(void)
{
        for (int i = 0; i < n_win; i++) {
                tui_win_draw(&windows[i]);
        }

        if (main_menu) {
                tui_menu_draw(main_menu);
        }
}


int tui_window_create(int x, int y, int w, int h, int flags, char *title)
{
        if (n_win == WIN_MAX) {
                return -1;
        }
        int new_win = n_win;

        if (title) {
                windows[new_win].title = strdup(title);
        }

        windows[new_win].x = x - 1;
        windows[new_win].y = y - 1;
        windows[new_win].w = w;
        windows[new_win].h = h;

        n_win++;

        return new_win;
}


void tui_menu_append(tui_menu_t *parent, tui_menu_t *child)
{
        if (parent->nsub == MENU_MAX) return;

        parent->submenus[parent->nsub] = child;
        parent->nsub++;
}
