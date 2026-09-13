# SH-4 translator tests

- `oracle/`: `sh-elf-objdump` output for all 65,536 instruction words; the decoder test compares
  against it (`translator/tests/test_decoder.cpp`).
- `cprogs/`: C programs compiled freestanding with the KOS GCC (`tools/docker/run.sh 'make -C
  tests/sh4/cprogs'`), then `python3 tests/sh4/cprogs/gen_specs.py --translate <dream-translate>
  --name c1_idioms --bin ... --syms ...` prints the `programs.cmake` entry from the translator's
  discovery. Compiler idioms for the differential harness.
- `programs/`: small hand-written SH-4 programs, assembled in the toolchain container, translated
  to C++ at build time by `dream-translate`, compiled into `dream_emit_tests`, and executed against
  hand-computed expectations (`tests/sh4/test_programs.cpp`). This is the translate-then-run
  pipeline the differential harness (WP1.5) will drive with KallistiOS programs.

Rebuild the program binaries after editing a `.s`:

```
tools/docker/run.sh 'cd tests/sh4/programs && for s in p1_sum p2_alu p3_mem p4_calls p5_fpu p6_switch p7_fpuedge p8_split p9_rte p11_condfall; do
  /opt/toolchains/dc/sh-elf/bin/sh-elf-as -little $s.s -o /tmp/$s.o &&
  /opt/toolchains/dc/sh-elf/bin/sh-elf-ld -EL -Ttext=0x8c010000 -e 0x8c010000 /tmp/$s.o -o /tmp/$s.elf &&
  /opt/toolchains/dc/sh-elf/bin/sh-elf-objcopy -O binary -j .text /tmp/$s.elf $s.bin &&
  /opt/toolchains/dc/sh-elf/bin/sh-elf-nm /tmp/$s.elf | grep " T _"; done'
```

`p10_longjmp` is linked at `-Ttext=0x8c030000 -e 0x8c030000` instead: its non-local return resumes
through the function table by address, so it must not share a base with the other programs linked
into the same test binary (`programs.cmake` carries the base per program).

Then update the function ranges in `programs/programs.cmake`. New programs are picked up by both
`dream_emit_tests` and the differential runner; add a case to `tools/oracle/cases.json` and, once
the oracle agrees, its captured state as the program's expectations in `test_programs.cpp`.
