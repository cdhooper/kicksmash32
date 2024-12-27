#
# Makefile to build AmigaOS/68k tools using Bebbo's GCC cross-compiler.
#

SMASH_PROG   := smash
FS_PROG      := smashfs
FTP_PROG     := smashftp
FSROM_PROG   := smashfsrom
FSROM_PROG_D := smashfsrom_d
SWITCH_PROG  := romswitch

DISK=kicksmash32-$(PROGVER).adf

OBJDIR       := objs
ROM_OBJDIR   := objs.rom
ROM_OBJDIR_D := objs.rom_d
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
FSROM_SRCS   := fs_rom.c \
		fs_hand.c fs_timer.c fs_vol.c fs_packet.c \
		printf.c sm_msg.c sm_file.c cpu_control.c \
		my_createtask.c sm_msg_core.c fs_rom_end.c
FSROM_OBJS   := $(FSROM_SRCS:%.c=$(ROM_OBJDIR)/%.o)
FSROM_OBJS_D := $(FSROM_SRCS:%.c=$(ROM_OBJDIR_D)/%.o)
SWITCH_SRCS  := romswitch.c sm_msg.c cpu_control.c printf.c \
		sm_msg_core.c fs_rom_end.c
SWITCH_OBJS  := $(SWITCH_SRCS:%.c=$(ROM_OBJDIR_D)/%.o)

CC      := m68k-amigaos-gcc
STRIP   := m68k-amigaos-strip
CFLAGS  := -Wall -Wextra -Wno-pointer-sign -Wno-format -Wno-strict-aliasing
CFLAGS  += -Wno-sign-compare -fomit-frame-pointer
CFLAGS_ROMFS := -DROMFS -fbaserel -resident -mcpu=68060
LDFLAGS_SMASH = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@_prog.lst -mcrt=clib2 -lgcc -lc -lamiga
#LDFLAGS_FS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@_prog.lst -nostdlib -lgcc -lamiga -lc
LDFLAGS_FS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@_prog.lst -lgcc -lamiga -lc -noixemul
LDFLAGS_FTP = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@_prog.lst -mcrt=clib2 -lgcc -lc -lamiga

# The below makes smashftp huge
#LDFLAGS_FTP = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -lgcc -lc -lamiga -noixemul

LDFLAGS_FSROM = -nostdlib -lgcc -lc -lamiga -Xlinker --verbose -Tfs_rom.ld -mcrt=clib2 -fbaserel
LDFLAGS_SWITCH = -nostdlib -lgcc -lc -lamiga -Xlinker --verbose -Tfs_rom.ld -mcrt=clib2 -fbaserel

PROGVER := $(SMASH_PROG)_$(shell awk '/char[[:space:]]*\*version =/{print $$7}' smash.c)

NDK_PATH := /opt/amiga/m68k-amigaos/ndk-include
VASM     := vasmm68k_mot

CFLAGS  += -Os
QUIET   := @

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

all: $(SMASH_PROG) $(FS_PROG) $(FTP_PROG) $(FSROM_PROG) $(FSROM_PROG_D) $(SWITCH_PROG)
	@:

gdb:
	m68k-amigaos-gdb $(SMASH_PROG)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(2)/%,$(filter-out $(2)/%,$(basename $(1)).o)) $(filter $(2)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SMASH_SRCS) $(FS_SRCS) $(FTP_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(OBJDIR))))
$(foreach SRCFILE,$(FSROM_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(ROM_OBJDIR))))
$(foreach SRCFILE,$(FSROM_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(ROM_OBJDIR_D))))
$(foreach SRCFILE,$(SWITCH_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(ROM_OBJDIR_D))))

OBJS := $(sort $(SMASH_OBJS) $(FS_OBJS) $(FTP_OBJS) $(FSROM_OBJS) $(FSROM_OBJS_D) $(SWITCH_OBJS))

