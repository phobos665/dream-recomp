/* Compiler-generated SH-4 idioms for the differential harness (docs/differential-harness.md).
 * Built freestanding with the KOS sh-elf-gcc (see Makefile) so the harness sees GCC's own code
 * shapes: libgcc division helpers, jump tables for switches, struct copies, loops, float ABI.
 * Every function is a leaf or calls only functions in this file; no globals, so the raw image has no
 * .data. Pointer arguments must point into the harness fill region. */
#include <stdint.h>

/* Division and modulo go through libgcc's __sdivsi3/__udivsi3/__sdivsi3_i4 etc. */
int32_t div_mod(int32_t a, int32_t b) {
    if (b == 0) return -1;
    return a / b + (a % b) * 3;
}

uint32_t udiv_mix(uint32_t a, uint32_t b) {
    if (b == 0) return 0xFFFFFFFFu;
    return (a / b) ^ (a % b) ^ (a / 10u) ^ (a % 7u);
}

/* Dense switch: GCC emits a jump table (mova/braf or jmp) for this shape. */
int32_t dense_switch(int32_t sel, int32_t x) {
    switch (sel) {
    case 0: return x + 1;
    case 1: return x * 3;
    case 2: return x >> 2;
    case 3: return (int32_t)((uint32_t)x << 5);
    case 4: return -x;
    case 5: return x ^ 0x55;
    case 6: return x | 0x100;
    case 7: return x & 0xFF;
    case 8: return x - 1000;
    case 9: return x * x;
    default: return 0x7FFFFFFF;
    }
}

/* 64-bit arithmetic: dmuls/dmulu, addc/subc chains. */
uint64_t mul64(uint32_t a, uint32_t b) { return (uint64_t)a * b + ((uint64_t)a << 32) - b; }

int64_t sub64(int64_t a, int64_t b) { return a - b * 5 + (a >> 7); }

/* Sign/zero extension, byte/word memory access, rotates through shifts. */
uint32_t pack_bytes(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < (n & 63u); ++i) {
        h ^= p[i];
        h *= 16777619u;
        h = (h << 13) | (h >> 19);
    }
    return h ^ (uint32_t)(int32_t)(int16_t)p[1] ^ (uint32_t)(int32_t)(int8_t)p[2];
}

typedef struct {
    int32_t x, y, z;
    int16_t w;
    uint8_t flags;
    uint8_t pad;
} Vec;

/* Struct copy and field arithmetic; writes through pointers. */
void vec_transform(Vec* out, const Vec* in, int32_t k) {
    Vec t = *in;
    t.x = t.x * k + t.y;
    t.y = t.y - t.z * k;
    t.z = (t.z << 1) | (t.flags & 1);
    t.w = (int16_t)(t.w + k);
    t.flags ^= 0xA5;
    *out = t;
    out[1] = *in;
}

/* Insertion sort over words in place. */
void sort_words(uint32_t* a, uint32_t n) {
    n &= 31u;
    for (uint32_t i = 1; i < n; ++i) {
        uint32_t v = a[i];
        uint32_t j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = v;
    }
}

/* Float ABI (fr4.. arguments, fr0 result), compares, conversions, fmac shapes. */
float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

float clamp_scale(float v, float lo, float hi, int32_t k) {
    float s = (float)k * 0.25f;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v * s - (float)(int32_t)v;
}

int32_t float_to_fixed(float v) { return (int32_t)(v * 65536.0f); }

/* Recursion and multiple call sites: exercises bsr/jsr/rts and the stack. */
uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

/* Byte string routines the compiler is not allowed to replace (-fno-builtin). */
uint32_t str_len(const char* s) {
    uint32_t n = 0;
    while (s[n] && n < 4096) ++n;
    return n;
}

void mem_copy(uint8_t* d, const uint8_t* s, uint32_t n) {
    n &= 1023u;
    while (n--) *d++ = *s++;
}
