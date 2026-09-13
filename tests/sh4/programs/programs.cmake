# Test programs assembled in the toolchain container (see *.s); each entry is
#   name|bin|base|function-specs (space separated ENTRY:END:name)
# Regenerate the .bin files with tools/docker/run.sh and the commands in tests/sh4/README.md.
set(DREAM_SH4_PROGRAMS
  "p1_sum|p1_sum.bin|0x8c010000|0x8c010000:0x8c010014:sum"
  "p2_alu|p2_alu.bin|0x8c010000|0x8c010000:0x8c0100bc:alu"
  "p3_mem|p3_mem.bin|0x8c010000|0x8c010000:0x8c010034:mem"
  "p5_fpu|p5_fpu.bin|0x8c010000|0x8c010000:0x8c010068:fpu"
  "p6_switch|p6_switch.bin|0x8c010000|0x8c010000:0x8c01005e:sw"
  "p4_calls|p4_calls.bin|0x8c010000|0x8c010000:0x8c010022:calls 0x8c010022:0x8c010026:inc 0x8c010026:0x8c01002c:dbl"
  "p7_fpuedge|p7_fpuedge.bin|0x8c010000|0x8c010000:0x8c0100f0:fpuedge"
  "p8_split|p8_split.bin|0x8c010000|0x8c010000:0x8c010002:split 0x8c010002:0x8c010016:tail"
  "p9_rte|p9_rte.bin|0x8c010000|0x8c010000:0x8c01000c:rtejump 0x8c01000c:0x8c010012:target"
  # Linked at its own base: the non-local return resumes through the function table by address,
  # which must not be shared with the other programs' functions.
  "p10_longjmp|p10_longjmp.bin|0x8c030000|0x8c030000:0x8c030024:main 0x8c030024:0x8c03003c:save 0x8c03003c:0x8c030058:deep 0x8c030058:0x8c030070:restore"
  "p11_condfall|p11_condfall.bin|0x8c010000|0x8c010000:0x8c010006:loop 0x8c010006:0x8c01000c:done"
  "p12_selfcmp|p12_selfcmp.bin|0x8c010000|0x8c010000:0x8c01005c:selfcmp"
  # GCC-compiled C unit (tests/sh4/cprogs, `make specs` prints this line)
  "c1_idioms|../cprogs/c1_idioms.bin|0x8c010000|0x8c010000:0x8c010024:div_mod 0x8c010028:0x8c010072:udiv_mix 0x8c010080:0x8c0100e4:dense_switch 0x8c0100e8:0x8c010100:mul64 0x8c010100:0x8c010132:sub64 0x8c010134:0x8c010178:pack_bytes 0x8c010180:0x8c0101d6:vec_transform 0x8c0101d8:0x8c010222:sort_words 0x8c010224:0x8c010246:dot3 0x8c010248:0x8c01026a:clamp_scale 0x8c010270:0x8c01027c:float_to_fixed 0x8c010280:0x8c010526:fib 0x8c01052c:0x8c010552:str_len 0x8c010554:0x8c010574:mem_copy 0x8c010578:0x8c0106b8:udivsi3_i4i 0x8c01059e:0x8c0105d4:udivsi3_i4i_059e 0x8c0105cc:0x8c0105d4:udivsi3_i4i_05cc 0x8c0105d4:0x8c010648:udivsi3_i4i_05d4 0x8c010648:0x8c010796:sdivsi3_i4i 0x8c010674:0x8c0106b8:sdivsi3_i4i_0674 0x8c01069a:0x8c0106b8:sdivsi3_i4i_069a"
)