$(FSROM_OBJS):: CFLAGS += $(CFLAGS_ROMFS) -DNO_DEBUG
$(FSROM_OBJS_D):: CFLAGS += $(CFLAGS_ROMFS)
$(SWITCH_OBJS):: CFLAGS += $(CFLAGS_ROMFS)
#$(FSROM_OBJS):: CFLAGS += -flto
#LDFLAGS_FSROM += -flto -O2

$(FSROM_OBJS) $(FSROM_OBJS_D) $(SWITCH_OBJS): | $(ROM_OBJDIR) $(ROM_OBJDIR_D)

$(OBJS): Makefile $(SMASH_HDRS) | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -Wa,-a,-ad >$(@:.o=.lst) -o $@

$(FS_OBJS): $(FS_HDRS)
$(FTP_OBJS): $(FTP_HDRS)

$(SMASH_PROG): $(SMASH_OBJS)
$(FTP_PROG): $(FTP_OBJS)
$(FSROM_PROG): $(FSROM_OBJS) fs_rom.ld
$(FSROM_PROG_D): $(FSROM_OBJS_D) fs_rom.ld
$(SWITCH_PROG): $(SWITCH_OBJS) fs_rom.ld

$(SMASH_PROG):
	@echo Building $@
	$(QUIET)$(CC) $^ $(LDFLAGS_SMASH) -o $@

$(FTP_PROG):
	@echo Building $@
	$(QUIET)$(CC) $^ $(LDFLAGS_FTP) -o $@

$(FSROM_PROG):
	@echo Building $@
	$(QUIET)$(CC) $(filter %.o,$^) $(LDFLAGS_FSROM) -Xlinker -Map=$(ROM_OBJDIR)/$@.map -Wa,-a,-ad > $(ROM_OBJDIR)/$@.lst -nostartfiles -o $(ROM_OBJDIR)/$@
	$(QUIET)$(STRIP) -o $@ $(ROM_OBJDIR)/$@

$(FSROM_PROG_D):
	@echo Building $@
	$(QUIET)$(CC) $(filter %.o,$^) $(LDFLAGS_FSROM) -Xlinker -Map=$(ROM_OBJDIR_D)/$@.map -Wa,-a,-ad > $(ROM_OBJDIR_D)/$@.lst -nostartfiles -o $(ROM_OBJDIR_D)/$@
	$(QUIET)$(STRIP) -o $@ $(ROM_OBJDIR_D)/$@

$(SWITCH_PROG):
	@echo Building $@
	$(QUIET)$(CC) $(filter %.o,$^) $(LDFLAGS_SWITCH) -Xlinker -Map=$(ROM_OBJDIR)/$@.map -Wa,-a,-ad > $(ROM_OBJDIR)/$@.lst -nostartfiles -o $@

$(FS_PROG): $(FS_OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(FS_OBJS) $(LDFLAGS_FS) -o $@

$(OBJDIR) $(ROM_OBJDIR) $(ROM_OBJDIR_D):
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
	$(QUIET)rm -rf $(SMASH_PROG).lha $(PROGVER)
	$(QUIET)mkdir $(PROGVER)
	$(QUIET)cp -p $(SMASH_PROG) $(PROGVER)/
	$(QUIET)lha -aq2 $(PROGVER).lha $(PROGVER)
	$(QUIET)rm -rf $(PROGVER)

adf:
	xdftool $(DISK) format "KickSmash32 $(PROGVER)"
	xdftool $(DISK) makedir kicksmash
	xdftool $(DISK) write romswitch kicksmash/romswitch
	xdftool $(DISK) write smash kicksmash/smash
	xdftool $(DISK) write smashfs kicksmash/smashfs
	xdftool $(DISK) write smashfsrom kicksmash/smashfsrom
	xdftool $(DISK) write smashftp kicksmash/smashftp
	xdftool $(DISK) boot install

clean clean-all:
	@echo Cleaning
	@rm -rf $(OBJS) $(OBJDIR) $(ROM_OBJDIR) $(ROM_OBJDIR_D)

FLINT_FILE=flexelint.lnt
flint:
	flexelint -v -w3 -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/sys-include -I/opt/amiga/m68k-amigaos/clib2/include flexelint.lnt $(SMASH_SRCS)
