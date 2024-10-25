#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#include <devices/timer.h>

#include <inline/exec.h>
#include <inline/dos.h>
extern struct ExecBase *SysBase;

#include "printf.h"
#include "fs_hand.h"
#include "fs_timer.h"

static struct MsgPort  *timerport;
struct timerequest     *timerio;
static uint8_t          timer_running = 0;
ULONG                   timer_msg_mask = 0;

void
timer_close(void)
{
    if (timer_running) {
        printf("Timer wait finish\n");
        WaitIO(&timerio->tr_node);
        timer_running = 0;
    }

    if (timerio != NULL) {
        CloseDevice(&timerio->tr_node);
        DeleteExtIO(&timerio->tr_node);
        timerio = NULL;
    }

    if (timerport != NULL) {
        DeletePort(timerport);
        timerport = NULL;
    }
    timer_msg_mask = 0;
    printf("Timer closed\n");
}

void
timer_open(void)
{
    int rc;

    if (timerport != NULL) {
        printf("Attempted to re-open timer\n");
        return;
    }

    timerport = CreatePort(NULL, 0);
    if (timerport == NULL) {
        printf("Can't create timer port\n");
        timer_close();
        return;
    }

    timerio = (struct timerequest *)
                CreateExtIO(timerport, sizeof (struct timerequest));
    if (timerio == NULL) {
        printf("Failed to alloc timerio\n");
        timer_close();
        return;
    }
    rc = OpenDevice(TIMERNAME, UNIT_VBLANK, &timerio->tr_node, 0);
    if (rc != 0) {
        printf("Failed to open timer device\n");
        timer_close();
        return;
    }
    timer_msg_mask = BIT(timerport->mp_SigBit);
    printf("Timer opened\n");
}

void
timer_restart(uint msec)
{
    if (timerio != NULL) {
        timerio->tr_time.tv_secs  = msec / 1000;
        timerio->tr_time.tv_micro = (msec % 1000) * 1000;
        timerio->tr_node.io_Command = TR_ADDREQUEST;
        SendIO(&timerio->tr_node);
        timer_running = 1;
    }
}
