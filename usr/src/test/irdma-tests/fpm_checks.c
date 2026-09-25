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
 * Run the FPM query and commit checks from irdma_osdep.c against hostile
 * firmware values.  Every value a rejected case changes would otherwise
 * reach a divide, an unbounded loop, an allocation size or an SD index in
 * the core code.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef unsigned long long u64;
typedef unsigned int uint_t;

#define	ISP2(x)			(((x) & ((x) - 1)) == 0)
#define	IRDMA_HMC_MAX_SD_COUNT	8192
#define	IRDMA_CEQ_MAX_COUNT	1024
#define	IRDMA_HMC_DIRECT_BP_SIZE	0x200000ULL

enum { IRDMA_HMC_IW_QP, IRDMA_HMC_IW_CQ, IRDMA_HMC_IW_SRQ, IRDMA_HMC_IW_HTE,
	IRDMA_HMC_IW_ARP, IRDMA_HMC_IW_APBVT_ENTRY, IRDMA_HMC_IW_MR,
	IRDMA_HMC_IW_XF, IRDMA_HMC_IW_XFFL, IRDMA_HMC_IW_Q1, IRDMA_HMC_IW_Q1FL,
	IRDMA_HMC_IW_TIMER, IRDMA_HMC_IW_FSIMC, IRDMA_HMC_IW_FSIAV,
	IRDMA_HMC_IW_PBLE, IRDMA_HMC_IW_RRF, IRDMA_HMC_IW_RRFFL,
	IRDMA_HMC_IW_HDR,
	IRDMA_HMC_IW_MD, IRDMA_HMC_IW_OOISC, IRDMA_HMC_IW_OOISCFFL,
	IRDMA_HMC_IW_MAX };

struct irdma_hmc_obj_info { u64 base; u32 max_cnt; u32 cnt; u64 size; };
struct irdma_hmc_info {
	struct irdma_hmc_obj_info *hmc_obj;
	struct { u32 sd_cnt; } sd_table;
	u16 first_sd_index;
};
struct irdma_hmc_fpm_misc {
	u32 max_ceqs, max_sds, xf_block_size, q1_block_size, ht_multiplier,
	    timer_bucket, rrf_block_size, ooiscf_block_size;
};
struct irdma_sc_dev { struct irdma_hmc_fpm_misc hmc_fpm_misc; };
typedef struct irdma { int errors; } irdma_t;

static irdma_t the_irdma;
#define	IRDMA_FROM_DEV(d)	((void)(d), &the_irdma)

static void
irdma_error(irdma_t *irdma, const char *fmt, ...)
{
	(void) fmt;
	irdma->errors++;
}

#include "fpm_bodies.h"

static struct irdma_hmc_obj_info obj[IRDMA_HMC_IW_MAX];
static struct irdma_hmc_info hmc = { obj, { 0 }, 0 };
static struct irdma_sc_dev dev;

/* Plausible E810 answers to the FPM query. */
static void
good(void)
{
	uint_t i;

	memset(obj, 0, sizeof (obj));
	for (i = 0; i < IRDMA_HMC_IW_MAX; i++) {
		obj[i].max_cnt = 4096;
		obj[i].size = 64;
	}
	obj[IRDMA_HMC_IW_PBLE].max_cnt = 1 << 24;
	obj[IRDMA_HMC_IW_PBLE].size = 8;
	obj[IRDMA_HMC_IW_APBVT_ENTRY].max_cnt = 1;
	obj[IRDMA_HMC_IW_APBVT_ENTRY].size = 8192;
	obj[IRDMA_HMC_IW_OOISC].max_cnt = 0;
	obj[IRDMA_HMC_IW_OOISC].size = 0;
	memset(&dev, 0, sizeof (dev));
	dev.hmc_fpm_misc.max_ceqs = 256;
	dev.hmc_fpm_misc.max_sds = 3072;
	dev.hmc_fpm_misc.xf_block_size = 16;
	dev.hmc_fpm_misc.q1_block_size = 64;
	dev.hmc_fpm_misc.ht_multiplier = 8;
	dev.hmc_fpm_misc.timer_bucket = 256;
	dev.hmc_fpm_misc.rrf_block_size = 64;
	the_irdma.errors = 0;
}

static int
query(void)
{
	return (irdma_osdep_fpm_query_check(&dev, &hmc, &dev.hmc_fpm_misc));
}

static u32 req[IRDMA_HMC_IW_MAX];

/*
 * A committed layout: every object packed after the one before, with the
 * counts the driver asked for.
 */
static void
commit_layout(u32 sds)
{
	u64 base = 0;
	uint_t i;

	for (i = 0; i < IRDMA_HMC_IW_MAX; i++) {
		obj[i].cnt = obj[i].max_cnt == 0 ? 0 :
		    (i == IRDMA_HMC_IW_PBLE ? 4096 : obj[i].max_cnt / 2);
		req[i] = obj[i].cnt;
		obj[i].base = base;
		base += (u64)obj[i].cnt * obj[i].size;
		base = (base + 511) & ~511ULL;
	}
	hmc.sd_table.sd_cnt = sds;
}

