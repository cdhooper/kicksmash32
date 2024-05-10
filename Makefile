#
# Makefile to build KickSmash firmware, host software, and Amiga software
#

MAKE ?= make
MAKEFLAGS += --no-print-directory

all: build-sw build-fw build-amiga
clean: clean-sw clean-fw clean-amiga

build-sw build-fw build-amiga:
	@echo
	@echo Building in $(@:build-%=%)
	@$(MAKE) -C $(@:build-%=%)

clean-sw clean-fw clean-amiga:
	@echo
	@echo Cleaning in $(@:clean-%=%)
	@$(MAKE) -C $(@:clean-%=%) clean

.PHONY: build-sw build-fw build-amiga clean-sw clean-fw clean-amiga all clean
