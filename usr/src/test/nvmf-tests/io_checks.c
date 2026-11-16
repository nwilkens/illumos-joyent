/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * An I/O command names its LU by NSID and sizes its transfer with an SGL,
 * both of which are the host's.  The status that comes back is the LU's SCSI
 * status, with sense data that STMF builds 18 bytes long.
 */
#include "io.h"

static void
command(uint8_t opc, uint32_t nsid, size_t len, uint8_t want, int line)
{
	uint8_t sc = nvmft_check_io_command(opc, nsid, len);

	if (sc != want) {
		(void) fprintf(stderr, "line %d: sc %#x, expected %#x\n", line,
		    sc, want);
		abort();
	}
}

#define	COMMAND(opc, nsid, len, want)	command(opc, nsid, len, want, __LINE__)

static void
status(uint8_t st, const uint8_t *sense, size_t len, uint8_t want_sct,
    uint8_t want_sc, int line)
{
	uint8_t sct = 0xff, sc = 0xff;

	nvmft_scsi_to_nvme(st, sense, len, &sct, &sc);
	if (sct != want_sct || sc != want_sc) {
		(void) fprintf(stderr, "line %d: %u/%#x, expected %u/%#x\n",
		    line, sct, sc, want_sct, want_sc);
		abort();
	}
}

#define	STATUS(st, sense, len, sct, sc) \
	status(st, sense, len, sct, sc, __LINE__)

/* Fixed format sense data as stmf_scsilib_send_status() builds it. */
static void
check(uint8_t key, uint8_t asc, uint8_t want_sct, uint8_t want_sc, int line)
{
	uint8_t fixed[18] = { 0x70, 0, key, 0, 0, 0, 0, 10 };
	uint8_t desc[8] = { 0x72, key, asc };

	fixed[12] = asc;
	status(STATUS_CHECK, fixed, sizeof (fixed), want_sct, want_sc, line);
	status(STATUS_CHECK, desc, sizeof (desc), want_sct, want_sc, line);
}

#define	CHECK(key, asc, sct, sc)	check(key, asc, sct, sc, __LINE__)

