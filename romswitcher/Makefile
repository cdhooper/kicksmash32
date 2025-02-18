#
# Makefile to build AmigaOS/68k tools using Bebbo's GCC cross-compiler.
#

PROGROM	 := switcher.rom
PROG     := switcher

OBJDIR   := objs
AM	 := ../amiga
FW	 := ../fw
MED_SRCS := med_cmds.c mem_access.c med_cmdline.c med_readline.c
SRCS     := main.c autoconfig.c cache.c keyboard.c pcmds.c \
            printf.c reset.c scanf.c screen.c serial.c \
            sprite.c strtoll.c strtoull.c timer.c util.c blitter.c \
	    vectors.c mouse.c draw.c intuition.c gadget.c \
	    testdraw.c testgadget.c audio.c $(MED_SRCS) \
	    $(AM)/romswitch.c $(AM)/cpu_control.c $(AM)/sm_msg.c \
	    $(AM)/sm_msg_core.c
ADDOBJS  :=
#ADDOBJS  := $(OBJDIR)/audio0.o

#OBJS    := $(SRCS:%.c=$(OBJDIR)/%.o)
MED_OBJS := $(MED_SRCS:%.c=$(OBJDIR)/%.o)

CC      := m68k-amigaos-gcc
LD      := m68k-amigaos-ld
OBJCOPY := m68k-amigaos-objcopy
OBJDUMP := m68k-amigaos-objdump
DD	:= dd
CFLAGS  := -Wall -Wextra -Wno-pointer-sign -Wno-format -Wno-strict-aliasing
CFLAGS  += -Wno-sign-compare -fomit-frame-pointer
CFLAGS  += -DROMFS -DSTANDALONE -DEMBEDDED_CMD -fbaserel -resident -mcpu=68060
CFLAGS  += -DRAM_BASE=0x00000000
CFLAGS  += -I. -I$(FW) -I$(AM)
#CFLAGS  += -DRAM_BASE=0x07e00000
#LDFLAGS := -nostartfiles -nodefaultlibs -nostdlib
#LDFLAGS += -lgcc -lc -lamiga -Xlinker --verbose -Trom.ld -mcrt=clib2 -fbaserel
LDFLAGS := --nostdlib \
	   --amiga-databss-together \
	   -L/opt/amiga13/lib \
	   -L/opt/amiga13/lib/gcc \
	   -L/opt/amiga13/lib/gcc/m68k-amigaos/13.2.0 \
	   -L/opt/amiga13/lib/gcc/m68k-amigaos/13.2.0/libb \
	   -L/opt/amiga13/m68k-amigaos/lib \
	   -L/opt/amiga13/m68k-amigaos/lib/libb \

#	   -L/opt/amiga13/m68k-amigaos/clib2/lib \
#	   -L/opt/amiga13/m68k-amigaos/clib2/lib/libb
LDFLAGS += -lgcc -lc --verbose
#	   -L/opt/amiga13/m68k-amigaos/libnix/lib \
#	   -L/opt/amiga13/m68k-amigaos/libnix/lib/libb
#LDFLAGS += -lgcc -lnix4 --verbose

# const char RomID[] = "ROM MED 0.1 (2025-01-09)\n";
PROGVER := $(PROG)_$(shell awk '/const char RomID/{print $$7}' main.c)

NDK_PATH := /opt/amiga/m68k-amigaos/ndk-include
VASM     := vasmm68k_mot

CFLAGS  += -Os

PROG     := switcher

DEHUNK   := $(OBJDIR)/dehunk

# Enable to completely turn off debug output (smashfs is about 5K smaller)
#CFLAGS += -NO_DEBUG

#LDFLAGS_FTP += -g
#LDFLAGS += -g
#CFLAGS  += -g

NOW  := $(shell date +%s)
ifeq ($(OS),Darwin)
DATE := $(shell date -j -f %s $(NOW)  '+%Y-%m-%d')
TIME := $(shell date -j -f %s $(NOW)  '+%H:%M:%S')
else
DATE := $(shell date -d "@$(NOW)" '+%Y-%m-%d')
TIME := $(shell date -d "@$(NOW)" '+%H:%M:%S')
endif
CFLAGS += -DBUILD_DATE=\"$(DATE)\" -DBUILD_TIME=\"$(TIME)\"

