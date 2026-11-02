#include "dos32_tui.h"

#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <conio.h>
#include <dos.h>
#include <graph.h>

#include "dos32_msg.h"


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
        int id;
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
                         int flags, int id)
{
        tui_menu_t *m;

        m = malloc(sizeof(tui_menu_t));
        m->id = id;
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


#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5


void write_crtc(unsigned char reg, unsigned char val)
{


}


static void vga_init_80x25(void)
{
        union REGS regs;
        regs.w.ax = 3;
        int386(0x10, &regs, &regs);
}


static void vga_init_100x25(void)
{
        union REGS regs;

        regs.w.ax = 3;
        int386(0x10, &regs, &regs);

        outp(0x3C4, 1);
        unsigned char seq1 = inp(0x3C5);
        outp(0x3C5, seq1 | 1);  // 8-bit character width

        outp(CRTC_INDEX, 0x11);
        unsigned crtc11 = inp(0x3D5);
        outp(0x3D5, crtc11 & 0x7F); // unlock crtc

        write_crtc(0x00, 119); // horizontal total
        write_crtc(0x01, 99);  // visible chars - 1
        write_crtc(0x02, 100); // blanking start
        write_crtc(0x03, 0x28);// blanking end
        write_crtc(0x04, 105); // sync start
        write_crtc(0x05, 0x02); // sync end
        write_crtc(0x13, 50); // offset (mem width in words)

        outp(CRTC_INDEX, 0x11);
        outp(0x3D5, crtc11 | 0x80); // lock crtc
}

#define VESA_132x43 0x10A

int set_extended_text_mode(unsigned short mode)
{
        union REGS regs;

        regs.w.ax = 0x4F02;
        regs.w.bx = mode;
        int386(0x10, &regs, &regs);

        return (regs.h.al == 0x4F);
}



void tui_init(int c, int r)
{
        if (c == 132 && r == 43) {
                if (!set_extended_text_mode(VESA_132x43)) {
                        c = 80; r = 25;
                }
        }

        cols = c;
        rows = r;
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
        vga_init_80x25();
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
                // printf("draw selection (%d,%d)x(%d,%d)\n", m->x, m->y, m->w, m->h);
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
                // printf("Drawing popup (%d, %d)x(%d, %d)\n", pw->x, pw->y, pw->w, pw->h);
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


static tui_menu_t *menu_stack[MENU_MAX];
static volatile int current_menu = 0;


int tui_current_menu(void)
{
        return current_menu;
}


int tui_menu_handle_enter(void)
{
        if (!current_menu) {
                return 0;
        }
        /* we only allow it for a popup menu */
        tui_menu_t *m = menu_stack[current_menu - 1];
        if (m->flags != MENU_POPUP || m->submenus[0] == NULL) {
                return 0;
        }
        int signal = m->submenus[0]->submenus[m->submenus[0]->selection]->signal;
        dos_msg_push(DM_COMMAND, signal, 0, NULL, NULL);
        return 1;
}


int tui_menu_handle_down(void)
{
        if (!current_menu) {
                return 0;
        }
        /* we only allow it for a popup menu */
        tui_menu_t *m = menu_stack[current_menu - 1];
        if (m->flags != MENU_POPUP || m->submenus[0] == NULL) {
                return 0;
        }
        m->submenus[0]->selection += 1;
        m->submenus[0]->selection %= m->submenus[0]->nsub;
        return 1;
}


int tui_menu_handle_right(void)
{
        if (!current_menu) {
                return 0;
        }
        // TODO: if current selection item has a popup menu,
        // open it
        tui_menu_t *m = menu_stack[current_menu - 1];
        if (m->flags == MENU_POPUP) {
                tui_menu_close(m);
        }
        if (!current_menu) {
                return 1;
        }
        m = menu_stack[current_menu - 1];
        if (m->flags == MENU_BAR) {
                m->selection++;
                m->selection %= m->nsub;
                tui_menu_open(m->submenus[m->selection]);
                return 1;
        }

        return 0;
}


int tui_menu_handle_left(void)
{
        if (!current_menu) {
                return 0;
        }
        tui_menu_t *m = menu_stack[current_menu - 1];
        if (m->flags == MENU_POPUP) {
                tui_menu_close(m);
        }
        if (!current_menu) {
                return 1;
        }
        m = menu_stack[current_menu - 1];
        if (m->flags == MENU_BAR) {
                m->selection--;
                m->selection += m->nsub;
                m->selection %= m->nsub;
                tui_menu_open(m->submenus[m->selection]);
                return 1;
        }
        return 0;
}

int tui_menu_handle_up(void)
{
        if (!current_menu) {
                return 0;
        }
        /* we only allow it for a popup menu */
        tui_menu_t *m = menu_stack[current_menu - 1];
        if (m->flags != MENU_POPUP || m->submenus[0] == NULL) {
                return 0;
        }
        m->submenus[0]->selection -= 1 - m->submenus[0]->nsub;
        m->submenus[0]->selection %= m->submenus[0]->nsub;
        return 1;
}


void tui_menu_open(tui_menu_t *menu)
{
        // only menu bar and menu popup can be opened
        if (menu->flags == MENU_BAR || menu->flags == MENU_POPUP) {
                menu->state |= MENU_STATE_OPEN;
                menu_stack[current_menu] = menu;
                current_menu++;
        }
}


void tui_menu_close(tui_menu_t *menu)
{
        menu->state &= ~MENU_STATE_OPEN;
        /* find the parent menu in the menu stack */
        if (menu->flags == MENU_BAR || menu->flags == MENU_POPUP) {
                for (int i = current_menu - 1; i >= 0; i--) {
                        if (menu_stack[i] == menu) {
                                current_menu = i;
                                break;
                        } else {
                                if (i == 0) {
                                        printf("tui_menu_close: Menu wasn't active\n");
                                        break;
                                }
                        }
                }
        }

        /* close all children */
        for (int i = 0; i < menu->nsub; i++) {
                tui_menu_close(menu->submenus[i]);
        }
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


static tui_menu_t *menu_item_find_sub(tui_menu_t *m, int id)
{
        for (int i = 0; i < m->nsub; i++) {
                if (m->submenus[i]->id == id) {
                        return m->submenus[i];
                } else {
                        tui_menu_t *s =
                            menu_item_find_sub(m->submenus[i], id);
                        if (s) {
                                return s;
                        }
                }
        }
        return NULL;
}


static tui_menu_t *menu_item_find(int id)
{
        tui_menu_t *m = main_menu;
        if (!m) {
                return NULL;
        }

        if (m->id == id) {
                return m;
        }

        return menu_item_find_sub(m, id);
}


static void dm_menu_open(int id)
{
        tui_menu_t *m = menu_item_find(id);
        if (!m) {
                printf("dm_open: menu not found\n");
                return;
        }
        tui_menu_open(m);
}


static void dm_menu_close(int id)
{
        tui_menu_t *m = menu_item_find(id);
        if (!m) {
                printf("dm_close: menu not found\n");
                return;
        }
        tui_menu_close(m);
}


static void dm_menu_open_popup(int menu_id, int popup_id)
{
        tui_menu_t *m = menu_item_find(menu_id);
        if (!m) {
                printf("dm_menu_open_poup: menu not found\n");
                return;
        }
        if (popup_id == MENU_SELECTION_CURRENT) {
                popup_id = m->selection;
        }
        if (popup_id >= m->nsub) {
                printf("dm_menu_open_popup: popup out of range\n");
                return;
        }
        tui_menu_t *p = m->submenus[popup_id];
        if (!p) {
                printf("dm_menu_open_poup: popup menu is NULL\n");
                return;
        }
        tui_menu_open(p);
}


typedef void __interrupt (*isr_ptr_t)();

isr_ptr_t old_keyboard_isr;

static volatile unsigned char key_states[128] = {0};
static volatile int last_scancode = 0;
static int is_extended = 0;

void __interrupt new_keyboard_isr()
{
        msg_isr_enter();
        unsigned int scancode;

        scancode = inp(0x60);
        last_scancode = scancode;

        if (scancode == 0xE0) {
                is_extended = 1;
                goto isr_return;
        }

        if (scancode & 0x80) {
                scancode &= 0x7F;
                key_states[scancode] = 0;

                if (is_extended) {
                        scancode += 1000;
                        is_extended = 0;
                }
                dos_msg_push(DM_KEY_UP, scancode, 0, NULL, NULL);
        } else {
                key_states[scancode] = 1;
                if (is_extended) {
                        scancode += 1000;
                        is_extended = 0;
                }
                dos_msg_push(DM_KEY_DOWN, scancode, 0, NULL, NULL);
        }
isr_return:
        outp(0x20, 0x20);
        msg_isr_leave();
}


void tui_run(msg_handler_t msg_func)
{
        int quit = 0;
        dos_msg_t msg;

        old_keyboard_isr = (isr_ptr_t)_dos_getvect(0x09);

        _dos_setvect(0x09, new_keyboard_isr);
        while (!quit) {
                if (!dos_msg_pop(&msg)) {
                        continue;
                }
                if (msg_func(msg.msg, msg.iarg1, msg.iarg2, msg.parg1, msg.parg2)) {
                        /* message already handled */
                        continue;
                }
                switch (msg.msg) {
                case DM_QUIT:
                        quit = 1;
                        break;
                case DM_MENU_OPEN:
                        // iarg1 = ID of menu
                        dm_menu_open(msg.iarg1);
                        dos_msg_push(DM_REDRAW, 0, 0, NULL, NULL);
                        break;
                case DM_MENU_OPEN_POPUP:
                        // iarg1 = ID of menu, iarg2 = child index of popup (-1 = current)
                        dm_menu_open_popup(msg.iarg1, msg.iarg2);
                        dos_msg_push(DM_REDRAW, 0, 0, NULL, NULL);
                        break;
                case DM_MENU_CLOSE:
                        // iarg1 = ID of menu
                        dm_menu_close(msg.iarg1);
                        dos_msg_push(DM_REDRAW, 0, 0, NULL, NULL);
                        break;
                case DM_REDRAW:
                        tui_draw();
                        break;
                default:
                        break;
                }
        }

        _dos_setvect(0x09, old_keyboard_isr);
}


void tui_get_screen_size(int *c, int *r)
{
        *c = cols;
        *r = rows;
}
