HOSTSMASH_PROG=hostsmash
HOSTSMASH_SRCS=hostsmash.c ../fw/version.c ../fw/crc32.c
CRCIT_PROG=crcit
CRCIT_SRCS=crcit.c ../fw/crc32.c
CC := gcc
#CFLAGS  := -O2 -g -pthread -Wall -Wpedantic
#LDFLAGS := -O2 -g -lpthread
CFLAGS  := -O2 -pthread -Wall -Wpedantic -Wno-variadic-macros
LDFLAGS := -O2 -lpthread
OS := $(shell uname -s)

#
# Allowing links opens security risk for remote mounts because anything
# the user running hostsmash can access may then be potentially linked
# and accessed by the Amiga
CFLAGS += -DALLOW_CREATE_LINK

NOW  := $(shell date +%s)
ifeq ($(OS),Darwin)
DATE := $(shell date -j -f %s $(NOW)  '+%Y-%m-%d')
TIME := $(shell date -j -f %s $(NOW)  '+%H:%M:%S')
else
DATE := $(shell date -d "@$(NOW)" '+%Y-%m-%d')
TIME := $(shell date -d "@$(NOW)" '+%H:%M:%S')
endif
CFLAGS += -DBUILD_DATE=\"$(DATE)\" -DBUILD_TIME=\"$(TIME)\"

# Get the default target operating system
ifneq (,$(filter $(OS),Windows_NT Windows win))
    TARGET_OS ?= Windows_NT
else
    TARGET_OS ?= $(OS)
endif

# Windows
ifneq (,$(filter $(TARGET_OS),Windows_NT Windows win win32 win64))
    CFLAGS := $(filter-out -DALLOW_CREATE_LINK, $(CFLAGS))
    # -static works for Vista+, but doesn't work for XP
    LDFLAGS += -lws2_32 -static
    OBJDIR := objs.win64
    HOSTSMASH_SRCS += mingw-realpath.c
    CC := x86_64-w64-mingw32-gcc
endif
ifneq (,$(filter $(TARGET_OS),win32))
    # Windows 32-bit
    LDFLAGS += -Lmingw_7.5.0_i686  # old version of pthread library for Win XP
    OBJDIR := objs.win32
    CC := i686-w64-mingw32-gcc
endif
ifneq (,$(filter $(TARGET_OS),Windows_NT Windows win win32 win64))
    HOSTSMASH_PROG := $(HOSTSMASH_PROG).exe
    CRCIT_PROG := $(CRCIT_PROG).exe
endif

# Linux
ifeq ($(TARGET_OS),Linux)
    CFLAGS += -DLINUX
    UNAME_M := $(shell uname -m)
    OBJDIR := objs.$(UNAME_M)
endif

# MacOS
ifeq ($(TARGET_OS),Darwin)
#   CFLAGS += -DOSX -target x86_64-apple-macos10.12
    CFLAGS += -DOSX #-lxml2 -I/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/libxml2
    OBJDIR := objs.mac
endif

#CFLAGS += -fanalyzer

HOSTSMASH_OPROG := $(OBJDIR)/$(HOSTSMASH_PROG)
CRCIT_OPROG := $(OBJDIR)/$(CRCIT_PROG)

#ifneq ($(TARGET_OS),$(OS))
#    $(info HOST=$(OS) TARGET=$(TARGET_OS))
#endif

# If verbose is specified with no other targets, then build everything
ifeq ($(MAKECMDGOALS),verbose)
verbose: all
endif
ifeq (,$(filter verbose timed, $(MAKECMDGOALS)))
QUIET   := @
else
QUIET   :=
endif

#HOSTSMASH_OBJS  := $(HOSTSMASH_SRCS:%.c=$(OBJDIR)/%.o)
#CRCIT_OBJS  := $(CRCIT_SRCS:%.c=$(OBJDIR)/%.o)

nativeprog: $(HOSTSMASH_OPROG) $(CRCIT_OPROG)
	@:

all: $(HOSTSMASH_OPROG) $(CRCIT_OPROG) win32 win64
	@:

win32:
	$(MAKE) TARGET_OS=win32
win win64:
	$(MAKE) TARGET_OS=win64

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(2)/%,$(filter-out $(2)/%,$(basename $(notdir $(1))).o)) $(filter $(2)/%,$(basename $(1)).o): $(1)
# The following line adds that object to macro containing the list of objects
$(3) += $(patsubst %,$(2)/%,$(filter-out $(2)/%,$(basename $(notdir $(1))).o))
endef

$(foreach SRCFILE,$(HOSTSMASH_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(OBJDIR),HOSTSMASH_OBJS)))
$(foreach SRCFILE,$(CRCIT_SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE),$(OBJDIR),CRCIT_OBJS)))


$(HOSTSMASH_OBJS) $(CRCIT_OBJS): Makefile ../fw/version.h ../fw/smash_cmd.h ../fw/crc32.h ../amiga/host_cmd.h
$(OBJDIR)/hostsmash.o: | $(USB_HDR)
$(OBJDIR)/version.o: $(filter-out $(OBJDIR)/version.o,$(HOSTSMASH_OBJS)) Makefile

$(HOSTSMASH_OPROG): $(HOSTSMASH_OBJS)
	@echo Building $@
	$(QUIET)$(CC) -o $@ $(HOSTSMASH_OBJS) $(LDFLAGS)
	@rm -f $(HOSTSMASH_PROG)
	@ln -s $@

$(CRCIT_OPROG): $(CRCIT_OBJS)
	@echo Building $@
	$(QUIET)$(CC) -o $@ $(CRCIT_OBJS) $(LDFLAGS)
	@rm -f $(CRCIT_PROG)
	@ln -s $@

$(sort $(HOSTSMASH_OBJS) $(CRCIT_OBJS)): Makefile | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(OBJDIR):
	@echo Creating $@
	$(QUIET)mkdir $@

$(USB_HDR):
	echo "You must install the libusb development package"
	echo "On Fedora:   dnf install libusb-devel"
	echo "On Raspbian: apt-get install libusb-dev"
	exit 1

clean:
	@echo Cleaning
	$(QUIET)rm -rf $(HOSTSMASH_OPROG) $(CRCIT_OPROG) $(OBJDIR)

clean-all: clean
	@$(MAKE) TARGET_OS=win32 clean
	@$(MAKE) TARGET_OS=win64 clean

verbose:

.PHONY: all clean clean-all verbose nativeprog
