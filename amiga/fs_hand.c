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
#include "fs_packet.h"

#define DIRBUF_SIZE 2000

#ifdef ROMFS
#define REFRESH_VOLUME_LIST() refresh_volume_list(0)
struct DosPacket *rom_startup_pkt;  // Set if DOS started smashfs (fs_rom.c)
static struct MsgPort *reject_port;  // Boot node port while volume is absent
static ULONG reject_mask;
void smashboot_add(const char *name, int bootpri);
#else
#define REFRESH_VOLUME_LIST() refresh_volume_list()
#define reject_mask 0
#endif
#define SHUTDOWN_MAX_SECS 60  // Max wait at exit for locks to be released

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
static uint8_t do_refresh = 8;  // Non-zero: refresh volume list next tick

/*
 * volume_refresh_soon
 * -------------------
 * Requests a volume list refresh at the next timer tick. This is used
 * when the host reports a stale handle, so that a hostsmash restart is
 * detected (see check_host_epoch()) and volume handles are reopened.
 */
void
volume_refresh_soon(void)
{
    do_refresh = 1;
}

/*
 * check_host_epoch
 * ----------------
 * The host reports a value in Volume Directory entries which changes
 * every time hostsmash is started (0 for hosts which predate this).
 * A change means all handles from the previous host are invalid:
 * reopen the volume handles. Locks and files opened earlier remain
 * stale; the host refuses them with KM_STATUS_STALE.
 */
static void
check_host_epoch(uint32_t epoch)
{
    static uint32_t prev_epoch;
    static uint8_t  prev_valid = 0;

    if (prev_valid && (epoch != prev_epoch)) {
        printf("Host restarted (epoch %x -> %x)\n", prev_epoch, epoch);
        volume_reopen_handles();
    }
    prev_epoch = epoch;
    prev_valid = 1;
}

/*
 * refresh_volume_list
 * -------------------
 * Reads the host's Volume Directory, adding new volumes to the DOS list
 * and flushing those which are gone. With boot_scan set (smashboot ROM
 * module, before DOS starts), it instead creates a boot node for each
 * volume the host reports as bootable.
 *
 * The host is read without holding the DosList lock (see volume_update()).
 * Returns -1 if the DosList lock was not available, in which case the
 * refresh should be tried again soon.
 */
