#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
// #include <inline/exec_protos.h>
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
#include "smash_cmd.h"

struct MsgPort *mp;

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

#if 0
void
start(register struct DosPacket *start_pkt asm("%a0"))
//  printf("startup %x %x\n", start_pkt->dp_Arg1, start_pkt->dp_Arg2);
#endif
int
main(int argc, char *argv[])
{
    int arg;
    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary(DOSNAME, 0);

    for (arg = 1; arg < argc; arg++) {
        printf("%d: %s\n", arg, argv[arg]);
    }
    mp = CreatePort(NULL, 0);
    if (mp == NULL)
        goto createport_fail;

    DeletePort(mp);
createport_fail:
    CloseLibrary((struct Library *)DOSBase);
    return (0);
}
