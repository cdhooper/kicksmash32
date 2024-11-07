/*
 * $Id: amiga_createtask.c,v 1.6 2006-09-25 15:12:47 obarthel Exp $
 *
 * :ts=4
 *
 * Portable ISO 'C' (1994) runtime library for the Amiga computer
 * Copyright (c) 2002-2015 by Olaf Barthel <obarthel (at) gmx.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   - Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   - Neither the name of Olaf Barthel nor the names of contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/****************************************************************************/

#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/tasks.h>

#include <string.h>

/****************************************************************************/

#include <proto/exec.h>
#include <clib/alib_protos.h>

/****************************************************************************/

// #ifndef _STDLIB_PROFILE_H
// #include "stdlib_profile.h"
// #endif /* _STDLIB_PROFILE_H */

/****************************************************************************/

// #include "macros.h"
// #include "debug.h"

/****************************************************************************/

/*
 *  Create a task with given name, priority, and stack size.
 *  It will use the default exception and trap handlers for now.
 */

/****************************************************************************/

/* the template for the mementries.  Unfortunately, this is hard to
 * do from C: mementries have unions, and they cannot be statically
 * initialized...
 *
 * In the interest of simplicity I recreate the mem entry structures
 * here with appropriate sizes.  We will copy this to a local
 * variable and set the stack size to what the user specified,
 * then attempt to actually allocate the memory.
 */

#define ME_TASK 	0
#define ME_STACK	1

#define NUM_MEM_ENTRIES	2

/****************************************************************************/

struct FakeMemEntry
{
	ULONG fme_Reqs;
	ULONG fme_Size;
};

/****************************************************************************/

struct FakeMemList
{
	struct Node			fml_Node;
	UWORD				fml_NumEntries;
	struct FakeMemEntry	fml_ME[NUM_MEM_ENTRIES];
};

/****************************************************************************/

#define ENTER()
#define SHOWSTRING(x)
#define SHOWVALUE(x)
#define SHOWPOINTER(x)
#define SHOWMSG(x)
#define PROFILE_OFF()
#define PROFILE_ON()
#define RETURN(x)
#define assert(x)

struct Task *
my_CreateTask(CONST_STRPTR name,LONG pri,CONST APTR init_pc,ULONG stack_size, void *userdata)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
        struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
	struct Task * new_task;
	struct FakeMemList fake_mem_list;
	struct MemList * ml = NULL;
	APTR result = NULL;

	ENTER();

	SHOWSTRING(name);
	SHOWVALUE(pri);
	SHOWPOINTER(init_pc);
	SHOWVALUE(stack_size);

	assert( name != NULL && (-128 <= pri && pri <= 127) && init_pc != NULL && stack_size > 0 );

	if(name == NULL || pri < -128 || pri > 127 || init_pc == NULL || stack_size == 0)
	{
		SHOWMSG("invalid parameters");
		goto out;
	}

	/* round the stack up to longwords... */
	stack_size = (stack_size + 3UL) & ~3UL;

	/*
	 * This will allocate two chunks of memory: task of PUBLIC
	 * and stack of PRIVATE
	 */
	memset(&fake_mem_list,0,sizeof(fake_mem_list));

	fake_mem_list.fml_NumEntries			= NUM_MEM_ENTRIES;
	fake_mem_list.fml_ME[ME_TASK].fme_Reqs	= MEMF_PUBLIC | MEMF_CLEAR;
	fake_mem_list.fml_ME[ME_TASK].fme_Size	= sizeof(struct Task);
	fake_mem_list.fml_ME[ME_STACK].fme_Reqs	= MEMF_ANY | MEMF_CLEAR;
	fake_mem_list.fml_ME[ME_STACK].fme_Size	= stack_size;

	ml = (struct MemList *)AllocEntry((struct MemList *)&fake_mem_list);

	/* Did the allocation succeed? */
	if(((LONG)ml) < 0)
	{
		SHOWMSG("memory allocation failed");

		/* Note: if AllocEntry() fails, the entire allocation is
		 *       released before the function returns with bit #31
		 *       set and the number of the slot that failed being
		 *       returned. Thus, the return value is not a valid
		 *       address that would need to be freed.
		 */
		ml = NULL;
		goto out;
	}

	/* set the stack accounting stuff */
	new_task = (struct Task *)ml->ml_ME[ME_TASK].me_Addr;

	new_task->tc_SPLower	= ml->ml_ME[ME_STACK].me_Addr;
	new_task->tc_SPUpper	= (APTR)((ULONG)(new_task->tc_SPLower) + stack_size);
	new_task->tc_SPReg		= new_task->tc_SPUpper;
        new_task->tc_UserData		= userdata;

	/* misc task data structures */
	new_task->tc_Node.ln_Type	= NT_TASK;
	new_task->tc_Node.ln_Pri	= pri;
	new_task->tc_Node.ln_Name	= (char *)name;


	/* add it to the tasks memory list */
	NewList(&new_task->tc_MemEntry);
	AddHead(&new_task->tc_MemEntry,(struct Node *)ml);

	/* add the task to the system -- use the default final PC */

	PROFILE_OFF();
	result = AddTask(new_task,init_pc,NULL);
	PROFILE_ON();

	if(result == NULL)
	{
		SHOWMSG("could not add task");
		goto out;
	}

	/* Gobbled up by task. */
	ml = NULL;

 out:

	if(ml != NULL)
		FreeEntry(ml);

	RETURN(result);
	return(result);
}
