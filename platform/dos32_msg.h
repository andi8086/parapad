#ifndef DOS32_MSG_H
#define DOS32_MSG_H


#define DM_QUIT              66
#define DM_MENU_OPEN         100
#define DM_MENU_CLOSE        101
#define DM_MENU_OPEN_POPUP   102
#define DM_KEY_DOWN          10
#define DM_KEY_UP            11
#define DM_REDRAW            12
#define DM_COMMAND           13

struct dos_msg {
        int msg;
        int iarg1;
        int iarg2;
        void *parg1;
        void *parg2;
};


typedef struct dos_msg dos_msg_t;

int dos_msg_push(int msg, int iarg1, int iarg2, void *parg1, void *parg2);
int dos_msg_pop(dos_msg_t *msg);
void msg_isr_enter(void);
void msg_isr_leave(void);


#endif
