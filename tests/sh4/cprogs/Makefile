# Built inside the toolchain container: tools/docker/run.sh 'make -C tests/sh4/cprogs'
# Freestanding, single-precision SH-4 code linked flat at 0x8C010000 with libgcc for the division
# helpers. The .syms file (nm output) feeds gen_specs.py, which prints the programs.cmake entry.
CC = /opt/toolchains/dc/sh-elf/bin/sh-elf-gcc
NM = /opt/toolchains/dc/sh-elf/bin/sh-elf-nm
OBJCOPY = /opt/toolchains/dc/sh-elf/bin/sh-elf-objcopy
CFLAGS = -O2 -ml -m4-single-only -ffreestanding -fno-builtin -fno-strict-aliasing -fomit-frame-pointer \
         -Wall -Wextra -fno-asynchronous-unwind-tables
LDFLAGS = -nostdlib -Wl,-Ttext=0x8c010000 -Wl,--build-id=none

PROGS = c1_idioms

all: $(PROGS:=.bin) $(PROGS:=.syms)

%.elf: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-e,_div_mod -o $@ $< -lgcc

%.bin: %.elf
	$(OBJCOPY) -O binary -j .text -j .rodata -j .rodata.* -j .data $< $@

%.syms: %.elf
	$(NM) -n $< > $@

# programs.cmake entry: run gen_specs.py on the host with the built translator, e.g.
#   python3 tests/sh4/cprogs/gen_specs.py --translate build/dream/translator/dream-translate \
#       --name c1_idioms --bin tests/sh4/cprogs/c1_idioms.bin --syms tests/sh4/cprogs/c1_idioms.syms

clean:
	rm -f $(PROGS:=.elf) $(PROGS:=.bin) $(PROGS:=.syms)
