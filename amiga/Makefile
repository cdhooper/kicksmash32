#
# Makefile to build AmigaOS/68k tools using Bebbo's GCC cross-compiler.
#

SMASH_PROG   := smash
FS_PROG      := smashfs
FSROM_PROG   := smashfsrom
FTP_PROG     := smashftp
OBJDIR       := objs
ROM_OBJDIR   := objs.rom
SMASH_SRCS   := smash.c crc32.c sm_msg.c cpu_control.c
SMASH_HDRS   := smash_cmd.h host_cmd.h crc32.h cpu_control.h sm_msg.h sm_file.h
SMASH_OBJS   := $(SMASH_SRCS:%.c=$(OBJDIR)/%.o)
FS_SRCS      := fs_hand.c fs_timer.c fs_vol.c fs_packet.c \
		printf.c crc32.c sm_msg.c sm_file.c cpu_control.c
FS_HDRS	     := fs_hand.h fs_packet.h fs_timer.h fs_vol.h printf.h
FS_OBJS      := $(FS_SRCS:%.c=$(OBJDIR)/%.o)
FTP_SRCS     := smashftp.c smashftp_cli.c crc32.c sm_msg.c cpu_control.c \
		readline.c sm_file.c
FTP_HDRS     := smashftp.h smashftp_cli.h readline.h
FTP_OBJS     := $(FTP_SRCS:%.c=$(OBJDIR)/%.o)
FSROM_ASRCS    :=
#FSROM_ASRCS    := fs_rom_asm.S
FSROM_AOBJS    := $(FSROM_ASRCS:%.S=$(ROM_OBJDIR)/%.o)
FSROM_CSRCS    := fs_rom.c $(FS_SRCS) fs_rom_end.c
FSROM_COBJS    := $(FSROM_CSRCS:%.c=$(ROM_OBJDIR)/%.o)
FSROM_OBJS     := $(FSROM_AOBJS) $(FSROM_COBJS)

CC      := m68k-amigaos-gcc
STRIP   := m68k-amigaos-strip
CFLAGS  := -Wall -Wextra -Wno-pointer-sign -Wno-format -Wno-strict-aliasing
CFLAGS  += -Wno-sign-compare -fomit-frame-pointer
SMASH_LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -mcrt=clib2 -lgcc -lc -lamiga
#LDFLAGS_FS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -nostdlib -lgcc -lamiga -lc
LDFLAGS_FS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -lgcc -lamiga -lc -noixemul
LDFLAGS_FTP = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -mcrt=clib2 -lgcc -lc -lamiga

# The below makes smashftp huge
#LDFLAGS_FTP = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -lgcc -lc -lamiga -noixemul

#LDFLAGS_FSROM = -Xlinker -Map=$(ROM_OBJDIR)/$@.map -Wa,-a > $(ROM_OBJDIR)/$@.lst -nostartfiles -nostdlib -lgcc -lc -lamiga -Xlinker -v -Xlinker --verbose -Tfs_rom.ld -noixemul -fbaserel
LDFLAGS_FSROM = -Xlinker -Map=$(ROM_OBJDIR)/$@.map -Wa,-a > $(ROM_OBJDIR)/$@.lst -nostartfiles -nostdlib -lgcc -lc -lamiga -Xlinker -v -Xlinker --verbose -Tfs_rom.ld -mcrt=clib2 -fbaserel
PROGVER := $(SMASH_PROG)_$(shell awk '/char[[:space:]]*\*version =/{print $$7}' smash.c)

NDK_PATH := /opt/amiga/m68k-amigaos/ndk-include
VASM     := vasmm68k_mot

CFLAGS  += -Os
QUIET   := @
#QUIET   :=

# Enable to completely turn off debug output (smashfs is about 5K smaller)
#CFLAGS += -NO_DEBUG

#LDFLAGS_FTP += -g
#LDFLAGS += -g
#CFLAGS  += -g