int
main(void)
{
	uint8_t sense[18] = { 0x70, 0, KEY_ILLEGAL_REQUEST };
	const uint8_t gen = NVME_CQE_SCT_GENERIC;
	const uint8_t media = NVME_CQE_SCT_INTEGRITY;

	/* NSID n is LUN n - 1, and a LUN has 14 bits. */
	COMMAND(NVME_OPC_NVM_READ, 1, 512, NVME_CQE_SC_GEN_SUCCESS);
	COMMAND(NVME_OPC_NVM_WRITE, 0x4000, 4096, NVME_CQE_SC_GEN_SUCCESS);
	COMMAND(NVME_OPC_NVM_READ, 0, 512, NVME_CQE_SC_GEN_INV_NS);
	COMMAND(NVME_OPC_NVM_READ, 0x4001, 512, NVME_CQE_SC_GEN_INV_NS);
	COMMAND(NVME_OPC_NVM_WRITE, 0x10001, 512, NVME_CQE_SC_GEN_INV_NS);
	COMMAND(NVME_OPC_NVM_WRITE, UINT32_MAX, 512, NVME_CQE_SC_GEN_INV_NS);
	COMMAND(NVME_OPC_NVM_FLUSH, 0x4001, 0, NVME_CQE_SC_GEN_INV_NS);

	/* A READ or WRITE moves at least one block. */
	COMMAND(NVME_OPC_NVM_READ, 1, 0, NVME_CQE_SC_GEN_INV_DSGL_LEN);
	COMMAND(NVME_OPC_NVM_WRITE, 1, 0, NVME_CQE_SC_GEN_INV_DSGL_LEN);
	COMMAND(NVME_OPC_NVM_WRITE, 1, (size_t)UINT32_MAX + 1,
	    NVME_CQE_SC_GEN_INV_DSGL_LEN);
	COMMAND(NVME_OPC_NVM_FLUSH, 1, 0, NVME_CQE_SC_GEN_SUCCESS);

	/* Commands with no translation never get an STMF task. */
	COMMAND(NVME_OPC_NVM_COMPARE, 1, 512, NVME_CQE_SC_GEN_INV_OPC);
	COMMAND(NVME_OPC_NVM_WRITE_ZERO, 1, 0, NVME_CQE_SC_GEN_INV_OPC);
	COMMAND(NVME_OPC_NVM_DSET_MGMT, 1, 4096, NVME_CQE_SC_GEN_INV_OPC);
	COMMAND(0xff, 1, 512, NVME_CQE_SC_GEN_INV_OPC);

	STATUS(STATUS_GOOD, NULL, 0, gen, NVME_CQE_SC_GEN_SUCCESS);
	STATUS(STATUS_RESERVATION_CONFLICT, NULL, 0, gen,
	    NVME_CQE_SC_GEN_NVM_RSV_CNFLCT);
	STATUS(STATUS_BUSY, NULL, 0, gen, NVME_CQE_SC_GEN_CMD_INTR);
	STATUS(STATUS_QFULL, NULL, 0, gen, NVME_CQE_SC_GEN_CMD_INTR);
	STATUS(STATUS_TASK_ABORT, NULL, 0, gen, NVME_CQE_SC_GEN_ABORT_REQUEST);
	STATUS(STATUS_ACA_ACTIVE, NULL, 0, gen, NVME_CQE_SC_GEN_INTERNAL_ERR);

	CHECK(KEY_NO_SENSE, 0, gen, NVME_CQE_SC_GEN_SUCCESS);
	CHECK(KEY_NOT_READY, 0x04, gen, NVME_CQE_SC_GEN_NVM_NS_NOTRDY);
	CHECK(KEY_MEDIUM_ERROR, 0x11, media, NVME_CQE_SC_INT_NVM_READ);
	CHECK(KEY_MEDIUM_ERROR, 0x0c, media, NVME_CQE_SC_INT_NVM_WRITE);
	CHECK(KEY_HARDWARE_ERROR, 0x44, gen, NVME_CQE_SC_GEN_INTERNAL_ERR);
	CHECK(KEY_ILLEGAL_REQUEST, 0x21, gen, NVME_CQE_SC_GEN_NVM_LBA_RANGE);
	CHECK(KEY_ILLEGAL_REQUEST, 0x20, gen, NVME_CQE_SC_GEN_INV_OPC);
	CHECK(KEY_ILLEGAL_REQUEST, 0x25, gen, NVME_CQE_SC_GEN_INV_NS);
	CHECK(KEY_ILLEGAL_REQUEST, 0x24, gen, NVME_CQE_SC_GEN_INV_FLD);
	CHECK(KEY_UNIT_ATTENTION, 0x29, gen, NVME_CQE_SC_GEN_CMD_INTR);
	CHECK(KEY_WRITE_PROTECT, 0x27, gen, NVME_CQE_SC_GEN_NS_RDONLY);
	CHECK(KEY_ABORTED_COMMAND, 0, gen, NVME_CQE_SC_GEN_ABORT_REQUEST);
	CHECK(KEY_MISCOMPARE, 0x1d, media, NVME_CQE_SC_INT_NVM_COMPARE);

	/* Short, absent or unknown sense data is not read past its end. */
	sense[12] = 0x21;
	STATUS(STATUS_CHECK, sense, 14, gen, NVME_CQE_SC_GEN_NVM_LBA_RANGE);
	STATUS(STATUS_CHECK, sense, 13, gen, NVME_CQE_SC_GEN_INTERNAL_ERR);
	STATUS(STATUS_CHECK, sense, 0, gen, NVME_CQE_SC_GEN_INTERNAL_ERR);
	STATUS(STATUS_CHECK, NULL, 18, gen, NVME_CQE_SC_GEN_INTERNAL_ERR);
	sense[0] = 0x7f;
	STATUS(STATUS_CHECK, sense, 18, gen, NVME_CQE_SC_GEN_INTERNAL_ERR);
	sense[0] = 0x72;
	STATUS(STATUS_CHECK, sense, 3, gen, NVME_CQE_SC_GEN_INTERNAL_ERR);

	(void) printf("I/O command and status checks passed\n");
	return (0);
}
