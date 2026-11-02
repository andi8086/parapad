#include "dos32_msg.h"

#include <string.h>


#define MSG_MAX 16


static dos_msg_t msg_stack[MSG_MAX];
int n_msgs = 0;

static volatile int locked = 0;
static volatile int in_interrupt = 0;

void msg_isr_enter(void)
{
        in_interrupt = 1;
}

void msg_isr_leave(void)
{
        in_interrupt = 0;
}


static void msg_lock(void)
{
        if (!in_interrupt) {
                _asm "cli";
        }
}


static void msg_unlock(void)
{
        if (!in_interrupt) {
                _asm "sti";
        }
}


int dos_msg_push(int msg, int iarg1, int iarg2, void *parg1, void *parg2)
{
        msg_lock();
        if (n_msgs >= MSG_MAX) {
                msg_unlock();
                return 0;
        }
        msg_stack[n_msgs].msg = msg;
        msg_stack[n_msgs].iarg1 = iarg1;
        msg_stack[n_msgs].iarg2 = iarg2;
        msg_stack[n_msgs].parg1 = parg1;
        msg_stack[n_msgs].parg2 = parg2;
        n_msgs++;
        msg_unlock();
        return 1;
}


int dos_msg_pop(dos_msg_t *msg)
{
        msg_lock();
        if (n_msgs == 0) {
                msg_unlock();
                return 0;
        }

        memcpy(msg, &msg_stack[0], sizeof(dos_msg_t));

        for (int i = 1; i < n_msgs; i++) {
                memcpy(&msg_stack[i-1], &msg_stack[i], sizeof(dos_msg_t));
        }
        n_msgs--;

        msg_unlock();
        return 1;
}
