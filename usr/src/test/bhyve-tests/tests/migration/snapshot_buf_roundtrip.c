/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Unit tests for the vm_snapshot_buf primitives used by every
 * migration validation site.  Every cross-host topology check on
 * the import path (ncpus mismatch, MSI-X table_count mismatch,
 * virtio vc_nvq vs vc_max_nvq, viona nrings vs vc_max_nvq) reads a
 * value through SNAPSHOT_VAR_OR_LEAVE before validating; a
 * regression in the underlying primitives would let those checks
 * pass garbage.
 *
 * struct vm_snapshot_buffer's buf_start and buf_size are declared
 * const, so vm_snapshot_meta cannot be reassigned after creation
 * and each test brace-initialises its own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include <machine/vmm.h>
#include <vmmapi.h>
#include <vmm_snapshot.h>

#define	BUFSZ	256

static int failures;

#define	REQUIRE(cond, msg) do {						\
	if (!(cond)) {							\
		(void) fprintf(stderr,					\
		    "FAIL %s:%d %s: %s\n", __func__, __LINE__,		\
		    #cond, (msg));					\
		failures++;						\
	}								\
} while (0)

/*
 * Save a uint32_t and a uint64_t, then restore them out of the same
 * buffer; values must round-trip byte-identically and the buffer
 * remainder must shrink by exactly the encoded size.
 */
static void
test_var_roundtrip(void)
{
	uint8_t buf[BUFSZ];
	uint32_t src_u32 = 0xdeadbeef;
	uint64_t src_u64 = 0x0123456789abcdefULL;
	uint32_t dst_u32 = 0;
	uint64_t dst_u64 = 0;
	int ret = 0;

	struct vm_snapshot_meta save = {
		.buffer = {
			.buf_start = buf,
			.buf_size = sizeof (buf),
			.buf = buf,
			.buf_rem = sizeof (buf),
		},
		.op = VM_SNAPSHOT_SAVE,
	};
	SNAPSHOT_VAR_OR_LEAVE(src_u32, &save, ret, save_done);
	SNAPSHOT_VAR_OR_LEAVE(src_u64, &save, ret, save_done);
save_done:
	REQUIRE(ret == 0, "save round-trip should succeed");
	size_t saved_bytes = sizeof (buf) - save.buffer.buf_rem;
	REQUIRE(saved_bytes == sizeof (src_u32) + sizeof (src_u64),
	    "saved bytes should equal sum of var sizes");

	struct vm_snapshot_meta restore = {
		.buffer = {
			.buf_start = buf,
			.buf_size = saved_bytes,
			.buf = buf,
			.buf_rem = saved_bytes,
		},
		.op = VM_SNAPSHOT_RESTORE,
	};
	ret = 0;
	SNAPSHOT_VAR_OR_LEAVE(dst_u32, &restore, ret, restore_done);
	SNAPSHOT_VAR_OR_LEAVE(dst_u64, &restore, ret, restore_done);
restore_done:
	REQUIRE(ret == 0, "restore round-trip should succeed");
	REQUIRE(dst_u32 == src_u32, "u32 should round-trip");
	REQUIRE(dst_u64 == src_u64, "u64 should round-trip");
}

/*
 * vm_snapshot_buf must refuse to write past the end of a too-small
 * buffer rather than truncating silently.
 */
static void
test_save_overflow(void)
{
	uint8_t buf[4];
	uint64_t v = 0xffffffffffffffffULL;
	int ret = 0;

	struct vm_snapshot_meta save = {
		.buffer = {
			.buf_start = buf,
			.buf_size = sizeof (buf),
			.buf = buf,
			.buf_rem = sizeof (buf),
		},
		.op = VM_SNAPSHOT_SAVE,
	};
	SNAPSHOT_VAR_OR_LEAVE(v, &save, ret, done);
done:
	REQUIRE(ret != 0, "save into too-small buffer must fail");
}

/*
 * vm_snapshot_buf_cmp must report mismatches between the source's
 * encoded value and the destination's local value.  This is the
 * primitive that backs SNAPSHOT_VAR_CMP_OR_LEAVE — the topology and
 * version checks rely on it returning non-zero when source != dest.
 */
static void
test_cmp_mismatch(void)
{
	uint8_t buf[BUFSZ];
	uint32_t src = 0x11111111;
	uint32_t dst_match = src;
	uint32_t dst_mismatch = 0x22222222;
	int ret;

	struct vm_snapshot_meta save = {
		.buffer = {
			.buf_start = buf,
			.buf_size = sizeof (buf),
			.buf = buf,
			.buf_rem = sizeof (buf),
		},
		.op = VM_SNAPSHOT_SAVE,
	};
	ret = 0;
	SNAPSHOT_VAR_OR_LEAVE(src, &save, ret, save_done);
save_done:
	REQUIRE(ret == 0, "save for CMP setup should succeed");
	size_t saved = sizeof (buf) - save.buffer.buf_rem;

	struct vm_snapshot_meta cmp_match = {
		.buffer = {
			.buf_start = buf,
			.buf_size = saved,
			.buf = buf,
			.buf_rem = saved,
		},
		.op = VM_SNAPSHOT_RESTORE,
	};
	ret = 0;
	SNAPSHOT_VAR_CMP_OR_LEAVE(dst_match, &cmp_match, ret, cmp_match_done);
cmp_match_done:
	REQUIRE(ret == 0, "CMP with matching value should succeed");

	struct vm_snapshot_meta cmp_mismatch = {
		.buffer = {
			.buf_start = buf,
			.buf_size = saved,
			.buf = buf,
			.buf_rem = saved,
		},
		.op = VM_SNAPSHOT_RESTORE,
	};
	ret = 0;
	SNAPSHOT_VAR_CMP_OR_LEAVE(dst_mismatch, &cmp_mismatch, ret,
	    cmp_mismatch_done);
cmp_mismatch_done:
	REQUIRE(ret != 0, "CMP with mismatched value must fail");
}

int
main(int argc, char *argv[])
{
	(void) argc; (void) argv;

	test_var_roundtrip();
	test_save_overflow();
	test_cmp_mismatch();

	if (failures != 0) {
		(void) fprintf(stderr,
		    "snapshot_buf_roundtrip: %d failures\n", failures);
		return (1);
	}
	(void) printf("snapshot_buf_roundtrip: ok\n");
	return (0);
}
