/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 * A copy of the CDDL is available at http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Execute the production ice.pkg validator against constructed packages.
 * The structure stand-ins below carry the vendor field order and widths, so
 * the extracted validator computes the same offsets the driver does.  The
 * package is root-supplied, but a corrupt file must fall back to safe mode
 * instead of reading past its allocation before the signature check.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int boolean_t;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint8_t u8;
#define	B_TRUE	1
#define	B_FALSE	0
#define	LE16_TO_CPU(x)	(x)
#define	LE32_TO_CPU(x)	(x)
#undef bcopy
#define	bcopy(s, d, n)	((void) memcpy((d), (s), (n)))
#define	STRUCT_HACK_VAR_LEN	1

#pragma pack(1)
struct ice_pkg_ver { u8 major, minor, update, draft; };
struct ice_pkg_hdr {
	struct ice_pkg_ver pkg_format_ver;
	__le32 seg_count;
	__le32 seg_offset[STRUCT_HACK_VAR_LEN];
};
#define	SEGMENT_TYPE_ICE_E810	0x00000010
#define	SEGMENT_TYPE_SIGNING	0x00001001
#define	SEGMENT_TYPE_ICE_E830	0x00000017
#define	ICE_PKG_NAME_SIZE	32
struct ice_generic_seg_hdr {
	__le32 seg_type;
	struct ice_pkg_ver seg_format_ver;
	__le32 seg_size;
	char seg_id[ICE_PKG_NAME_SIZE];
};
struct ice_device_id_entry { __le32 device, sub_device; };
struct ice_seg {
	struct ice_generic_seg_hdr hdr;
	__le32 device_table_count;
	struct ice_device_id_entry device_table[STRUCT_HACK_VAR_LEN];
};
struct ice_nvm_table {
	__le32 table_count;
	__le32 vers[STRUCT_HACK_VAR_LEN];
};
#define	ICE_PKG_BUF_SIZE	4096
struct ice_buf { u8 buf[ICE_PKG_BUF_SIZE]; };
struct ice_buf_table {
	__le32 buf_count;
	struct ice_buf buf_array[STRUCT_HACK_VAR_LEN];
};
struct ice_sign_seg {
	struct ice_generic_seg_hdr hdr;
	__le32 seg_id, sign_type, signed_seg_idx, signed_buf_start;
	__le32 signed_buf_count, flags;
	u8 reserved[40];
	struct ice_buf_table buf_tbl;
};
struct ice_section_entry { __le32 type; __le16 offset; __le16 size; };
struct ice_buf_hdr {
	__le16 section_count;
	__le16 data_end;
	struct ice_section_entry section_entry[STRUCT_HACK_VAR_LEN];
};
#define	ICE_MIN_S_OFF		12
#define	ICE_MAX_S_OFF		4095
#define	ICE_MIN_S_SZ		1
#define	ICE_MAX_S_SZ		4084
#define	ICE_MIN_S_COUNT		1
#define	ICE_MAX_S_COUNT		511
#define	ICE_MIN_S_DATA_END	12
#define	ICE_MAX_S_DATA_END	4096

#define	ICE_SID_METADATA		1
#define	ICE_SID_XLT1_SW			12
#define	ICE_SID_XLT2_SW			13
#define	ICE_SID_PROFID_TCAM_SW		14
#define	ICE_SID_PROFID_REDIR_SW		15
#define	ICE_SID_FLD_VEC_SW		16
#define	ICE_SID_XLT1_ACL		22
#define	ICE_SID_XLT2_ACL		23
#define	ICE_SID_PROFID_TCAM_ACL		24
#define	ICE_SID_PROFID_REDIR_ACL	25
#define	ICE_SID_FLD_VEC_ACL		26
#define	ICE_SID_XLT1_FD			32
#define	ICE_SID_XLT2_FD			33
#define	ICE_SID_PROFID_TCAM_FD		34
#define	ICE_SID_PROFID_REDIR_FD		35
#define	ICE_SID_FLD_VEC_FD		36
#define	ICE_SID_XLT1_RSS		42
#define	ICE_SID_XLT2_RSS		43
#define	ICE_SID_PROFID_TCAM_RSS		44
#define	ICE_SID_PROFID_REDIR_RSS	45
#define	ICE_SID_FLD_VEC_RSS		46
#define	ICE_SID_RXPARSER_BOOST_TCAM	56
#define	ICE_SID_XLT1_PE			82
#define	ICE_SID_XLT2_PE			83
#define	ICE_SID_PROFID_TCAM_PE		84
#define	ICE_SID_PROFID_REDIR_PE		85
#define	ICE_SID_FLD_VEC_PE		86
#define	ICE_SID_LBL_RXPARSER_TMEM	0x80000018
#define	ICE_SID_LBL_PTYPE_META		0x8000002F

