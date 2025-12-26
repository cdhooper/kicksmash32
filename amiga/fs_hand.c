#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#include <fcntl.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <exec/ports.h>
#include <exec/nodes.h>
#include <exec/libraries.h>

#include <devices/timer.h>

#include <libraries/dos.h>
#include <libraries/dosextens.h>
#include <libraries/filehandler.h>
#include "printf.h"
#include "smash_cmd.h"
#include "cpu_control.h"
#include "host_cmd.h"
#include "sm_msg.h"
#include "sm_file.h"
#include "fs_hand.h"
#include "fs_vol.h"
#include "fs_timer.h"

#define DIRBUF_SIZE 2000

const char *version = "\0$VER: smashfs "VERSION" ("BUILD_DATE") \xA9 Chris Hooper";

BOOL __check_abort_enabled = 0;       // Disable gcc clib2 ^C break handling

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

uint    flag_debug = 0;
uint8_t flag_output = 1;
uint8_t sm_file_active = 0;
uint8_t grunning = 0;
uint8_t gvolumes_inuse = 0;
static uint runtime_max = 0;

static void
refresh_volume_list(void)
{
    static uint8_t fopen_fails = 0;
    handle_t       phandle = 0;
    handle_t       handle;
    uint           rc;
    uint           rlen;
    uint           type;
    uint           pos;
    uint           entlen;
    uint8_t       *data;
    char          *dname;
    hm_fdirent_t  *dent;

    if (sm_fservice() == 0) {
        volume_flush();
        return;  // file service is not active
    }

    rc = sm_fopen(phandle, "::", HM_MODE_READ, &type, 0, &handle);
    if (handle == 0) {
        printf("Could not open volume directory\n");
        if (fopen_fails++ >= 1)
            volume_flush();  // 2 strikes
        return;
    }
    fopen_fails = 0;
    while (1) {
        rc = sm_fread(handle, DIRBUF_SIZE, (void **) &data, &rlen, 0);
        if ((rlen == 0) && (rc != KM_STATUS_EOF)) {
            printf("Dir read failed: %s\n", smash_err(rc));
            goto gvl_fail;
        }
        printf("vols:");
        for (pos = 0; pos < rlen; ) {
            dent   = (hm_fdirent_t *)(((uintptr_t) data) + pos);
            entlen = dent->hmd_elen;
            if ((entlen < 2) || (entlen > 256))
                break;
            dname = (char *) (dent + 1);
            if (strlen(dname) < VOLNAME_MAXLEN) {
                uint vol_flags   = dent->hmd_ino;
                int  vol_bootpri = (int) dent->hmd_nlink;
                volume_seen(dname, dent->hmd_atime, vol_flags, vol_bootpri);
            }
            printf(" %s", (char *) (dent + 1));
            pos += sizeof (*dent) + entlen;
        }
        printf("\n");
        if (rc == KM_STATUS_EOF)
            break;  // End of directory reached
    }
gvl_fail:
    sm_fclose(handle);
    volume_flush();
}

void
handle_messages(void)
{
    ULONG waitmask = volume_msg_masks | timer_msg_mask | SIGBREAKF_CTRL_C;
    ULONG mask;
    uint  runtime = 0;
    uint  shutdown_timer = 15;
    static uint8_t do_refresh = 8;

//  printf("Volmasks=%08x Timermask=%08x\n", volume_msg_masks, timer_msg_mask);
    while (grunning || (gvolumes_inuse && (shutdown_timer != 0))) {
        mask = Wait(waitmask);
        if (mask & timer_msg_mask) {
            uint timer_msec = 1000;
            WaitIO(&timerio->tr_node);
            printf(".");
            if (grunning) {
                if ((runtime++ == runtime_max) && (runtime_max != 0)) {
                    printf("Runtime max %d\n", runtime_max);
                    grunning = 0;
                }
                if ((runtime & 7) == 0)
                    do_refresh++;
                if (do_refresh) {
                    /* To avoid deadlock, handlers never use LockDosList() */
                    struct DosList *dl = AttemptLockDosList(LDF_DEVICES |
                                                            LDF_VOLUMES |
                                                            LDF_WRITE);
                    if (dl != NULL) {
                        refresh_volume_list();
                        waitmask = volume_msg_masks | timer_msg_mask |
                                   SIGBREAKF_CTRL_C;
                        UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);
                        do_refresh = 0;
                    } else {
                        timer_msec = 1000 / TICKS_PER_SECOND;  // one tick
                    }
                }
            } else {
                if (shutdown_timer > 0) {
                    /* To avoid deadlock, handlers never use LockDosList() */
                    struct DosList *dl = AttemptLockDosList(LDF_DEVICES |
                                                            LDF_VOLUMES |
                                                            LDF_WRITE);
                    if (dl != NULL) {
                        volume_flush();
                        UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);
                        shutdown_timer--;
                        if (gvolumes_inuse)
                            printf("shutdown in %u\n", shutdown_timer);
                    } else {
                        timer_msec = 1000 / TICKS_PER_SECOND;  // one tick
                    }
                }
            }
            timer_restart(timer_msec);
        }
        if (mask & volume_msg_masks)
            volume_message(mask & volume_msg_masks);

        if (mask & SIGBREAKF_CTRL_C) {
            printf("Signal exit\n");
            grunning = 0;
        }
    }
}

void __chkabort(void) { }             // Disable gcc libnix ^C break handling

int
main(int argc, char *argv[])
{
    int arg;
    int rc = 0;
    uint8_t output_flag = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4;
#pragma GCC diagnostic pop
    DOSBase = (struct DosLibrary *)OpenLibrary(DOSNAME, 0);
    if (DOSBase == NULL) {
        printf("Failed to open %s\n", DOSNAME);
        return (1);
    }
    cpu_control_init();

    for (arg = 1; arg < argc; arg++) {
        const char *ptr = argv[arg];
        if (*ptr == '-') {
            for (++ptr; *ptr != '\0'; ptr++) {
                switch (*ptr) {
                    case 't':
                        runtime_max = 240; // limit to 4 minutes runtime
                        break;
                    case 'd':  // Show debug output
                        output_flag++;
                        break;
                    case 'h':  // Show help (usage)
                        goto show_usage;
                    case 'q':  // Quiet (no debug output)
                        output_flag = 0;
                        break;
                    case 'v':  // Show version
                        printf("%s\n", version + 7);
                        goto go_exit;
                    default:
                        printf("Unknown -%s\n", ptr);
show_usage:
                        printf("-d - debug output (-dd = serial debug))\n"
                               "-h - display this help text\n"
                               "-t - limit runtime to 240 minutes\n"
                               "-v - show smashfs version\n"
                               "");
                        rc = 1;
                        goto go_exit;
                }
            }
        } else {
            printf("Unknown argument %s\n", argv[arg]);
            goto show_usage;
        }
    }
    flag_output = output_flag;
    printf("\n%s\n", version + 7);

    grunning = 1;
    timer_open();
    refresh_volume_list();
    timer_restart(1000);

    handle_messages();

    LockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);
        volume_close();
        volume_flush();
    UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);

    timer_close();
    printf("smashfs exit\n");
go_exit:
    CloseLibrary((struct Library *)DOSBase);
    return (rc);
}
