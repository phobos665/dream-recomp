# Built inside the toolchain container (tools/docker), where KOS_BASE and the sh-elf toolchain are
# preset. `make` produces hello.elf and hello.bin (the raw 1ST_READ.BIN form, linked at 0x8C010000).
TARGET = hello.elf
OBJS = hello.o

include $(KOS_BASE)/Makefile.rules

all: rm-elf $(TARGET) hello.bin

clean: rm-elf
	-rm -f $(OBJS) hello.bin

rm-elf:
	-rm -f $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS)

hello.bin: $(TARGET)
	$(KOS_OBJCOPY) -R .stack -O binary $(TARGET) $@
