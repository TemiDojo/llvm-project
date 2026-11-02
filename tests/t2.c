// Classic overflow
// - Last byte of buf2 overflows to buf1
// - Expected output: exit 134 (VarGuard catches it)
void test1(void) {
	char buf1[8];
	char buf2[8];
	for (int i = 0; i <= sizeof(buf2); i++) {
		buf2[i] = 'A';
	}
}

/*
// No overflow
// - Expected output: exit 0
void test2(void) {
	char buf1[8];
	char buf2[8];
	for (int i = 0; i < sizeof(buf2); i++) {
		buf2[i] = 'A';
	}
}

// Harmless overflow into padding
// - Writes beyond buf2 BUT doesn't overflow into buf1,
//   writes into padding instead
// - 5-byte array in 8-byte alignment -> 3 bytes of padding
// - Direct out-of-bounds write (the compiler sees the bug)
// - Expected output: exit 0
void test3(void) {
	char buf1[8];
	char buf2[5];
	buf2[5] = 'A';
}

// Reverse overflow
// - Writes beyond buf2 but overflows off the other end,
//   not touching buf1
// - Direct out-of-bounds write (the compiler sees the bug)
// - Expected output: exit 0
void test4(void) {
	char buf1[8];
	char buf2[8];
	buf2[-1] = 'A';
}
*/

int main(void) {
	test1(); // Classic overflow (loop)
	// test2(); // No overflow	(loop)
	// test3(); // Harmless overflow into padding   (direct write)
	// test4(); // Reverse overflow 		(direct write)	
}