struct ice_meta_sect {
	struct ice_pkg_ver ver;
	char name[28];
	__le32 track_id;
};
struct ice_label { __le16 value; char name[64]; };
struct ice_label_section {
	__le16 count;
	struct ice_label label[STRUCT_HACK_VAR_LEN];
};
struct ice_fv_word { u8 prot_id; __le16 off; u8 resvrd; };
struct ice_fv { struct ice_fv_word ew[48]; };
struct ice_sw_fv_section {
	__le16 count, base_offset;
	struct ice_fv fv[STRUCT_HACK_VAR_LEN];
};
struct ice_boost_tcam_entry { __le16 addr, reserved; u8 key[40]; u8 grp, bits[43]; };
struct ice_boost_tcam_section {
	__le16 count, reserved;
	struct ice_boost_tcam_entry tcam[STRUCT_HACK_VAR_LEN];
};
struct ice_xlt1_section { __le16 count, offset; u8 value[STRUCT_HACK_VAR_LEN]; };
struct ice_xlt2_section { __le16 count, offset; __le16 value[STRUCT_HACK_VAR_LEN]; };
struct ice_prof_tcam_entry { __le16 addr; u8 key[10]; u8 prof_id; };
struct ice_prof_id_section {
	__le16 count;
	struct ice_prof_tcam_entry entry[STRUCT_HACK_VAR_LEN];
};
struct ice_prof_redir_section {
	__le16 count, offset;
	u8 redir_value[STRUCT_HACK_VAR_LEN];
};
#pragma pack()

#define	ICE_MAX_FV_WORDS	48
#define	CTASSERT(x)	extern char ctassert_dummy[(x) ? 1 : -1]
#include "ddp_sections_body.h"

/*
 * One E810 segment with no device or NVM entries and one buffer.  The buffer's
 * section table is filled by each case.
 */
#define	SEG_OFF		(offsetof(struct ice_pkg_hdr, seg_offset) + 4)
#define	BUFS_OFF	(SEG_OFF + offsetof(struct ice_seg, device_table) + \
			sizeof (struct ice_nvm_table) - 4)
#define	PKG_LEN(nbuf)	(BUFS_OFF + 4 + (nbuf) * ICE_PKG_BUF_SIZE)

static uint8_t *
package(unsigned nbuf, struct ice_buf_hdr **bufp)
{
	uint8_t *pkg = calloc(1, PKG_LEN(nbuf));
	struct ice_pkg_hdr *hdr = (struct ice_pkg_hdr *)pkg;
	struct ice_generic_seg_hdr *seg = (void *)(pkg + SEG_OFF);
	struct ice_buf_table *bufs = (void *)(pkg + BUFS_OFF);

	assert(pkg != NULL);
	hdr->seg_count = 1;
	hdr->seg_offset[0] = SEG_OFF;
	seg->seg_type = SEGMENT_TYPE_ICE_E810;
	seg->seg_size = PKG_LEN(nbuf) - SEG_OFF;
	bufs->buf_count = nbuf;
	*bufp = (struct ice_buf_hdr *)bufs->buf_array[0].buf;
	return (pkg);
}

static void
section(struct ice_buf_hdr *buf, unsigned idx, uint32_t type, uint16_t off,
    uint16_t size, uint16_t count)
{
	buf->section_entry[idx].type = type;
	buf->section_entry[idx].offset = off;
	buf->section_entry[idx].size = size;
	if (off + 2 <= ICE_PKG_BUF_SIZE)
		memcpy((uint8_t *)buf + off, &count, sizeof (count));
}

static boolean_t
one(uint32_t type, uint16_t off, uint16_t size, uint16_t count)
{
	struct ice_buf_hdr *buf;
	uint8_t *pkg = package(1, &buf);
	boolean_t ok;

	buf->section_count = 1;
	buf->data_end = ICE_PKG_BUF_SIZE;
	section(buf, 0, type, off, size, count);
	ok = ice_ddp_pkg_valid(pkg, PKG_LEN(1));
	free(pkg);
	return (ok);
}

/* The shipped package must pass every check. */
static void
real_package(const char *path)
{
	FILE *fp = fopen(path, "rb");
	uint8_t *pkg;
	long len;

	assert(fp != NULL);
	assert(fseek(fp, 0, SEEK_END) == 0);
	len = ftell(fp);
	assert(len > 0);
	rewind(fp);
	pkg = malloc((size_t)len);
	assert(pkg != NULL);
	assert(fread(pkg, 1, (size_t)len, fp) == (size_t)len);
	(void) fclose(fp);
	assert(ice_ddp_pkg_valid(pkg, (uint64_t)len));
	/* Truncating the file must fail the outer bounds, not read past. */
	assert(!ice_ddp_pkg_valid(pkg, (uint64_t)len - 1));
	free(pkg);
	(void) printf("real package %s: %ld bytes valid\n", path, len);
}

