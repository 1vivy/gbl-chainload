CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11
PROJ    := $(realpath ../..)
DPL     := $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib
GBL_MODE ?= 1

CFLAGS += -D__HOST_BUILD__ -DGBL_MODE=$(GBL_MODE) \
          -I$(PROJ)/GblChainloadPkg/Include/Library \
          -I$(DPL)/Internal

SRCS := abl-patcher.c \
        $(DPL)/Internal/ScanLib.c \
        $(DPL)/Internal/PeSections.c \
        $(DPL)/Internal/Encode.c \
        $(DPL)/Internal/Arm64Decode.c \
        $(DPL)/Internal/PatchEngine.c \
        $(DPL)/PatchTable.c \
        $(DPL)/universal/universal.c \
        $(DPL)/oem/oneplus_canoe.c \
        $(DPL)/mode_1/mode_1.c

abl-patcher: $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f abl-patcher *.o

# Quick self-check: build and exercise against the infiniti fixture.
test: abl-patcher
	./abl-patcher --in $(PROJ)/images/infiniti/LinuxLoader_infiniti.efi --check-anchors-only
	./abl-patcher --in $(PROJ)/images/infiniti/LinuxLoader_infiniti.efi --out /tmp/patched-infiniti.bin
	@echo "ok abl-patcher self-test"