int
main(void)
{
	struct irdma_hmc_fpm_misc *m = &dev.hmc_fpm_misc;
	int cases = 0;

	good();
	assert(query() == 0 && the_irdma.errors == 0);

	/* Each misc value that the core divides by, loops on or indexes. */
#define	REJECT(field, value)	do {				\
	good(); m->field = (value);				\
	assert(query() == -EINVAL && the_irdma.errors == 1);	\
	cases++;						\
} while (0)
	REJECT(max_sds, 0);
	REJECT(max_sds, IRDMA_HMC_MAX_SD_COUNT + 1);
	REJECT(max_ceqs, 0);
	REJECT(max_ceqs, IRDMA_CEQ_MAX_COUNT + 1);
	REJECT(xf_block_size, 0);	/* divides XF even with XF unused */
	REJECT(xf_block_size, 1U << 31);
	REJECT(q1_block_size, 0);
	REJECT(rrf_block_size, 1U << 20);
	REJECT(ooiscf_block_size, UINT32_MAX);
	REJECT(ht_multiplier, 0);
	REJECT(ht_multiplier, 17);
	REJECT(timer_bucket, 1U << 20);

	/* Object sizes and counts that overflow the core arithmetic. */
#define	REJECT_OBJ(type, field, value)	do {			\
	good(); obj[type].field = (value);			\
	assert(query() == -EINVAL && the_irdma.errors == 1);	\
	cases++;						\
} while (0)
	REJECT_OBJ(IRDMA_HMC_IW_MR, size, 0);
	REJECT_OBJ(IRDMA_HMC_IW_MR, size, 96);
	REJECT_OBJ(IRDMA_HMC_IW_QP, size, 1ULL << 40);
	REJECT_OBJ(IRDMA_HMC_IW_FSIMC, max_cnt, UINT32_MAX);
	REJECT_OBJ(IRDMA_HMC_IW_QP, max_cnt, 0);
	REJECT_OBJ(IRDMA_HMC_IW_CQ, max_cnt, 0);
	REJECT_OBJ(IRDMA_HMC_IW_Q1, max_cnt, 1023);	/* irdma_q1_cnt loop */
	REJECT_OBJ(IRDMA_HMC_IW_PBLE, max_cnt, 1023);
	/* The core sets each size to BIT_ULL(size field from firmware). */
	{
		volatile u32 shift[] = { 64, 65, 255, UINT32_MAX };
		uint_t i;

		for (i = 0; i < sizeof (shift) / sizeof (shift[0]); i++) {
			REJECT_OBJ(IRDMA_HMC_IW_QP, size, BIT_ULL(shift[i]));
			REJECT_OBJ(IRDMA_HMC_IW_TIMER, size, BIT_ULL(shift[i]));
		}
		assert(BIT_ULL(shift[0] - 1) == 1ULL << 63);
	}
	/* An absent object may report no size. */
	good();
	obj[IRDMA_HMC_IW_SRQ].max_cnt = 0;
	obj[IRDMA_HMC_IW_SRQ].size = 0;
	assert(query() == 0);

	/* The committed layout must lie inside the SD table. */
	good();
	commit_layout(64);
	assert(irdma_osdep_fpm_commit_check(&dev, &hmc, req) == 0);
#define	REJECT_COMMIT(stmt)	do {					\
	good(); commit_layout(64); stmt;				\
	assert(irdma_osdep_fpm_commit_check(&dev, &hmc, req) ==		\
	    -EINVAL);							\
	cases++;							\
} while (0)
	REJECT_COMMIT(hmc.sd_table.sd_cnt = 0);
	REJECT_COMMIT(hmc.sd_table.sd_cnt = m->max_sds + 1);
	REJECT_COMMIT(obj[IRDMA_HMC_IW_MR].cnt = obj[IRDMA_HMC_IW_MR].max_cnt
	    + 1);
	REJECT_COMMIT(obj[IRDMA_HMC_IW_PBLE].base = 64 *
	    IRDMA_HMC_DIRECT_BP_SIZE);
	REJECT_COMMIT(obj[IRDMA_HMC_IW_PBLE].base = 64 *
	    IRDMA_HMC_DIRECT_BP_SIZE - 8);
	REJECT_COMMIT(obj[IRDMA_HMC_IW_PBLE].cnt = 1000);
	REJECT_COMMIT(obj[IRDMA_HMC_IW_QP].cnt = 0);
	REJECT_COMMIT(obj[IRDMA_HMC_IW_TIMER].base = UINT64_MAX - 100);
	/* More than the driver asked for, though within the query limit. */
	REJECT_COMMIT(obj[IRDMA_HMC_IW_QP].cnt = req[IRDMA_HMC_IW_QP] + 1);
	REJECT_COMMIT(obj[IRDMA_HMC_IW_MR].cnt = req[IRDMA_HMC_IW_MR] * 2);

	(void) printf("PASS: FPM checks accept a sane layout and reject %d "
	    "hostile values\n", cases);
	return (0);
}
