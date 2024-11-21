#
# Makefile to build KickSmash firmware, host software, and Amiga software
#

VER ?= 1.1+
MAKE ?= make
MAKEFLAGS += --no-print-directory

all: build-sw build-fw build-amiga
clean: clean-sw clean-fw clean-amiga

build-fw build-amiga build-sw:
	@echo
	@echo "* Building in $(@:build-%=%)"
	@$(MAKE) -C $(@:build-%=%) all

clean-sw clean-fw clean-amiga:
	@echo
	@echo "* Cleaning in $(@:clean-%=%)"
	@$(MAKE) -C $(@:clean-%=%) clean-all

# ---------------------------------------------------------------

RELEASE_DIR := kicksmash_$(VER)
RELEASE_LHA := kicksmash_$(VER).lha
RELEASE_ZIP := kicksmash_$(VER).zip

RELEASE_TARGETS :=
RELEASE_DIRS :=
define RELEASE_IT
ifneq (,$(wildcard $(1)))
RELEASE_TARGETS += $(RELEASE_DIR)/$(2)
RELEASE_DIRS += $(dir $(RELEASE_DIR)/$(2))
$(RELEASE_DIR)/$(2): $(1)
endif
endef
$(eval $(call RELEASE_IT,sw/objs.x86_64/hostsmash,sw/hostsmash.linux_x86_64))
$(eval $(call RELEASE_IT,sw/objs.mac/hostsmash,sw/hostsmash.mac))
$(eval $(call RELEASE_IT,sw/objs.armv7l/hostsmash,sw/hostsmash.pi32))
$(eval $(call RELEASE_IT,sw/objs.win32/hostsmash.exe,sw/hostsmash_win32.exe))
$(eval $(call RELEASE_IT,sw/objs.win64/hostsmash.exe,sw/hostsmash_win64.exe))
$(eval $(call RELEASE_IT,fw/objs/fw.bin,fw/fw.bin))
$(eval $(call RELEASE_IT,fw/objs/fw.elf,fw/fw.elf))
$(eval $(call RELEASE_IT,fw/Makefile.release,fw/Makefile))
$(eval $(call RELEASE_IT,fw/.gdbinit,fw/.gdbinit))
$(eval $(call RELEASE_IT,amiga/smash,amiga/smash))
$(eval $(call RELEASE_IT,amiga/smashftp,amiga/smashftp))
$(eval $(call RELEASE_IT,amiga/smashfs,amiga/smashfs))
$(eval $(call RELEASE_IT,amiga/smashfsrom,amiga/smashfsrom))
$(eval $(call RELEASE_IT,amiga/smashfsrom_d,amiga/smashfsrom_d))
$(eval $(call RELEASE_IT,README.md,README.md))
$(eval $(call RELEASE_IT,LICENSE.md,LICENSE.md))
$(foreach DOC,$(wildcard doc/*),$(eval $(call RELEASE_IT,$(DOC),$(DOC))))

RELEASE_DIRS := $(sort $(RELEASE_DIR) $(RELEASE_DIRS))

ifneq (,$(wildcard $(RELEASE_LHA))$(wildcard $(RELEASE_ZIP)))
release:
	@echo $(RELEASE_LHA) or $(RELEASE_ZIP) already exists
else
release: all
	@$(MAKE) do_release

do_release: populating $(RELEASE_TARGETS) $(RELEASE_LHA) $(RELEASE_ZIP)

$(RELEASE_LHA): all populating $(RELEASE_TARGETS)
	@echo "* Building $@"
	@rm -f $@
	@lha -aq2 $@ $(RELEASE_DIR)

$(RELEASE_ZIP): all populating $(RELEASE_TARGETS)
	@echo "* Building $@"
	@rm -f $@
	@zip -rq $@ $(RELEASE_DIR)

populating:
	@echo "* Populating $(RELEASE_DIR)"

$(RELEASE_TARGETS): | $(RELEASE_DIRS)
	cp -p $^ $@
endif
$(RELEASE_DIRS):
	mkdir -p $@

.PHONY: build-sw build-fw build-amiga clean-sw clean-fw clean-amiga all clean release populating
