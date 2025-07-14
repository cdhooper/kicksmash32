# Kicksmash Rev 1
# Kicksmash Rev 2 replaces some address pins which have weak drive
# Kicksmash Rev 3 adds 74FCT2244 buffers for data lines
# Kicksmash Rev 4 fixes OEWE circuit

SRCS   := main.c clock.c gpio.c printf.c timer.c uart.c usb.c version.c \
	  led.c irq.c mem_access.c readline.c cmdline.c cmds.c pcmds.c \
	  prom_access.c m29f160xt.c utils.c crc32.c adc.c kbrst.c scanf.c \
	  pin_tests.c stm32flash.c config.c msg.c
USRCS  := usbdfu.c clock.c

OBJDIR := objs
OBJS   := $(SRCS:%.c=$(OBJDIR)/%.o)
UOBJS  := $(USRCS:%.c=$(OBJDIR)/%.o)

QUIET  := @
VERBOSE :=

# If verbose is specified with no other targets, then build everything
ifeq ($(MAKECMDGOALS),verbose)
verbose: all
endif
ifeq (,$(filter verbose timed, $(MAKECMDGOALS)))
QUIET   := @
else
QUIET   :=
VERBOSE := -v
endif

# libopencm3
OPENCM3_DIR := /usr/lib/libopencm3
ifeq (,$(wildcard $(OPENCM3_DIR)))
OPENCM3_DIR := libopencm3
endif
OPENCM3_HEADER := $(OPENCM3_DIR)/include/libopencm3/stm32/rcc.h

# Our output name
BINARY = $(OBJDIR)/fw
UBINARY = $(OBJDIR)/usbdfu

# Linker script for our MCU
LDSCRIPT = stm32f1.ld  # Also works for STM32F4


BOARD_TYPE := STM32F107

ifeq ($(BOARD_TYPE), STM32F4)
#$(info Building for STM32F4)
LIBNAME		:= opencm3_stm32f4
ARCH_FLAGS	:= -mthumb -mcpu=cortex-m4 -mhard-float
DEFS		+= -DSTM32F4 -DSTM32F407xx
else ifeq ($(BOARD_TYPE), STM32F103)
#$(info Building for STM32F103)
LIBNAME		:= opencm3_stm32f1
ARCH_FLAGS	:= -mthumb -mcpu=cortex-m3 -msoft-float
DEFS		+= -DSTM32F1 -DSTM32F103xE
else
#$(info Building for STM32F107)
LIBNAME		:= opencm3_stm32f1
ARCH_FLAGS	:= -mthumb -mcpu=cortex-m3 -msoft-float
DEFS		+= -DSTM32F1 -DSTM32F107xC
endif

DEFS		+= -DEMBEDDED_CMD

OPENCM3_LIB := $(OPENCM3_DIR)/lib/lib$(LIBNAME).a

# Where the Black Magic Probe is attached
BMP_PORT = /dev/ttyACM0

DFU_UTIL=dfu-util
ST_BUILD_DIR=/usr/stlink
ifeq (,$(wildcard $(ST_BUILD_DIR)))
ST_BUILD_DIR=stutils
endif
ST_TOOLS_PATH=$(ST_BUILD_DIR)/build/Release/bin
OS := $(shell uname -s)

NOW  := $(shell date +%s)
ifeq ($(OS),Darwin)
DATE := $(shell date -j -f %s $(NOW)  '+%Y-%m-%d')
TIME := $(shell date -j -f %s $(NOW)  '+%H:%M:%S')
else
DATE := $(shell date -d "@$(NOW)" '+%Y-%m-%d')
TIME := $(shell date -d "@$(NOW)" '+%H:%M:%S')
endif


## Boilerplate

# Compiler configuration
PREFIX		?= arm-none-eabi
CC		:= $(PREFIX)-gcc
CXX		:= $(PREFIX)-g++
LD		:= $(PREFIX)-gcc
AR		:= $(PREFIX)-ar
AS		:= $(PREFIX)-as
SIZE		:= $(PREFIX)-size
OBJCOPY		:= $(PREFIX)-objcopy
OBJDUMP		:= $(PREFIX)-objdump
GDB		:= $(PREFIX)-gdb
STFLASH		= $(shell which st-flash)
OPT		:= -O2
DEBUG		:= -ggdb3
CSTD		?= -std=gnu99

