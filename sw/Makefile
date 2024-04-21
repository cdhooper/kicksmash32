PROG=hostsmash
SRCS=hostsmash.c crc32.c version.c
CC=gcc
CFLAGS  := -O2 -g -pthread -Wall -Wpedantic
LDFLAGS := -O2 -g -lpthread

NOW  := $(shell date)
DATE := $(shell date -d '$(NOW)' '+%Y-%m-%d')
TIME := $(shell date -d '$(NOW)' '+%H:%M:%S')
CFLAGS += -DBUILD_DATE=\"$(DATE)\" -DBUILD_TIME=\"$(TIME)\"

ifeq ($(OS),Windows_NT)
    CFLAGS += -DWIN32
    OBJDIR := objs.win
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        CFLAGS += -DLINUX
	UNAME_M := $(shell uname -m)
	OBJDIR := objs.$(UNAME_M)
    endif
    ifeq ($(UNAME_S),Darwin)
#        CFLAGS += -DOSX -target x86_64-apple-macos10.12
        CFLAGS += -DOSX -lxml2 -I/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/libxml2
	OBJDIR := objs.mac
    endif
endif

OBJS   := $(SRCS:%.c=$(OBJDIR)/%.o)

all: $(PROG)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))


$(OBJS): Makefile smash_cmd.h version.h
$(OBJDIR)/hostsmash.o: | $(USB_HDR)
$(OBJDIR)/version.o: $(filter-out $(OBJDIR)/version.o,$(OBJS)) Makefile

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(OBJS): Makefile | $(OBJDIR)
	$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(OBJDIR):
	mkdir $@

$(USB_HDR):
	echo "You must install the libusb development package"
	echo "On Fedora:   dnf install libusb-devel"
	echo "On Raspbian: apt-get install libusb-dev"
	exit 1

clean:
	rm -rf $(PROG) $(OBJDIR)