int
main(int argc, char **argv)
{
	struct ice_buf_hdr *buf;
	uint8_t *pkg;

	if (argc > 1)
		real_package(argv[1]);

	/* Metadata: the struct fits, or the section is one byte at the end. */
	assert(one(ICE_SID_METADATA, 20, 36, 1));
	assert(one(ICE_SID_METADATA, 4096 - 36, 36, 1));
	assert(!one(ICE_SID_METADATA, 20, 35, 1));
	assert(!one(ICE_SID_METADATA, 4095, 1, 1));

	/* Generic extents the core also refuses. */
	assert(!one(ICE_SID_METADATA, 11, 36, 1));
	assert(!one(ICE_SID_METADATA, 4090, 8, 1));
	assert(!one(ICE_SID_METADATA, 20, 0, 1));

	/* Counted arrays must fit the section, per entry width. */
	assert(one(ICE_SID_FLD_VEC_SW, 20, 4 + 2 * 192, 2));
	assert(!one(ICE_SID_FLD_VEC_SW, 20, 4 + 2 * 192, 3));
	assert(!one(ICE_SID_FLD_VEC_SW, 20, 200, 100));
	/* Field vectors are narrower outside the switch block. */
	assert(one(ICE_SID_FLD_VEC_ACL, 20, 4 + 31 * 128, 31));
	assert(!one(ICE_SID_FLD_VEC_ACL, 20, 4 + 31 * 128 - 1, 31));
	assert(one(ICE_SID_FLD_VEC_RSS, 20, 4 + 40 * 96, 40));
	assert(!one(ICE_SID_FLD_VEC_RSS, 20, 4 + 40 * 96 - 1, 40));
	assert(one(ICE_SID_FLD_VEC_PE, 20, 4 + 96, 1));
	assert(one(ICE_SID_LBL_RXPARSER_TMEM, 20, 2 + 3 * 66, 3));
	assert(!one(ICE_SID_LBL_RXPARSER_TMEM, 20, 2 + 3 * 66 - 1, 3));
	assert(one(ICE_SID_RXPARSER_BOOST_TCAM, 20, 4 + 88, 1));
	assert(!one(ICE_SID_RXPARSER_BOOST_TCAM, 20, 4 + 88, 2));
	assert(one(ICE_SID_XLT1_PE, 20, 4 + 100, 100));
	assert(!one(ICE_SID_XLT1_PE, 20, 4 + 99, 100));
	assert(one(ICE_SID_XLT2_RSS, 20, 4 + 200, 100));
	assert(!one(ICE_SID_XLT2_RSS, 20, 4 + 199, 100));
	assert(one(ICE_SID_PROFID_TCAM_FD, 20, 2 + 13 * 4, 4));
	assert(!one(ICE_SID_PROFID_TCAM_FD, 20, 2 + 13 * 4 - 1, 4));
	assert(one(ICE_SID_PROFID_REDIR_ACL, 20, 4 + 7, 7));
	assert(!one(ICE_SID_PROFID_REDIR_ACL, 20, 4 + 6, 7));

	/* A type the driver never enumerates carries no minimum. */
	assert(one(0x7000, 4095, 1, 0));
	/* Other label types have their own layouts; the shipped PTYPE_META. */
	assert(one(ICE_SID_LBL_PTYPE_META, 12, 3164, 93));
	/* A typed section too short for its count still owes its header. */
	assert(!one(ICE_SID_FLD_VEC_SW, 4095, 1, 0));
	assert(one(ICE_SID_FLD_VEC_SW, 4092, 4, 0));
	assert(!one(ICE_SID_LBL_RXPARSER_TMEM, 4095, 1, 0));

	/* A segment with no buffers is unusable: the core reads the first. */
	pkg = package(1, &buf);
	((struct ice_buf_table *)(pkg + BUFS_OFF))->buf_count = 0;
	assert(!ice_ddp_pkg_valid(pkg, PKG_LEN(1)));
	free(pkg);

	/* Buffer header bounds. */
	pkg = package(1, &buf);
	buf->section_count = 0;
	buf->data_end = ICE_PKG_BUF_SIZE;
	assert(!ice_ddp_pkg_valid(pkg, PKG_LEN(1)));
	buf->section_count = 512;
	assert(!ice_ddp_pkg_valid(pkg, PKG_LEN(1)));
	buf->section_count = 1;
	buf->data_end = ICE_PKG_BUF_SIZE + 1;
	section(buf, 0, ICE_SID_METADATA, 20, 36, 1);
	assert(!ice_ddp_pkg_valid(pkg, PKG_LEN(1)));
	buf->data_end = ICE_PKG_BUF_SIZE;
	assert(ice_ddp_pkg_valid(pkg, PKG_LEN(1)));
	free(pkg);

	/* Every buffer is checked, not just the first. */
	pkg = package(2, &buf);
	buf->section_count = 1;
	buf->data_end = ICE_PKG_BUF_SIZE;
	section(buf, 0, ICE_SID_METADATA, 20, 36, 1);
	buf = (struct ice_buf_hdr *)((uint8_t *)buf + ICE_PKG_BUF_SIZE);
	buf->section_count = 1;
	buf->data_end = ICE_PKG_BUF_SIZE;
	section(buf, 0, ICE_SID_METADATA, 4095, 1, 1);
	assert(!ice_ddp_pkg_valid(pkg, PKG_LEN(2)));
	section(buf, 0, ICE_SID_METADATA, 4060, 36, 1);
	assert(ice_ddp_pkg_valid(pkg, PKG_LEN(2)));
	free(pkg);

	(void) printf("PASS: ice ddp section bounds\n");
	return (0);
}
