EE_BIN = superpsx.elf
EE_OBJS = src/main.o src/memory.o src/cpu.o src/hardware.o src/graphics.o src/dynarec.o src/gte.o src/cdrom.o src/loader.o
EE_LIBS = -lpatches -lps2_drivers -ldebug -lgraph -ldma -ldraw -lmath3d

# Remove -DENABLE_VRAM_DUMP to disable VRAM dumping (improves performance)
EE_CFLAGS = -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include -I$(PS2SDK)/ports/include -Iinclude -O2 -G0 -Wall -DENABLE_HOST_LOG -DENABLE_VRAM_DUMP
EE_LDFLAGS = -L$(PS2SDK)/ee/lib -L$(PS2SDK)/ports/lib

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

run: $(EE_BIN)
	/Applications/PCSX2.app/Contents/MacOS/PCSX2 -batch -earlyconsolelog -elf "$(shell pwd)/$(EE_BIN)"

reset:
	ps2client reset

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
