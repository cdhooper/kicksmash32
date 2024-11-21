PROG1=hostsmash
SRCS1=hostsmash.c crc32.c version.c
PROG2=crcit
SRCS2=crcit.c crc32.c
CC := gcc
#CFLAGS  := -O2 -g -pthread -Wall -Wpedantic
#LDFLAGS := -O2 -g -lpthread
CFLAGS  := -O2 -pthread -Wall -Wpedantic -Wno-variadic-macros
LDFLAGS := -O2 -lpthread
UNAME_S := $(shell uname -s)

#
# Allowing links opens security risk for remote mounts because anything
# the user running hostsmash can access may then be potentially linked
# and accessed by the Amiga
CFLAGS += -DALLOW_CREATE_LINK

NOW  := $(shell date +%s)
ifeq ($(UNAME_S),Darwin)
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
    OS ?= $(shell uname -s)
    TARGET_OS ?= $(OS)
endif

# Windows
ifneq (,$(filter $(TARGET_OS),Windows_NT Windows win win32 win64))
    CFLAGS := $(filter-out -DALLOW_CREATE_LINK, $(CFLAGS))
    # -static works for Vista+, but doesn't work for XP
    LDFLAGS += -lws2_32 -static
    OBJDIR := objs.win64
    SRCS1 += mingw-realpath.c
    CC := x86_64-w64-mingw32-gcc
endif
ifneq (,$(filter $(TARGET_OS),win32))
    # Windows 32-bit
    LDFLAGS += -Lmingw_7.5.0_i686  # old version of pthread library for Win XP
    OBJDIR := objs.win32
    CC := i686-w64-mingw32-gcc
endif
ifneq (,$(filter $(TARGET_OS),Windows_NT Windows win win32 win64))
    PROG1 := $(PROG1).exe
    PROG2 := $(PROG2).exe
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

OPROG1 := $(OBJDIR)/$(PROG1)
OPROG2 := $(OBJDIR)/$(PROG2)

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

OBJS1  := $(SRCS1:%.c=$(OBJDIR)/%.o)
OBJS2  := $(SRCS2:%.c=$(OBJDIR)/%.o)

nativeprog: $(OPROG1) $(OPROG2)
	@:

all: $(OPROG1) $(OPROG2) win32 win64
	@:

win32:
	$(MAKE) TARGET_OS=win32
win win64:
	$(MAKE) TARGET_OS=win64

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS1),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCS2),$(eval $(call DEPEND_SRC,$(SRCFILE))))


$(OBJS1) $(OBJS2): Makefile smash_cmd.h version.h crc32.h host_cmd.h
$(OBJDIR)/hostsmash.o: | $(USB_HDR)
$(OBJDIR)/version.o: $(filter-out $(OBJDIR)/version.o,$(OBJS1)) Makefile

$(OPROG1): $(OBJS1)
	@echo Building $@
	$(QUIET)$(CC) -o $@ $(OBJS1) $(LDFLAGS)
	@rm -f $(PROG1)
	@ln -s $@

$(OPROG2): $(OBJS2)
	@echo Building $@
	$(QUIET)$(CC) -o $@ $(OBJS2) $(LDFLAGS)
	@rm -f $(PROG2)
	@ln -s $@

$(sort $(OBJS1) $(OBJS2)): Makefile | $(OBJDIR)
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
	$(QUIET)rm -rf $(OPROG1) $(OPROG2) $(OBJDIR)

clean-all: clean
	@$(MAKE) TARGET_OS=win32 clean
	@$(MAKE) TARGET_OS=win64 clean

verbose:

.PHONY: all clean clean-all verbose nativeprog
