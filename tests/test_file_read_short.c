/*
	Regression test for CR-07 (docs/audit_2026-08-20_code_review.md): source/
	util.c's file_read() used to discard fread()'s return value entirely
	(`(void)!fread(res, length, 1, f);`), so a short/truncated read silently
	handed callers a partially-filled-but-"successful" buffer -- res[length]
	is still NUL, so it even LOOKED like a valid (if short) C string. The
	fix checks fread()'s return against the single-item (nmemb==1) contract
	and returns NULL on anything but a complete read.

	SCOPE / WHAT THIS FILE DOES NOT ATTEMPT: file_read() performs fopen(),
	fseek(SEEK_END)/ftell()/fseek(SEEK_SET), then a single fread() -- all as
	one opaque, effectively-atomic sequence inside the function, with no
	injection seam a test can hook. The only way file_read()'s internal
	ftell()-measured length can legitimately EXCEED what fread() actually
	delivers is if the file shrinks between that ftell() and the fread()
	call microseconds later -- a genuine race against a concurrent writer/
	truncator. Reproducing that deterministically (not flakily) would
	require either OS-specific fault injection or adding a test-only I/O
	seam to production code, neither of which this fix's scope calls for.
	This file therefore:
	  (1) exercises the REAL, PRODUCTION file_read() end-to-end for every
	      behavior that IS deterministically reachable from outside the
	      function -- a full successful read (byte-exact, still NUL-
	      terminated, matching item 4's "behavior-preserving for successful
	      reads" requirement), a missing file, and the zero-length-file edge
	      case the `length > 0` guard exists for -- and
	  (2) directly proves the DETECTION CONDITION the fix relies on --
	      `fread(ptr, size, 1, f) != 1` reliably signals a short/incomplete
	      read and a full read reliably signals success -- using the exact
	      same standard-library idiom against a deterministically (not
	      racily) constructed short read: requesting more bytes than remain
	      after seeking partway through an already-written file. This is the
	      load-bearing fact the file_read() fix depends on; combined with
	      (1)'s regression coverage and direct code-path inspection of
	      util.c's file_read(), it substitutes for an unreliable, flaky race
	      test against the live race window described above.

	The GL-facing half of CR-07 (source/platform/pc/gfx.c: shader compile/
	link failure propagation) cannot run headless in this suite (no GL
	context) and is covered by code-path inspection only, per this fix's
	task instructions.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#include "util.h"

// Writes `len` bytes from `data` to `path`, truncating any existing file.
static void write_fixture(const char* path, const void* data, size_t len) {
	FILE* f = fopen(path, "wb");
	assert(f);
	if(len > 0)
		assert(fwrite(data, 1, len, f) == len);
	assert(fclose(f) == 0);
}

// (1a) A full, successful read: file_read() must still return a byte-exact,
// NUL-terminated copy of the file -- proves the fix does not disturb the
// success path (task item 4's "behavior-preserving for successful reads").
static void test_full_read_roundtrip(void) {
	char path[128];
	test_fixture_dir("test_file_read_short_roundtrip.bin", path, sizeof path);
	// Deliberately include a zero byte mid-content and non-ASCII bytes: proves
	// the returned buffer is a byte-exact copy (via memcmp over the known
	// length), not something relying on strlen()/text semantics.
	uint8_t content[] = {'h', 'i', 0x00, 0xFF, 0x7F, 'X', 'Y', 'Z'};
	write_fixture(path, content, sizeof(content));

	void* res = file_read(path);
	assert(res);
	assert(memcmp(res, content, sizeof(content)) == 0);
	// file_read() NUL-terminates one byte past the file's real length.
	assert(((const char*)res)[sizeof(content)] == 0);

	free(res);
	assert(remove(path) == 0);
	printf("  full read: %zu bytes byte-exact + NUL-terminated OK\n",
		   sizeof(content));
}

// (1b) A missing file must return NULL, not crash or fabricate a buffer.
static void test_missing_file_returns_null(void) {
	char path[128];
	test_fixture_dir("test_file_read_short_does_not_exist.bin", path, sizeof path);
	remove(path); // best-effort; ignore result in case it never existed
	void* res = file_read(path);
	assert(!res);
	printf("  missing file -> NULL OK\n");
}

// (1c) The `length > 0` guard added by the fix must not misfire on a
// legitimately empty file: fread()'s own "nmemb/size == 0" case returns 0
// without that meaning failure, so file_read() must special-case it and
// still return a valid (empty) NUL-terminated buffer, not NULL.
static void test_empty_file_returns_empty_buffer(void) {
	char path[128];
	test_fixture_dir("test_file_read_short_empty.bin", path, sizeof path);
	write_fixture(path, "", 0);

	void* res = file_read(path);
	assert(res);
	assert(((const char*)res)[0] == 0);

	free(res);
	assert(remove(path) == 0);
	printf("  zero-byte file -> non-NULL empty buffer (not NULL) OK\n");
}

// (2) Direct proof of the detection condition file_read()'s fix relies on:
// fread(ptr, size, 1, f) reliably distinguishes a complete read (returns 1)
// from a short/incomplete one (returns 0), using the SAME standard-library
// call idiom production file_read() now uses, against a deterministically
// (not racily) constructed short read -- request more bytes than remain
// after seeking partway into an already-written file, rather than trying to
// race a concurrent truncation against the live function (see file header).
static void test_fread_short_read_detection_idiom(void) {
	char path[128];
	test_fixture_dir("test_file_read_short_idiom.bin", path, sizeof path);
	const char payload[] = "0123456789"; // 10 bytes, no trailing NUL counted
	write_fixture(path, payload, 10);

	FILE* f = fopen(path, "rb");
	assert(f);

	// Seek to byte 6: exactly 4 bytes ("6789") remain in the file.
	assert(fseek(f, 6, SEEK_SET) == 0);

	// Request MORE than remains (6 bytes, only 4 available): this is a
	// deterministic short read, not a race -- fread() must report it by
	// returning something other than the single complete item requested.
	char short_buf[6] = {0};
	size_t short_result = fread(short_buf, 6, 1, f);
	assert(short_result != 1); // this is the exact condition file_read() checks
	assert(feof(f)); // confirms the short read was a genuine EOF, not a fluke

	fclose(f);

	// Now request EXACTLY what remains from a fresh handle: fread() must
	// report success (1) for the complementary, non-short case.
	f = fopen(path, "rb");
	assert(f);
	assert(fseek(f, 6, SEEK_SET) == 0);
	char exact_buf[4] = {0};
	size_t exact_result = fread(exact_buf, 4, 1, f);
	assert(exact_result == 1); // full item read -> NOT flagged as a short read
	assert(memcmp(exact_buf, "6789", 4) == 0);
	fclose(f);

	assert(remove(path) == 0);
	printf("  fread(ptr,size,1,f) idiom: short request -> %zu (!=1, rejected), "
		   "exact request -> %zu (==1, accepted) OK\n",
		   short_result, exact_result);
}

int main(void) {
	printf("test_file_read_short:\n");
	test_full_read_roundtrip();
	test_missing_file_returns_null();
	test_empty_file_returns_empty_buffer();
	test_fread_short_read_detection_idiom();
	printf("PASS test_file_read_short\n");
	return 0;
}
