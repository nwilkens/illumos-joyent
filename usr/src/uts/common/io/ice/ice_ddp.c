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
 * Dynamic Device Personalization (DDP) package load.
 *
 * The E810 needs a firmware package to enable its full pipeline (flexible
 * descriptors, advanced switch and flow rules).  The package is an opaque blob
 * the common code parses and downloads to the device; the driver only reads the
 * file and hands it over.  If the file is missing or rejected the device runs
 * in "safe mode": the common code reduces the advertised capabilities and the
 * driver still attaches and passes basic traffic, so a missing package is not
 * fatal.  This loads before the VSI is built because safe mode clamps the queue
 * counts the VSI configuration consumes.
 */

#include <sys/firmload.h>

#include "ice.h"
#include "ice_common.h"
#include "ice_ddp_common.h"

#define	ICE_DDP_PKG_FILE	"ice.pkg"

/*
 * Upper bound on the package file we will read into the kernel.  The shipped
 * package is well under this; the cap keeps a corrupt or hostile file from
 * driving an unbounded allocation.
 */
#define	ICE_DDP_PKG_MAX		(16 * 1024 * 1024)

static void
ice_ddp_safe_mode(ice_t *ice)
{
	ice_set_safe_mode_caps(&ice->ice_hw);
	ice->ice_safe_mode = B_TRUE;
}

/*
 * Bound the segment at idx and return its header.  seg_size covers the header,
 * so a segment that clears this can be walked out to its declared extent.
 */
static const struct ice_generic_seg_hdr *
ice_ddp_seg_hdr(const uint8_t *pkg, uint64_t len, uint32_t seg_count,
    uint32_t idx)
{
	const struct ice_pkg_hdr *hdr = (const struct ice_pkg_hdr *)pkg;
	const struct ice_generic_seg_hdr *seg;
	uint64_t off;

	if (idx >= seg_count)
		return (NULL);

	off = LE32_TO_CPU(hdr->seg_offset[idx]);
	if (off + sizeof (*seg) > len)
		return (NULL);

	seg = (const struct ice_generic_seg_hdr *)(pkg + off);
	if (off + LE32_TO_CPU(seg->seg_size) > len)
		return (NULL);

	return (seg);
}

/*
 * Repeat the address arithmetic ice_find_buf_table() performs and require the
 * result to stay inside the segment.  The device id table and the NVM version
 * table are each preceded by their own count and the buffer array follows;
 * every count is read only once the terms ahead of it are known to fit.
 */
static boolean_t
ice_ddp_cfg_seg_bufs(const struct ice_generic_seg_hdr *hdr, uint64_t *cntp,
    const struct ice_buf_table **bufsp)
{
	const struct ice_seg *seg = (const struct ice_seg *)hdr;
	const struct ice_nvm_table *nvms;
	const struct ice_buf_table *bufs;
	uint64_t size, need, cnt;

	size = LE32_TO_CPU(hdr->seg_size);
	if (size < offsetof(struct ice_seg, device_table))
		return (B_FALSE);

	need = (uint64_t)offsetof(struct ice_seg, device_table) +
	    (uint64_t)LE32_TO_CPU(seg->device_table_count) *
	    sizeof (seg->device_table[0]);
	if (need + offsetof(struct ice_nvm_table, vers) > size)
		return (B_FALSE);

	nvms = (const struct ice_nvm_table *)((const uint8_t *)hdr + need);
	need += (uint64_t)offsetof(struct ice_nvm_table, vers) +
	    (uint64_t)LE32_TO_CPU(nvms->table_count) * sizeof (nvms->vers[0]);
	if (need + offsetof(struct ice_buf_table, buf_array) > size)
		return (B_FALSE);

	bufs = (const struct ice_buf_table *)((const uint8_t *)hdr + need);
	cnt = LE32_TO_CPU(bufs->buf_count);
	need += (uint64_t)offsetof(struct ice_buf_table, buf_array) +
	    cnt * sizeof (bufs->buf_array[0]);
	if (need > size)
		return (B_FALSE);

	*cntp = cnt;
	if (bufsp != NULL)
		*bufsp = bufs;
	return (B_TRUE);
}

