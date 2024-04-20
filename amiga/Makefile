#
# Makefile to build AmigaOS/68k tools using Bebbo's GCC cross-compiler.
#

SMASH_PROG   := smash
OBJDIR       := objs
SMASH_SRCS   := smash.c crc32.c msg.c cpu_control.c
HDRS         := smash_cmd.h crc32.h cpu_control.h msg.h
SMASH_OBJS   := $(SMASH_SRCS:%.c=$(OBJDIR)/%.o)
CC      := m68k-amigaos-gcc
CFLAGS  := -Wall -Wextra -Wno-pointer-sign -fomit-frame-pointer
CFLAGS  += -Wno-strict-aliasing
LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -mcrt=clib2 -lgcc -lc -lamiga
#LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -mcrt=clib2 -lgcc -lc -lamiga
PROGVER := $(SMASH_PROG)_$(shell awk '/char[[:space:]]*\*version =/{print $$7}' smash.c)

CFLAGS  += -Os
QUIET   := @
#QUIET   :=

#LDFLAGS += -g
#CFLAGS  += -g

ifeq (, $(shell which $(CC) 2>/dev/null ))
$(error "No $(CC) in PATH: maybe do PATH=$$PATH:/opt/amiga/bin")
endif

all: $(SMASH_PROG)

gdb:
	m68k-amigaos-gdb $(SMASH_PROG)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SMASH_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))

$(SMASH_OBJS): Makefile $(HDRS) | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(SMASH_PROG): $(SMASH_OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(SMASH_OBJS) $(LDFLAGS) -o $@

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
	rm -f $(SMASH_OBJS) $(OBJDIR)/*.map $(OBJDIR)/*.lst

FLINT_FILE=flexelint.lnt
flint:
	flexelint -v -w3 -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/sys-include -I/opt/amiga/m68k-amigaos/clib2/include flexelint.lnt $(SMASH_SRCS)