# C flags
TGT_CFLAGS	+= $(OPT) $(CSTD) $(DEBUG)
TGT_CFLAGS	+= $(ARCH_FLAGS)
TGT_CFLAGS	+= -Wextra -Wshadow -Wimplicit-function-declaration
TGT_CFLAGS	+= -Wredundant-decls -Wmissing-prototypes -Wstrict-prototypes
TGT_CFLAGS	+= -fno-common -ffunction-sections -fdata-sections
TGT_CFLAGS	+= -ffreestanding
TGT_CFLAGS	+= -DBUILD_DATE=\"$(DATE)\" -DBUILD_TIME=\"$(TIME)\"
TGT_CFLAGS	+= -Wa,-a > $(@:.o=.lst)

# C++ flags
TGT_CXXFLAGS	+= $(OPT) $(CXXSTD) $(DEBUG)
TGT_CXXFLAGS	+= $(ARCH_FLAGS)
TGT_CXXFLAGS	+= -Wextra -Wshadow -Wredundant-decls -Weffc++
TGT_CXXFLAGS	+= -fno-common -ffunction-sections -fdata-sections
TGT_CXXFLAGS	+= -std=c++11

# C & C++ preprocessor common flags
TGT_CPPFLAGS	+= -MD
TGT_CPPFLAGS	+= -Wall -Wundef -pedantic
TGT_CPPFLAGS	+= $(DEFS)
TGT_CPPFLAGS	+= -Wno-unused-parameter
#TGT_CPPFLAGS	+= -fanalyzer -Wno-analyzer-out-of-bounds

# Linker flags
TGT_LDFLAGS		+= --static -nostartfiles -nostdlib
TGT_LDFLAGS		+= -T$(LDSCRIPT)
TGT_LDFLAGS		+= $(ARCH_FLAGS) $(DEBUG)
TGT_LDFLAGS		+= -Wl,-Map=$(OBJDIR)/$*.map -Wl,--cref
TGT_LDFLAGS		+= -Wl,--gc-sections
ifeq ($(V),99)
TGT_LDFLAGS		+= -Wl,--print-gc-sections
endif

#TGT_CFLAGS	+= -g
#TGT_LDFLAGS	+= -g

# Used libraries
DEFS		+= -I$(OPENCM3_DIR)/include
LDFLAGS		+= -L$(OPENCM3_DIR)/lib
LDLIBS		+= -l$(LIBNAME)
LDLIBS		+= -Wl,--start-group -lc -lgcc -lnosys -Wl,--end-group

.SUFFIXES: .elf .bin .hex .srec .list .map .images
.SECONDARY:

all: bin size
size: $(BINARY).size
elf: $(BINARY).elf
bin: $(BINARY).bin
hex: $(BINARY).hex
srec: $(BINARY).srec
list: $(BINARY).list
flashbmp: $(BINARY).flashbmp

GENERATED_BINARIES=$(BINARY).elf $(BINARY).bin $(BINARY).hex $(BINARY).srec $(BINARY).list $(BINARY).map $(UBINARY).elf $(UBINARY).bin

$(OBJDIR)/%.bin: $(OBJDIR)/%.elf
	@echo Building $@
	$(QUIET)$(OBJCOPY) -Obinary $< $@

$(OBJDIR)/%.hex: $(OBJDIR)/%.elf
	@echo Building $@
	$(QUIET)$(OBJCOPY) -Oihex $< $@

$(OBJDIR)/%.srec: $(OBJDIR)/%.elf
	@echo Building $@
	$(QUIET)$(OBJCOPY) -Osrec $< $@

$(OBJDIR)/%.list: $(OBJDIR)/%.elf
	@echo Building $@
	$(QUIET)$(OBJDUMP) -S $< > $@

$(OBJDIR)/%.map: $(OBJDIR)/%.elf

DFUBIN_OBJ := $(OBJDIR)/usbdfu_bin.o
$(BINARY).elf: $(DFUBIN_OBJ)
$(OBJDIR)/%.elf: $(OBJS) $(LDSCRIPT) $(OPENCM3_LIB)
	@echo Building $@
	$(QUIET)$(LD) $(TGT_LDFLAGS) $(LDFLAGS) $(OBJS) $(DFUBIN_OBJ) $(LDLIBS) -o $@