QUIET   := @
VERBOSE :=

# If verbose is specified with no other targets, then build everything
ifeq ($(MAKECMDGOALS),verbose)
verbose: all
endif
ifeq (,$(filter verbose timed, $(MAKECMDGOALS)))
QUIET   := @
else
QUIET   :=
VERBOSE := -v
endif

ifeq (, $(shell which $(CC) 2>/dev/null ))
$(error "No $(CC) in PATH: maybe do PATH=$$PATH:/opt/amiga13/bin")
endif

all: $(PROGROM)
	@:

dis:
	$(OBJDUMP) -b binary -D $(PROGROM) --adjust-vma=0x00f80000 | less

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(2)/%,$(filter-out $(2)/%,$(basename $(notdir $(1))).o)) $(filter $(2)/%,$(basename $(1)).o): $(1)
# The following line adds that object to macro containing the list of objects
$(3) += $(patsubst %,$(2)/%,$(filter-out $(2)/%,$(basename $(notdir $(1))).o))
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(OBJDIR),OBJS)))

#$(info $(patsubst %,$(2)/%,$(filter-out $(2)/%,$(basename $(notdir $(1))).o)) $(filter $(2)/%,$(basename $(1)).o): $(1))
#$(info OBJS = $(OBJS))


$(OBJS): amiga_chipset.h screen.h printf.h util.h serial.h timer.h
$(OBJDIR)/serial.o: keyboard.h
$(OBJDIR)/sprite.o: sprite.h
$(OBJDIR)/main.o: sprite.h keyboard.h reset.h
$(OBJDIR)/draw.o: draw.h intuition.h
$(OBJDIR)/intuition.o: draw.h intuition.h
$(OBJDIR)/sm_msg.o $(OBJDIR)/sm_msg_core.o: $(AM)/cpu_control.h $(AM)/sm_msg.h $(FW)/smash_cmd.h $(AM)/host_cmd.h

$(MED_OBJS): med_cmdline.h med_cmds.h med_main.h med_readline.h pcmds.h
$(MED_OBJS):: CFLAGS += -Wno-unused-parameter

$(OBJS): Makefile | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -Wa,-a,-ad >$(@:.o=.lst) -o $@

#$(OBJDIR)/audio0.o: audio0.wav
#	@echo Creating $@
#	$(QUIET)ffmpeg -y -i $^ -f s8 -c:a pcm_s8 -ac 1 -ar 8000 $@.raw 2> /dev/null
#	$(QUIET)$(OBJCOPY) -I binary -O amiga $@.raw $@.t.o $(VERBOSE)
#	$(QUIET)$(OBJCOPY) --rename-section .data=.audio $@.t.o $@ $(VERBOSE)

$(PROG): $(OBJS) $(ADDOBJS) rom.ld

$(PROG):
	@echo Building $@
	$(QUIET)$(LD) $(filter %.o,$^) $(LDFLAGS) -Map=$(OBJDIR)/$@.map > $(OBJDIR)/$@.lst -o $(@:.rom=.exe) -Trom_exe.ld

#
# I use a custom program,dehunk, to create the ROM image. This is because,
# for an unknown reason, m68k-amigaos-objcopy can't recognize the generated
# Amiga executable.
#
$(PROGROM): $(PROG) $(DEHUNK)
#	$(QUIET)$(LD) $(filter %.o,$^) $(LDFLAGS) -Map=$(OBJDIR)/$@.map > $(OBJDIR)/$@.lst -o $@ -Trom.ld
	$(QUIET)$(DEHUNK) $(PROG) $@ $(VERBOSE)
#	$(QUIET)cat $@ $@ >$@.double

$(DEHUNK): dehunk.c
	cc -o $@ $?

$(OBJDIR):
	mkdir -p $@

clean clean-all:
	@echo Cleaning
	@rm -rf $(OBJS) $(ADDOBJS) $(OBJDIR)
