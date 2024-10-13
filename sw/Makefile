PROG=hostsmash
SRCS=hostsmash.c crc32.c version.c
PROG2=crcit
SRCS2=crcit.c crc32.c
CC := gcc
#CFLAGS  := -O2 -g -pthread -Wall -Wpedantic
#LDFLAGS := -O2 -g -lpthread
CFLAGS  := -O0 -g -pthread -Wall -Wpedantic -Wno-variadic-macros
LDFLAGS := -O0 -g -lpthread
UNAME_S := $(shell uname -s)

QUIET ?= @

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
ifneq (,$(filter $(TARGET_OS),Windows_NT Windows win))
    CFLAGS += -DWIN32
    OBJDIR := objs.win
    ifeq ($(OS),Linux)
	# Cross-compile
	CC := x86_64-w64-mingw32-gcc
	PROG := $(OBJDIR)/$(PROG).exe
	PROG2 := $(OBJDIR)/$(PROG2).exe
    endif
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

ifneq ($(TARGET_OS),$(OS))
    $(info HOST=$(OS) TARGET=$(TARGET_OS))
endif

# If verbose is specified with no other targets, then build everything
ifeq ($(MAKECMDGOALS),verbose)
verbose: all
endif
ifeq (,$(filter verbose timed, $(MAKECMDGOALS)))
QUIET   := @
else
QUIET   :=
endif

OBJS   := $(SRCS:%.c=$(OBJDIR)/%.o)
OBJS2  := $(SRCS2:%.c=$(OBJDIR)/%.o)

all: $(PROG) $(PROG2)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCS2),$(eval $(call DEPEND_SRC,$(SRCFILE))))


$(OBJS) $(OBJS2): Makefile smash_cmd.h version.h crc32.h host_cmd.h
$(OBJDIR)/hostsmash.o: | $(USB_HDR)
$(OBJDIR)/version.o: $(filter-out $(OBJDIR)/version.o,$(OBJS)) Makefile

$(PROG): $(OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(LDFLAGS) $(OBJS) -o $@

$(PROG2): $(OBJS2)
	@echo Building $@
	$(QUIET)$(CC) $(LDFLAGS) $(OBJS2) -o $@

$(sort $(OBJS) $(OBJS2)): Makefile | $(OBJDIR)
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
	$(QUIET)rm -rf $(PROG) $(OBJDIR)
