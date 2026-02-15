EE_BIN = superpsx.elf
EE_OBJS = src/main.o src/memory.o src/cpu.o src/hardware.o src/graphics.o src/dynarec.o src/gte.o src/cdrom.o src/loader.o src/joystick.o
EE_LIBS = -lpatches -lps2_drivers -ldebug -lgraph -ldma -ldraw -lmath3d

# Remove -DENABLE_VRAM_DUMP to disable VRAM dumping (improves performance)
ENABLE_VRAM_DUMP ?= 1
ENABLE_HOST_LOG ?= 1

EE_CFLAGS = -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include -I$(PS2SDK)/ports/include -Iinclude -O2 -G0 -Wall
EE_LDFLAGS = -L$(PS2SDK)/ee/lib -L$(PS2SDK)/ports/lib

# Allow passing game arguments to PCSX2 via `make run GAMEARGS="arg1 arg2"`
GAMEARGS ?=
ifneq ($(strip $(GAMEARGS)),)
	# Prefix the current working directory so the guest sees PWD as argv[1]
	PCSX2_GAMEARGS = -gameargs "host: $(GAMEARGS)"
else
	PCSX2_GAMEARGS =
endif

ifeq ($(ENABLE_HOST_LOG), 1)
	EE_CFLAGS += -DENABLE_HOST_LOG
endif

ifeq ($(ENABLE_VRAM_DUMP), 1)
	EE_CFLAGS += -DENABLE_VRAM_DUMP
endif

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

run: $(EE_BIN)
	/Applications/PCSX2.app/Contents/MacOS/PCSX2 -batch -earlyconsolelog -elf "$(shell pwd)/$(EE_BIN)" $(PCSX2_GAMEARGS)

reset:
	ps2client reset

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
