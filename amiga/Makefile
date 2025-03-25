#
# Makefile to build AmigaOS/68k tools using Bebbo's GCC cross-compiler.
#

SMASH_PROG   := smash
FS_PROG      := smashfs
FTP_PROG     := smashftp
FSROM_PROG   := smashfsrom
FSROM_PROG_D := smashfsrom_d
SWITCH_PROG  := romswitch

ALL_PROGS    := $(SMASH_PROG) $(FS_PROG) $(FTP_PROG) \
                $(FSROM_PROG) $(FSROM_PROG_D) $(SWITCH_PROG)

OBJDIR       := objs
ROM_OBJDIR   := objs.rom
ROM_OBJDIR_D := objs.rom_d
CRC32_C      := ../fw/crc32.c
SMASH_SRCS   := smash.c sm_msg.c cpu_control.c $(CRC32_C)
SMASH_HDRS   := cpu_control.h sm_msg.h sm_file.h host_cmd.h \
                ../fw/smash_cmd.h ../fw/crc32.h
FS_SRCS      := fs_hand.c fs_timer.c fs_vol.c fs_packet.c \
		printf.c sm_msg.c sm_file.c cpu_control.c $(CRC32_C)
FS_HDRS	     := fs_hand.h fs_packet.h fs_timer.h fs_vol.h printf.h
FTP_SRCS     := smashftp.c smashftp_cli.c sm_msg.c cpu_control.c \
		readline.c sm_file.c $(CRC32_C)
FTP_HDRS     := smashftp.h smashftp_cli.h readline.h
FSROM_SRCS   := fs_rom.c \
		fs_hand.c fs_timer.c fs_vol.c fs_packet.c \
		printf.c sm_msg.c sm_file.c cpu_control.c \
		my_createtask.c sm_msg_core.c fs_rom_end.c
SWITCH_SRCS  := romswitch.c sm_msg.c cpu_control.c printf.c \
		sm_msg_core.c fs_rom_end.c

#SMASH_OBJS   := $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(SMASH_SRCS)))
#FS_OBJS      := $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(FS_SRCS)))
#FTP_OBJS     := $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(FTP_SRCS)))
#FSROM_OBJS   := $(patsubst %.c,$(ROM_OBJDIR)/%.o,$(notdir $(FSROM_SRCS)))
#FSROM_OBJS_D := $(patsubst %.c,$(ROM_OBJDIR_D)/%.o,$(notdir $(FSROM_SRCS)))
#SWITCH_OBJS  := $(patsubst %.c,$(ROM_OBJDIR_D)/%.o,$(notdir $(SWITCH_SRCS)))

CC      := m68k-amigaos-gcc
STRIP   := m68k-amigaos-strip
CFLAGS  := -Wall -Wextra -Wno-pointer-sign -Wno-format -Wno-strict-aliasing
CFLAGS  += -Wno-sign-compare -fomit-frame-pointer -I../fw -mcpu=68060
CFLAGS_ROMFS := -DROMFS -fbaserel -resident -mcpu=68060
LDFLAGS_SMASH = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@_prog.lst -mcrt=clib2 -lgcc -lc -lamiga
#LDFLAGS_FS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@_prog.lst -nostdlib -lgcc -lamiga -lc
LDFLAGS_FS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@_prog.lst -lgcc -lamiga -lc -noixemul
LDFLAGS_FTP = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@_prog.lst -mcrt=clib2 -lgcc -lc -lamiga

NOW  := $(shell date +%s)
ifeq ($(OS),Darwin)
DATE := $(shell date -j -f %s $(NOW)  '+%Y-%m-%d')
TIME := $(shell date -j -f %s $(NOW)  '+%H:%M:%S')
else
DATE := $(shell date -d "@$(NOW)" '+%Y-%m-%d')
TIME := $(shell date -d "@$(NOW)" '+%H:%M:%S')
endif
CFLAGS += -DBUILD_DATE=\"$(DATE)\" -DBUILD_TIME=\"$(TIME)\"

# The below makes smashftp huge
#LDFLAGS_FTP = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -lgcc -lc -lamiga -noixemul

LDFLAGS_FSROM = -nostdlib -lgcc -lc -lamiga -Xlinker --verbose -Tfs_rom.ld -mcrt=clib2 -fbaserel
LDFLAGS_SWITCH = -nostdlib -lgcc -lc -lamiga -Xlinker --verbose -Tfs_rom.ld -mcrt=clib2 -fbaserel

VERSION := $(shell awk '/Version/{print $$2}' ../fw/version.c)
CFLAGS += -DVERSION=\"$(VERSION)\"
PROGVER := kicksmash_$(VERSION)

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

TEST :=
define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(2)/%,$(filter-out $(2)/%,$(basename $(notdir $(1))).o)) $(filter $(2)/%,$(basename $(1)).o): $(1)
# The following line adds that object to macro containing the list of objects
$(3) += $(patsubst %,$(2)/%,$(filter-out $(2)/%,$(basename $(notdir $(1))).o))
endef

$(foreach SRCFILE,$(SMASH_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(OBJDIR),SMASH_OBJS)))
$(foreach SRCFILE,$(FS_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(OBJDIR),FS_OBJS)))
$(foreach SRCFILE,$(FTP_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(OBJDIR),FTP_OBJS)))
$(foreach SRCFILE,$(FSROM_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(ROM_OBJDIR),FSROM_OBJS)))
$(foreach SRCFILE,$(FSROM_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(ROM_OBJDIR_D),FSROM_OBJS_D)))
$(foreach SRCFILE,$(SWITCH_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(ROM_OBJDIR_D),SWITCH_OBJS)))

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

ZIPFILE := $(PROGVER).zip
LHAFILE := $(PROGVER).lha
DISK	:= $(PROGVER).adf

zip:
	@echo Building $(ZIPFILE)
	$(QUIET)rm -rf $(ZIPFILE) $(PROGVER)
	$(QUIET)mkdir $(PROGVER)
	$(QUIET)cp -p $(ALL_PROGS) $(PROGVER)/
	$(QUIET)zip -rq $(ZIPFILE) $(PROGVER)
	$(QUIET)rm -rf $(PROGVER)

lha:
	@echo Building $(LHAFILE)
	$(QUIET)rm -rf $(LHAFILE) $(PROGVER)
	$(QUIET)mkdir $(PROGVER)
	$(QUIET)cp -p $(ALL_PROGS) $(PROGVER)/
	$(QUIET)lha -aq2 $(LHAFILE) $(PROGVER)
	$(QUIET)rm -rf $(PROGVER)

adf:
	xdftool $(DISK) format "$(PROGVER)"
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
