#include <string.h>

/* 
 * Usage
 * Select one test at compile time; only that test is compiled.
 *  Recommended: ./scripts/vg_runner2.sh -o0 -t1
 *  Other options:
 *    clang -std=c2y -O0 -Xclang -disable-O0-optnone -DTEST=1 -fno-stack-protector -S -emit-llvm tests/t2.c -o t2.ll
 *    clang -std=c2y -O1 -DTEST=1 -fno-stack-protector -S -emit-llvm tests/t2.c -o t2.ll
 *
 *
 * Known Limitations
 * These tests do NOT result in a failure/overflow detection.
 * test5() - overflow thru a lib call
 * test6() - overflow thru a ptr alias
 * test8() - overflow inside a struct
 * test9() - overflow past a canary
 *
 */

#ifndef TEST
#error "Select a test with -DTEST=<n>, e.g. clang -DTEST=3 t2.c"
#endif

#if TEST < 1 || TEST > 9
#error "TEST must be between 1 and 9"
#endif

// Keeps each test in its own stack frame at -O1 and above.
// Build with -DNOINLINE=to turn this off.
#ifndef NOINLINE
#define NOINLINE __attribute__((noinline))
#endif

#if TEST == 1
// Classic overflow
// - Last byte of buf2 overflows to buf1
// - Expected output: exit 134 (VarGuard catches it)
NOINLINE void test1(void) {
        char buf1[8];
        char buf2[8];
        for (int i = 0; i <= sizeof(buf2); i++) {
                buf2[i] = 'A';
        }
}
#endif

#if TEST == 2
// No overflow
// - Expected output: exit 0
NOINLINE void test2(void) {
        char buf1[8];
        char buf2[8];
        for (int i = 0; i < sizeof(buf2); i++) {
                buf2[i] = 'A';
        }
}
#endif

#if TEST == 3
// Harmless overflow into padding
// - Writes beyond buf2 BUT doesn't overflow into buf1,
//   writes into padding instead
// - 5-byte array in 8-byte alignment -> 3 bytes of padding
// - Direct out-of-bounds write (the compiler sees the bug)
// - Expected output: exit 0
NOINLINE void test3(void) {
        char buf1[8];
        char buf2[5];
        buf2[5] = 'A';
}
#endif

#if TEST == 4
// Reverse overflow
// - Writes beyond buf2 but overflows off the other end,
//   not touching buf1
// - Direct out-of-bounds write (the compiler sees the bug)
// - Expected output: exit 0
NOINLINE void test4(void) {
        char buf1[8];
        char buf2[8];
        buf2[-1] = 'A';
}
#endif

#if TEST == 5
// Overflow through a library call - memset
// - memset writes one byte past the end of buf2 (the
//   compiler sees the bug)
// - The out-of-bounds write happens inside memset, not as a
//   store in this function
// - Expected output: exit 0 (known limitation)
NOINLINE void test5(void) {
        char buf1[8];
        char buf2[8];
        memset(buf2, 'A', sizeof(buf2) + 1);
}
#endif

#if TEST == 6
// Overflow through a pointer alias
// - Same bug as test1, but writes go through a pointer to
//   buf2 instead of indexing buf2 directly
// - Expected output: exit 0 (known limitation)
NOINLINE void test6(void) {
        char buf1[8];
        char buf2[8];
        char *p = buf2;
        for (int i = 0; i <= sizeof(buf2); i++) {
                p[i] = 'A';
        }
}
#endif

#if TEST == 7
// Integer array overflow
// - Same as test1, but with an int array
// - Expected output: exit 134
NOINLINE void test7(void) {
        int buf1[4];
        int buf2[4];
        for (int i = 0; i <= 4; i++) {
                buf2[i] = 'A';
        }
}
#endif

#if TEST == 8
// Overflow inside a struct
// - Write to coursename field overflows into id field
//   inside the same struct (the compiler sees the bug)
// - Expected output: exit 0 (known limitation)
NOINLINE void test8(void) {
        struct { char coursename[8]; int id; } s;
        s.coursename[8] = 'A';
}
#endif

#if TEST == 9
// Overflow past a canary
// - buf2 (8 bytes) + its canary (8 bytes) = 16 bytes, so
//   buf2[16] lands in buf1, past the canary
// - Direct write (the compiler sees the bug)
// - Expected output: exit 0 (known limitation)
NOINLINE void test9(void) {
        char buf1[8];
        char buf2[8];
        buf2[16] = 'A';
}
#endif

#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)

int main(void) {
        XCAT(test, TEST)();  // expands to testN() when built with -DTEST=N
}