/*
 * The bytes a typed section must hold before the core reads its fixed header
 * and counted array.  ice_pkg_enum_section() checks only that a section lies
 * inside its 4 KiB buffer; the consumers then read a fixed struct, or index an
 * array by the section's own count, without comparing either to the section
 * size.  The metadata read happens before the signature check, so a corrupt
 * file must fail here instead of reading past the package allocation.
 * Sections the driver never enumerates need no minimum.
 */
static uint64_t
ice_ddp_sect_min(const uint8_t *sect, uint32_t size, uint32_t type)
{
	__le16 raw;
	uint16_t count = 0;

	/* A section too short for its count still owes its fixed header. */
	if (size >= sizeof (raw)) {
		bcopy(sect, &raw, sizeof (raw));
		count = LE16_TO_CPU(raw);
	}

	switch (type) {
	case ICE_SID_METADATA:
		return (sizeof (struct ice_meta_sect));
	case ICE_SID_XLT1_SW:
	case ICE_SID_XLT1_ACL:
	case ICE_SID_XLT1_FD:
	case ICE_SID_XLT1_RSS:
	case ICE_SID_XLT1_PE:
		return (offsetof(struct ice_xlt1_section, value) +
		    (uint64_t)count * sizeof (uint8_t));
	case ICE_SID_XLT2_SW:
	case ICE_SID_XLT2_ACL:
	case ICE_SID_XLT2_FD:
	case ICE_SID_XLT2_RSS:
	case ICE_SID_XLT2_PE:
		return (offsetof(struct ice_xlt2_section, value) +
		    (uint64_t)count * sizeof (__le16));
	case ICE_SID_PROFID_TCAM_SW:
	case ICE_SID_PROFID_TCAM_ACL:
	case ICE_SID_PROFID_TCAM_FD:
	case ICE_SID_PROFID_TCAM_RSS:
	case ICE_SID_PROFID_TCAM_PE:
		return (offsetof(struct ice_prof_id_section, entry) +
		    (uint64_t)count * sizeof (struct ice_prof_tcam_entry));
	case ICE_SID_PROFID_REDIR_SW:
	case ICE_SID_PROFID_REDIR_ACL:
	case ICE_SID_PROFID_REDIR_FD:
	case ICE_SID_PROFID_REDIR_RSS:
	case ICE_SID_PROFID_REDIR_PE:
		return (offsetof(struct ice_prof_redir_section, redir_value) +
		    (uint64_t)count * sizeof (uint8_t));
	case ICE_SID_FLD_VEC_SW:
	case ICE_SID_FLD_VEC_ACL:
	case ICE_SID_FLD_VEC_FD:
	case ICE_SID_FLD_VEC_RSS:
	case ICE_SID_FLD_VEC_PE:
		return (offsetof(struct ice_sw_fv_section, fv) +
		    (uint64_t)count * sizeof (struct ice_fv));
	case ICE_SID_RXPARSER_BOOST_TCAM:
		return (offsetof(struct ice_boost_tcam_section, tcam) +
		    (uint64_t)count * sizeof (struct ice_boost_tcam_entry));
	default:
		if (type >= ICE_SID_LBL_FIRST && type <= ICE_SID_LBL_LAST) {
			return (offsetof(struct ice_label_section, label) +
			    (uint64_t)count * sizeof (struct ice_label));
		}
		return (0);
	}
}

/*
 * Repeat ice_pkg_val_buf() and ice_pkg_enum_section()'s bounds on every
 * section table entry, then require each typed section to hold what its
 * consumer reads.  The whole buffer is inside the segment, so the section
 * table and every extent below ICE_PKG_BUF_SIZE are addressable here.
 */
