#
# Makefile to build AmigaOS/68k tools using Bebbo's GCC cross-compiler.
#

SMASH_PROG   := smash
FS_PROG      := smashfs
FTP_PROG     := smashftp
OBJDIR       := objs
SMASH_SRCS   := smash.c crc32.c sm_msg.c cpu_control.c
SMASH_HDRS   := smash_cmd.h host_cmd.h crc32.h cpu_control.h sm_msg.h sm_file.h
SMASH_OBJS   := $(SMASH_SRCS:%.c=$(OBJDIR)/%.o)
FS_SRCS      := fs_hand.c printf.c
FS_OBJS      := $(FS_SRCS:%.c=$(OBJDIR)/%.o)
FTP_SRCS     := smashftp.c smashftp_cli.c crc32.c sm_msg.c cpu_control.c \
		readline.c sm_file.c
FTP_OBJS     := $(FTP_SRCS:%.c=$(OBJDIR)/%.o)

CC      := m68k-amigaos-gcc
CFLAGS  := -Wall -Wextra -Wno-pointer-sign -fomit-frame-pointer
CFLAGS  += -Wno-strict-aliasing -Wno-sign-compare
LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -mcrt=clib2 -lgcc -lc -lamiga
LDFLAGS_FS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -lamiga -lgcc -nostdlib -ldebug -lgcc -lc
LDFLAGS_FTP = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -mcrt=clib2 -lgcc -lc -lamiga
PROGVER := $(SMASH_PROG)_$(shell awk '/char[[:space:]]*\*version =/{print $$7}' smash.c)

CFLAGS  += -Os
QUIET   := @
#QUIET   :=

#LDFLAGS += -g
#CFLAGS  += -g

#
# Allowing links opens security risk for remote mounts because anything
# the user running hostsmash can access may then be potentially linked
# and accessed by the Amiga
CFLAGS += -DALLOW_CREATE_LINK

ifeq (, $(shell which $(CC) 2>/dev/null ))
$(error "No $(CC) in PATH: maybe do PATH=$$PATH:/opt/amiga/bin")
endif

all: $(SMASH_PROG) $(FS_PROG) $(FTP_PROG)

gdb:
	m68k-amigaos-gdb $(SMASH_PROG)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SMASH_SRCS) $(FS_SRCS) $(FTP_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))

OBJS := $(sort $(SMASH_OBJS) $(FS_OBJS) $(FTP_OBJS))

$(OBJS): Makefile $(SMASH_HDRS) | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(SMASH_PROG): $(SMASH_OBJS)
$(FTP_PROG): $(FTP_OBJS)

$(SMASH_PROG):
	@echo Building $@
	$(QUIET)$(CC) $^ $(LDFLAGS) -o $@

$(FTP_PROG):
	@echo Building $@
	$(QUIET)$(CC) $^ $(LDFLAGS_FTP) -o $@

$(FS_PROG): $(FS_OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(FS_OBJS) $(LDFLAGS_FS) -o $@

$(OBJDIR):
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
	rm -f $(OBJS) $(OBJDIR)/*.map $(OBJDIR)/*.lst

FLINT_FILE=flexelint.lnt
flint:
	flexelint -v -w3 -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/sys-include -I/opt/amiga/m68k-amigaos/clib2/include flexelint.lnt $(SMASH_SRCS)