# Stand-alone program usbdfu:
# usbdfu.c CC-> usbdfu.o LINK-> usbdfu.elf OC-> usbdfu.bin OC->
# usbdfu_bin_temp.o OC-> usbdfu_bin.o   which gets linked with
# $(OBJS) to finally create fw.elf and fw.bin
$(UBINARY).elf: $(UOBJS) usbdfu.ld $(OPENCM3_LIB)
	@echo Building $@
	$(QUIET)$(LD) --static -nostartfiles -nostdlib -Tusbdfu.ld -mthumb -mcpu=cortex-m3 -msoft-float -ggdb3 -Wl,-Map=$(UBINARY).map -Wl,--cref -Wl,--gc-sections -L$(OPENCM3_DIR)/lib $(UOBJS) $(LDLIBS) -o $@

$(DFUBIN_OBJ): $(UBINARY).bin Makefile
	@echo Building $@
	$(QUIET)$(OBJCOPY) -I binary -O elf32-littlearm -B arm $< $<.t.o $(VERBOSE)
	$(QUIET)$(OBJCOPY) --rename-section .data=.rodata $<.t.o $@ $(VERBOSE)

# Get argument options for objcopy -O
#     arm-none-eabi-ld --print-output-format
# Get argument options for objcopy -B
#     arm-none-eabi-ld --verbose | sed -n 's/OUTPUT_ARCH(\([^()*]*\))/\1/p'
# Show sections and symbols in usbdfu_bin.o
#     arm-none-eabi-objdump -x objs/usbdfu_bin.o

$(OBJDIR)/%.o: %.c
	@echo Building $@
	$(QUIET)$(CC) $(TGT_CFLAGS) $(CFLAGS) $(TGT_CPPFLAGS) $(CPPFLAGS) -o $@ -c $(*).c

$(OBJDIR)/%.o: %.cxx
	@echo Building $@
	$(QUIET)$(CXX) $(TGT_CXXFLAGS) $(CXXFLAGS) $(TGT_CPPFLAGS) $(CPPFLAGS) -o $@ -c $(*).cxx

$(OBJDIR)/%.o: %.cpp
	@echo Building $@
	$(QUIET)$(CXX) $(TGT_CXXFLAGS) $(CXXFLAGS) $(TGT_CPPFLAGS) $(CPPFLAGS) -o $@ -c $(*).cpp

$(OBJDIR)/%.size: $(OBJDIR)/%.elf
	@echo "Output code size:"
	@$(SIZE) -A -d $< | grep -Ee 'text|data|bss' | awk ' \
    function human(x) { \
        if (x<1000) {return x} else {x/=1024} \
        s="kMGTEPZY"; \
        while (x>=1000 && length(s)>1) \
            {x/=1024; s=substr(s,2)} \
        return int(x+0.5) substr(s,1,1) \
    } \
	{printf("%10s %8s\n", $$1, human($$2))} \
'
$(OBJDIR)/version.o: $(filter-out $(OBJDIR)/version.o, $(OBJS))

$(OBJS) $(UOBJS): Makefile $(OPENCM3_HEADER) | $(OBJDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OPENCM3_DIR)/Makefile:
	@echo Cloning $@
	$(QUIET)git clone https://github.com/libopencm3/libopencm3

$(OPENCM3_DIR)/include/%: $(OPENCM3_DIR)/Makefile
	@echo Building $@
	$(QUIET)make -C $(OPENCM3_DIR) -j8

$(OPENCM3_LIB): $(OPENCM3_HEADER)

#	@echo Building $@
#	make -C $(OPENCM3_DIR) -j8

%.flashbmp: %.elf
	$(GDB) --batch \
		-ex 'target extended-remote $(BMP_PORT)' \
		-x bmp_flash.scr \
		$(OBJDIR)/$*.elf

clean clean-all:
	@echo Cleaning
	$(QUIET)$(RM) $(GENERATED_BINARIES) generated.* $(OBJS) $(OBJS:%.o=%.d) $(UOBJS)

# Dependencies
-include $(OBJS:.o=.d)