#
# Allowing links opens security risk for remote mounts because anything
# the user running hostsmash can access may then be potentially linked
# and accessed by the Amiga
CFLAGS += -DALLOW_CREATE_LINK

# If verbose is specified with no other targets, then build everything
ifeq ($(MAKECMDGOALS),verbose)
verbose: all
endif
ifeq (,$(filter verbose timed, $(MAKECMDGOALS)))
QUIET   := @
else
QUIET   :=
endif

ifeq (, $(shell which $(CC) 2>/dev/null ))
$(error "No $(CC) in PATH: maybe do PATH=$$PATH:/opt/amiga13/bin")
endif

all: $(SMASH_PROG) $(FS_PROG) $(FTP_PROG) $(FSROM_PROG)

gdb:
	m68k-amigaos-gdb $(SMASH_PROG)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SMASH_SRCS) $(FS_SRCS) $(FTP_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))
define DEPEND_ROMSRC
$(patsubst %,$(ROM_OBJDIR)/%,$(filter-out $(ROM_OBJDIR)/%,$(basename $(1)).o)) $(filter $(ROM_OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(FSROM_ASRCS) $(FSROM_CSRCS),$(eval $(call DEPEND_ROMSRC,$(SRCFILE))))

OBJS := $(sort $(SMASH_OBJS) $(FS_OBJS) $(FTP_OBJS) $(FSROM_COBJS))

$(FSROM_COBJS):: CFLAGS += -fbaserel
$(FSROM_COBJS): | $(ROM_OBJDIR)

$(OBJS): Makefile $(SMASH_HDRS) | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(FSROM_AOBJS): Makefile $(SMASH_HDRS) | $(ROM_OBJDIR)
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk $(filter %.S,$^) -I $(OBJDIR) -I $(NDK_PATH) $(ROMDRIVER) -o $@

$(FS_OBJS): $(FS_HDRS)
$(FTP_OBJS): $(FTP_HDRS)

$(SMASH_PROG): $(SMASH_OBJS)
$(FTP_PROG): $(FTP_OBJS)
$(FSROM_PROG): $(FSROM_OBJS) fs_rom.ld

$(SMASH_PROG):
	@echo Building $@
	$(QUIET)$(CC) $^ $(SMASH_LDFLAGS) -o $@

$(FTP_PROG):
	@echo Building $@
	$(QUIET)$(CC) $^ $(LDFLAGS_FTP) -o $@

$(FSROM_PROG):
	@echo Building $@
	$(QUIET)$(CC) $(filter %.o,$^) $(LDFLAGS_FSROM) -o $@

$(FS_PROG): $(FS_OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(FS_OBJS) $(LDFLAGS_FS) -o $@

$(OBJDIR) $(ROM_OBJDIR):
	mkdir -p $@

zip:
	@echo Building $(PROGVER).zip
	$(QUIET)rm -rf $(SMASH_PROG).zip $(PROGVER)
	$(QUIET)mkdir $(PROGVER)
	$(QUIET)cp -p $(SMASH_PROG) $(PROGVER)/
	$(QUIET)zip -rq $(PROGVER).zip $(PROGVER)
	$(QUIET)rm -rf $(PROGVER)

lha:
	@echo Building $(PROGVER).lha
	$(QUIET)rm -rf $(SMASH_PROG).zip $(PROGVER)
	$(QUIET)mkdir $(PROGVER)
	$(QUIET)cp -p $(SMASH_PROG) $(PROGVER)/
	$(QUIET)lha -aq2 $(PROGVER).lha $(PROGVER)
	$(QUIET)rm -rf $(PROGVER)

clean:
	rm -f $(OBJS) $(OBJDIR)/*.map $(OBJDIR)/*.lst $(ROM_OBJDIR)/*.map $(ROM_OBJDIR)/*.lst

FLINT_FILE=flexelint.lnt
flint:
	flexelint -v -w3 -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/sys-include -I/opt/amiga/m68k-amigaos/clib2/include flexelint.lnt $(SMASH_SRCS)
