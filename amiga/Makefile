#
# Makefile to build AmigaOS/68k tools using Bebbo's GCC cross-compiler.
#

PROG    := smash
OBJDIR  := objs
SRCS    := smash.c crc32.c
HDRS    := smash_cmd.h crc32.h
OBJS    := $(SRCS:%.c=$(OBJDIR)/%.o)
CC      := m68k-amigaos-gcc
CFLAGS  := -Wall -Wextra -Wno-pointer-sign -fomit-frame-pointer
CFLAGS  += -Wno-strict-aliasing
LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -mcrt=clib2 -lgcc -lc -lamiga
#LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -mcrt=clib2 -lgcc -lc -lamiga
PROGVER := $(PROG)_$(shell awk '/char[[:space:]]*\*version =/{print $$7}' smash.c)

CFLAGS  += -Os
QUIET   := @
#QUIET   :=

#LDFLAGS += -g
#CFLAGS  += -g

ifeq (, $(shell which $(CC) 2>/dev/null ))
$(error "No $(CC) in PATH: maybe do PATH=$$PATH:/opt/amiga/bin")
endif

all: $(PROG)

gdb:
	m68k-amigaos-gdb $(PROG)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))

$(OBJS): Makefile $(HDRS) | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(PROG): $(OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(OBJS) $(LDFLAGS) -o $@

$(OBJDIR):
	mkdir -p $@

zip:
	@echo Building $(PROGVER).zip
	$(QUIET)rm -rf $(PROG).zip $(PROGVER)
	$(QUIET)mkdir $(PROGVER)
	$(QUIET)cp -p $(PROG) $(PROGVER)/
	$(QUIET)zip -rq $(PROGVER).zip $(PROGVER)
	$(QUIET)rm -rf $(PROGVER)

lha:
	@echo Building $(PROGVER).lha
	$(QUIET)rm -rf $(PROG).zip $(PROGVER)
	$(QUIET)mkdir $(PROGVER)
	$(QUIET)cp -p $(PROG) $(PROGVER)/
	$(QUIET)lha -aq2 $(PROGVER).lha $(PROGVER)
	$(QUIET)rm -rf $(PROGVER)

clean:
	rm -f $(OBJS) $(OBJDIR)/*.map $(OBJDIR)/*.lst

FLINT_FILE=flexelint.lnt
flint:
	flexelint -v -w3 -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/sys-include -I/opt/amiga/m68k-amigaos/clib2/include flexelint.lnt $(SRCS)