UDEV_DIR        := /etc/udev/rules.d
UDEV_FILENAMES  := 70-st-link.rules 70-ttymxprog.rules
UDEV_FILE_PATHS := $(UDEV_FILENAMES:%=$(UDEV_DIR)/%)
$(UDEV_FILE_PATHS) &:
ifneq ($(OS),Darwin)
	sudo cp -np udev/* $(UDEV_DIR)
	sudo udevadm control --reload
	sudo udevadm trigger
endif
udev-files: $(UDEV_FILE_PATHS)

# Write STM32 flash using DFU mode
just-dfu:
	$(DFU_UTIL) --device 0483:df11 --alt 0 --download $(BINARY).bin --dfuse-address 0x08000000:leave

dfu-unprotect: $(UDEV_FILE_PATHS)
	$(DFU_UTIL) -s 0:force:unprotect -a 0 --device 0483:df11 --alt 0 --download $(BINARY).bin

dfu-clobber: $(UDEV_FILE_PATHS)
	$(DFU_UTIL) -s 0:force:unprotect --alt 1 --download flash_clobber.bin -Z 0x10 --dfuse-address 0x1ffff800:leave

#dfu-clobber: $(UDEV_FILE_PATHS)
#	$(DFU_UTIL) -s 0:force:unprotect --alt 1 --download flash_clobber.bin -Z 0xd --dfuse-address 0x1ffff800:leave

# Write STM32 flash using ST-Link
just-flash: $(ST_TOOLS_PATH)/st-flash | $(BINARY).bin $(UDEV_FILE_PATHS)
	$(ST_TOOLS_PATH)/st-flash $(ST_ARGS) --reset write $(BINARY).bin 0x08000000

# Erase STM32 flash using ST-Link
just-erase: $(ST_TOOLS_PATH)/st-flash | $(UDEV_FILE_PATHS)
	$(ST_TOOLS_PATH)/st-flash $(ST_ARGS) --flash=0x100000 erase 0x08000000 0x3c000

# Protect/unprotect/clobber flash by writing the option byte area
# The following can also be done by the STM32CubeProgrammer.
# F105 unlocked  1ffff800: a5 5a 07 f8 00 ff 00 ff ff 00 ff 00 ff 00 ff 00
# F105 protect   1ffff800: 00 ff 07 f8 ff 00 ff 00 00 ff 00 ff 00 ff 00 ff
# F105 clobber   1ffff800: 00 00 00 00 00 03 00 03 00 ff 00 ff 00 ff 00 ff

# Select specific programmer ("make stinfo" to get serial)
# export ST_ARGS="--serial 57FF6E064975545225502187"		# amiga1
# export ST_ARGS="--serial 303030303030303030303031"		# amiga2

just-unprotect: $(ST_TOOLS_PATH)/st-flash | $(UDEV_FILE_PATHS)
	$(ST_TOOLS_PATH)/st-flash $(ST_ARGS) --reset write flash_unprotect.bin 0x1ffff800
just-protect: $(ST_TOOLS_PATH)/st-flash | $(UDEV_FILE_PATHS)
	$(ST_TOOLS_PATH)/st-flash $(ST_ARGS) --reset write flash_protect.bin 0x1ffff800
just-clobber: $(ST_TOOLS_PATH)/st-flash | $(UDEV_FILE_PATHS)
	$(ST_TOOLS_PATH)/st-flash $(ST_ARGS) --reset write flash_clobber.bin 0x1ffff800

dfu: all $(UDEV_FILE_PATHS) just-dfu
flash: all just-flash

# Connect to remote STM32 via ST-Link (follow with "make gdb")
stlink: $(ST_TOOLS_PATH)/st-flash | $(UDEV_FILE_PATHS)
	$(ST_TOOLS_PATH)/st-util $(ST_ARGS) --no-reset

stinfo reset: $(ST_TOOLS_PATH)/st-flash | $(UDEV_FILE_PATHS)
	$(ST_TOOLS_PATH)/st-info --probe

# Get ST-TOOLS
$(ST_BUILD_DIR) get-stutils:
	git clone https://github.com/texane/stlink.git stutils

# Build ST-TOOLS
$(ST_TOOLS_PATH)/st-flash build-stutils: | $(ST_BUILD_DIR)
	make -C $(ST_BUILD_DIR) CMAKEFLAGS="-DCMAKE_INSTALL_PREFIX=. -DCMAKE_INSTALL_FULL_DATADIR=." -j4
	ln -s ../.. $(ST_BUILD_DIR)/build/Release/stlink
	ln -s . $(ST_BUILD_DIR)/build/Release/share

gdb:
	gdb -q -x .gdbinit $(BINARY).elf

.PHONY: images clean get-stutils build_stutils stlink dfu flash just-dfu just-flash just-unprotect just-dfu dfu-unprotect size elf bin hex srec list udev-files verbose