static boolean_t
ice_ddp_buf_ok(const struct ice_buf *buf)
{
	const struct ice_buf_hdr *hdr = (const struct ice_buf_hdr *)buf->buf;
	uint32_t count, data_end, i;

	count = LE16_TO_CPU(hdr->section_count);
	data_end = LE16_TO_CPU(hdr->data_end);
	if (count < ICE_MIN_S_COUNT || count > ICE_MAX_S_COUNT ||
	    data_end < ICE_MIN_S_DATA_END || data_end > ICE_MAX_S_DATA_END)
		return (B_FALSE);
	if (offsetof(struct ice_buf_hdr, section_entry) +
	    (uint64_t)count * sizeof (hdr->section_entry[0]) > ICE_PKG_BUF_SIZE)
		return (B_FALSE);

	for (i = 0; i < count; i++) {
		const struct ice_section_entry *ent = &hdr->section_entry[i];
		uint32_t type = LE32_TO_CPU(ent->type);
		uint32_t off = LE16_TO_CPU(ent->offset);
		uint32_t size = LE16_TO_CPU(ent->size);
		uint64_t need;

		if (off < ICE_MIN_S_OFF || off > ICE_MAX_S_OFF ||
		    size < ICE_MIN_S_SZ || size > ICE_MAX_S_SZ ||
		    off + size > ICE_PKG_BUF_SIZE)
			return (B_FALSE);

		need = ice_ddp_sect_min(buf->buf + off, size, type);
		if (need > size)
			return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
ice_ddp_cfg_seg_ok(const struct ice_generic_seg_hdr *hdr)
{
	const struct ice_buf_table *bufs;
	uint64_t cnt, i;

	if (!ice_ddp_cfg_seg_bufs(hdr, &cnt, &bufs))
		return (B_FALSE);
	for (i = 0; i < cnt; i++) {
		if (!ice_ddp_buf_ok(&bufs->buf_array[i]))
			return (B_FALSE);
	}
	return (B_TRUE);
}

/*
 * A signing segment carries its own buffer table, which
 * ice_download_pkg_sig_seg() hands to the firmware verbatim, and when it signs
 * configuration buffers it names another segment by index plus a range within
 * that segment's buffer array.  Bounding the range here is what gives the
 * core's own start and count test something valid to compare against.
 */
static boolean_t
ice_ddp_sign_seg_ok(const uint8_t *pkg, uint64_t len, uint32_t seg_count,
    const struct ice_generic_seg_hdr *hdr)
{
	const struct ice_sign_seg *sign = (const struct ice_sign_seg *)hdr;
	const struct ice_generic_seg_hdr *cfg;
	uint64_t size, need, start, cnt, bufs;

	size = LE32_TO_CPU(hdr->seg_size);
	if (size < offsetof(struct ice_sign_seg, buf_tbl) +
	    offsetof(struct ice_buf_table, buf_array))
		return (B_FALSE);

	need = (uint64_t)offsetof(struct ice_sign_seg, buf_tbl) +
	    (uint64_t)offsetof(struct ice_buf_table, buf_array) +
	    (uint64_t)LE32_TO_CPU(sign->buf_tbl.buf_count) *
	    sizeof (sign->buf_tbl.buf_array[0]);
	if (need > size)
		return (B_FALSE);

	/* Zero count is a reference signature: no config segment is used. */
	cnt = LE32_TO_CPU(sign->signed_buf_count);
	if (cnt == 0)
		return (B_TRUE);

	cfg = ice_ddp_seg_hdr(pkg, len, seg_count,
	    LE32_TO_CPU(sign->signed_seg_idx));
	if (cfg == NULL)
		return (B_FALSE);

	switch (LE32_TO_CPU(cfg->seg_type)) {
	case SEGMENT_TYPE_ICE_E810:
	case SEGMENT_TYPE_ICE_E830:
		break;
	default:
		return (B_FALSE);
	}

	if (!ice_ddp_cfg_seg_bufs(cfg, &bufs, NULL))
		return (B_FALSE);

	start = LE32_TO_CPU(sign->signed_buf_start);
	return (start + cnt <= bufs);
}

/*
 * The vendored parser bounds the package header, the segment offset array and
 * each segment's declared extent, but nothing inside a segment: it forms the
 * buffer table pointer from ice_seg->device_table_count and
 * ice_nvm_table->table_count without first proving either is in bounds, and
 * its typed section consumers never compare a section's size to what they
 * read.  Those dereferences start in ice_init_pkg_info(), ahead of every
 * version, compatibility and signature gate, so a corrupt file would panic
 * instead of falling back to safe mode.  All arithmetic here is 64 bit, which
 * also removes the u32 overflow in the core's own segment extent test.
 */
static boolean_t
ice_ddp_pkg_valid(const uint8_t *pkg, uint64_t len)
{
	const struct ice_pkg_hdr *hdr = (const struct ice_pkg_hdr *)pkg;
	uint32_t seg_count, i;

	if (len < offsetof(struct ice_pkg_hdr, seg_offset))
		return (B_FALSE);

	seg_count = LE32_TO_CPU(hdr->seg_count);
	if ((uint64_t)offsetof(struct ice_pkg_hdr, seg_offset) +
	    (uint64_t)seg_count * sizeof (hdr->seg_offset[0]) > len)
		return (B_FALSE);

	for (i = 0; i < seg_count; i++) {
		const struct ice_generic_seg_hdr *seg;

		seg = ice_ddp_seg_hdr(pkg, len, seg_count, i);
		if (seg == NULL)
			return (B_FALSE);

		switch (LE32_TO_CPU(seg->seg_type)) {
		case SEGMENT_TYPE_ICE_E810:
		case SEGMENT_TYPE_ICE_E830:
			if (!ice_ddp_cfg_seg_ok(seg))
				return (B_FALSE);
			break;
		case SEGMENT_TYPE_SIGNING:
			if (!ice_ddp_sign_seg_ok(pkg, len, seg_count, seg))
				return (B_FALSE);
			break;
		default:
			/*
			 * SEGMENT_TYPE_ICE_RUN_TIME_CFG needs no coverage:
			 * ice_cfg_tx_topo() is never called from this driver.
			 */
			break;
		}
	}

	return (B_TRUE);
}

boolean_t
ice_ddp_load(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	firmware_handle_t fwh;
	off_t size;
	uint8_t *buf;
	enum ice_ddp_state state;

	if (firmware_open(ICE_MODULE_NAME, ICE_DDP_PKG_FILE, &fwh) != 0) {
		ice_error(ice, "no ice.pkg found; safe mode limits the "
		    "device to one Tx and one Rx queue with no checksum "
		    "offload, LSO or RSS");
		ice_ddp_safe_mode(ice);
		return (B_TRUE);
	}

	size = firmware_get_size(fwh);
	if (size <= 0 || size > ICE_DDP_PKG_MAX) {
		ice_error(ice, "ignoring ice.pkg with bad size %lld",
		    (long long)size);
		(void) firmware_close(fwh);
		ice_ddp_safe_mode(ice);
		return (B_TRUE);
	}

	/*
	 * Zeroed because firmware_read() cannot report a short read: it
	 * discards kobj_read_file()'s byte count.  A file truncated between
	 * the size check and the read would otherwise leave stale kernel heap
	 * as package content.
	 */
	buf = kmem_zalloc((size_t)size, KM_SLEEP);
	if (firmware_read(fwh, 0, buf, (size_t)size) != 0) {
		ice_error(ice, "failed to read ice.pkg");
		kmem_free(buf, (size_t)size);
		(void) firmware_close(fwh);
		ice_ddp_safe_mode(ice);
		return (B_TRUE);
	}
	(void) firmware_close(fwh);

	if (!ice_ddp_pkg_valid(buf, (uint64_t)size)) {
		ice_error(ice, "ice.pkg segment bounds are inconsistent "
		    "with the file; using safe mode");
		kmem_free(buf, (size_t)size);
		ice_ddp_safe_mode(ice);
		return (B_TRUE);
	}

	/* ice_copy_and_init_pkg() copies into its own DMA; free ours after. */
	state = ice_copy_and_init_pkg(hw, buf, (u32)size);
	kmem_free(buf, (size_t)size);

	ice->ice_ddp_state = state;
	if (!ice_is_init_pkg_successful(state)) {
		ice_error(ice, "ice.pkg init failed (%d); using safe mode",
		    state);
		ice_ddp_safe_mode(ice);
	} else {
		char name[ICE_PKG_NAME_SIZE + 1];

		/*
		 * The package name is firmware input and need not be
		 * terminated.
		 */
		bcopy(hw->active_pkg_name, name, ICE_PKG_NAME_SIZE);
		name[ICE_PKG_NAME_SIZE] = '\0';
		dev_err(ice->ice_dip, CE_NOTE,
		    "!DDP package active: %s version %u.%u.%u.%u", name,
		    hw->active_pkg_ver.major, hw->active_pkg_ver.minor,
		    hw->active_pkg_ver.update, hw->active_pkg_ver.draft);
	}

	return (B_TRUE);
}