#ifdef ROMFS
int
refresh_volume_list(uint boot_scan)
#else
static int
refresh_volume_list(void)
#endif
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
    uint8_t        epoch_checked = 0;

    if (sm_fservice() == 0) {
#ifdef ROMFS
        if (boot_scan)
            return (0);  // Before DOS: there is no DOS list
#endif
        return (volume_update());  // file service is not active
    }

    rc = sm_fopen(phandle, "::", HM_MODE_READ, &type, 0, &handle);
    if (handle == 0) {
        printf("Could not open volume directory\n");
#ifdef ROMFS
        if (boot_scan)
            return (0);
#endif
        if (fopen_fails++ >= 1)
            return (volume_update());  // 2 strikes
        return (0);
    }
    fopen_fails = 0;
    while (1) {
        rc = sm_fread(handle, DIRBUF_SIZE, (void **) &data, &rlen, 0);
        if ((rlen == 0) && (rc != KM_STATUS_EOF)) {
            printf("Dir read failed: %s\n", smash_err(rc));
            goto gvl_fail;
        }
        printf("vols:");
        for (pos = 0; pos + sizeof (*dent) <= rlen; ) {
            dent   = (hm_fdirent_t *)(((uintptr_t) data) + pos);
            entlen = dent->hmd_elen;
            if ((entlen < 2) || (entlen > 256) ||
                (pos + sizeof (*dent) + entlen > rlen))
                break;
            dname = (char *) (dent + 1);
            if (memchr(dname, '\0', entlen) == NULL)
                break;  // Name not terminated within entry
            if (!epoch_checked) {
                /* Before volume_seen(), which may use volume handles */
                epoch_checked = 1;
                check_host_epoch(dent->hmd_rdev);
            }
            if (strlen(dname) < VOLNAME_MAXLEN) {
#ifdef ROMFS
                if (boot_scan) {
                    if (dent->hmd_ino & AV_FLAG_BOOTABLE)
                        smashboot_add(dname, (int32_t) dent->hmd_nlink);
                } else
#endif
                    volume_seen(dname, dent->hmd_atime);
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
#ifdef ROMFS
    if (boot_scan)
        return (0);
#endif
    return (volume_update());
}

void
handle_messages(void)
{
    ULONG waitmask = volume_msg_masks | timer_msg_mask | SIGBREAKF_CTRL_C;
    ULONG mask;
    uint  runtime = 0;
    uint  shutdown_timer = SHUTDOWN_MAX_SECS;
    uint8_t done = 0;

//  printf("Volmasks=%08x Timermask=%08x\n", volume_msg_masks, timer_msg_mask);
    /*
     * After a shutdown request (CTRL-C, ACTION_DIE, or runtime limit),
     * packets which release locks and files continue to be handled while
     * all others are rejected. This continues until no volume is in use,
     * or until SHUTDOWN_MAX_SECS have elapsed, at which point remaining
     * volumes are abandoned (see volume_abandon()).
     */
    while (!done) {
        /* volume_seen() and volume_update() add and delete volume ports */
        waitmask = volume_msg_masks | timer_msg_mask | SIGBREAKF_CTRL_C |
                   reject_mask;
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
                    if (REFRESH_VOLUME_LIST() == 0)
                        do_refresh = 0;
                    else
                        timer_msec = 1000 / TICKS_PER_SECOND;  // one tick
                }
            } else if (volume_update() != 0) {
                /* DosList is busy; volume_update() frees unused volumes */
                timer_msec = 1000 / TICKS_PER_SECOND;  // one tick
            } else if (gvolumes_inuse == 0) {
                done = 1;
            } else if (--shutdown_timer == 0) {
                /* To avoid deadlock, handlers never use LockDosList() */
                struct DosList *dl = AttemptLockDosList(LDF_DEVICES |
                                                        LDF_VOLUMES |
                                                        LDF_WRITE);
                if (dl != NULL) {
                    volume_abandon();
                    UnLockDosList(LDF_DEVICES | LDF_VOLUMES | LDF_WRITE);
                    done = 1;
                } else {
                    shutdown_timer = 1;  // Try again
                    timer_msec = 1000 / TICKS_PER_SECOND;  // one tick
                }
            } else if ((shutdown_timer % 10) == 0) {
                printf("%u volumes in use; shutdown in %u\n",
                       gvolumes_inuse, shutdown_timer);
            }
            if (!done)
                timer_restart(timer_msec);
        }
        if ((mask & volume_msg_masks) && !done)
            volume_message(mask & volume_msg_masks);
#ifdef ROMFS
        if (mask & reject_mask)
            volume_port_reject(reject_port);
#endif

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
                               "-t - limit runtime to 240 seconds\n"
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
    volume_events_open();
    if (timer_msg_mask == 0) {
        /* Without the timer, volumes are never refreshed or shut down */
        printf("smashfs: unable to open timer\n");
        rc = 1;
        goto go_exit;
    }
#ifdef ROMFS
    if (rom_startup_pkt != NULL) {
        /*
         * DOS started smashfs as the handler of a boot device node, and
         * may hold the DosList lock until the startup packet is replied.
         * Attach the boot volume now; the timer refresh adds the rest.
         */
        struct DosPacket  *pkt = rom_startup_pkt;
        struct DeviceNode *dn  = (struct DeviceNode *) BTOC(pkt->dp_Arg3);
        volume_boot_attach(dn);
        rom_startup_pkt = NULL;
        if (dn->dn_Task == NULL) {
            /*
             * The host volume is not available. Kickstart 3.2 boot code
             * sends packets to dn_Task even after a failed startup, so it
             * must not be left NULL. Point it at this process's port,
             * where every packet is rejected with ERROR_DEVICE_NOT_MOUNTED,
             * until volume_update() adopts the node when the volume appears.
             */
            reject_port = &((struct Process *) FindTask(NULL))->pr_MsgPort;
            reject_mask = BIT(reject_port->mp_SigBit);
            dn->dn_Task = reject_port;
        }
        ReplyPkt(pkt, DOSTRUE, 0);
        timer_restart(1000 / TICKS_PER_SECOND);  // Refresh soon
    } else
#endif
    {
        if (REFRESH_VOLUME_LIST() == 0) {
            timer_restart(1000);
        } else {
            do_refresh = 1;  // DosList busy; try again soon
            timer_restart(1000 / TICKS_PER_SECOND);
        }
    }

    /* Returns after all volumes have been freed or abandoned */
    handle_messages();

    timer_close();
    fs_packet_cleanup();
    printf("smashfs exit\n");
go_exit:
#ifdef ROMFS
    if (rom_startup_pkt != NULL)  // DOS is waiting for the handler startup
        ReplyPkt(rom_startup_pkt, DOSFALSE, ERROR_NO_FREE_STORE);
#endif
    volume_events_close();
    sm_file_exit();
    host_msg_exit();  // I don't know why atexit() doesn't work
    CloseLibrary((struct Library *)DOSBase);
    return (rc);
}
