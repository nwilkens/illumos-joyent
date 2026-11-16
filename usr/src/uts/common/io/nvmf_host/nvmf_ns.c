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
 * Provenance: re-bound to illumos from FreeBSD sys/dev/nvmf/host/nvmf_ns.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * The namespaces of one association, keyed by NSID.  Each namespace owns the
 * path that binds it to a blkdev disk.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/avl.h>
#include <sys/nvme.h>

#include "nvmf_var.h"

typedef struct nvmf_namespace {
	avl_node_t		node;
	uint32_t		id;
	uint32_t		lba_size;
	uint64_t		size;		/* in bytes */
	uint8_t			nguid[16];
	uint8_t			eui64[8];
	struct nvmf_path	*path;
} nvmf_namespace_t;

static int
nvmf_ns_compare(const void *a, const void *b)
{
	const nvmf_namespace_t *l = a;
	const nvmf_namespace_t *r = b;

	if (l->id < r->id)
		return (-1);
	return (l->id > r->id ? 1 : 0);
}

void
nvmf_ns_init(nvmf_softc_t *sc)
{
	avl_create(&sc->ns_tree, nvmf_ns_compare, sizeof (nvmf_namespace_t),
	    offsetof(nvmf_namespace_t, node));
}

void
nvmf_ns_fini(nvmf_softc_t *sc)
{
	avl_destroy(&sc->ns_tree);
}

/*
 * blkdev addresses a transfer in blocks of at least DEV_BSIZE, and READ and
 * WRITE carry a 16-bit block count, so only 512 byte to 64 KiB blocks are
 * presented.
 */
#define	NVMF_LBADS_MIN	9
#define	NVMF_LBADS_MAX	16

/*
 * Validate Identify Namespace data and derive the block count and block size.
 * dip may be NULL to suppress the warnings.
 */
boolean_t
nvmf_ns_fmt(const nvme_identify_nsid_t *data, uint64_t *nblksp,
    uint32_t *blksizep, dev_info_t *dip, uint32_t nsid)
{
	uint8_t lbaf, lbads;

	if (data->id_dps.dp_pinfo != 0) {
		if (dip != NULL)
			dev_err(dip, CE_WARN,
			    "!ns%u: End-to-end data protection not supported",
			    nsid);
		return (B_FALSE);
	}

	/* Formats 16 and up are in a part of the data that is not read. */
	lbaf = data->id_flbas.lba_format | (data->id_flbas.lba_fidxu << 4);
	if (lbaf > data->id_nlbaf || lbaf >= NVME_MAX_LBAF) {
		if (dip != NULL)
			dev_err(dip, CE_WARN,
			    "!ns%u: Invalid LBA format index", nsid);
		return (B_FALSE);
	}

	if (data->id_lbaf[lbaf].lbaf_ms != 0) {
		if (dip != NULL)
			dev_err(dip, CE_WARN,
			    "!ns%u: Namespaces with metadata are not supported",
			    nsid);
		return (B_FALSE);
	}

	lbads = data->id_lbaf[lbaf].lbaf_lbads;
	if (lbads < NVMF_LBADS_MIN || lbads > NVMF_LBADS_MAX) {
		if (dip != NULL)
			dev_err(dip, CE_WARN,
			    "!ns%u: unsupported LBA data size 2^%u", nsid,
			    lbads);
		return (B_FALSE);
	}

	*blksizep = 1U << lbads;
	*nblksp = data->id_nsize;
	return (B_TRUE);
}

static nvmf_namespace_t *
nvmf_ns_lookup(nvmf_softc_t *sc, uint32_t nsid, avl_index_t *where)
{
	nvmf_namespace_t key;

	ASSERT(MUTEX_HELD(&sc->lock));

	key.id = nsid;
	return (avl_find(&sc->ns_tree, &key, where));
}

static void
nvmf_ns_remove(nvmf_softc_t *sc, nvmf_namespace_t *ns)
{
	mutex_enter(&sc->lock);
	avl_remove(&sc->ns_tree, ns);
	mutex_exit(&sc->lock);

	nvmf_mpath_remove_path(ns->path);
	kmem_free(ns, sizeof (*ns));
}

static void
nvmf_ns_add(nvmf_softc_t *sc, uint32_t nsid, const nvme_identify_nsid_t *data)
{
	nvmf_namespace_t *ns;
	struct nvmf_path *path;
	uint64_t nblks;
	uint32_t lba_size;
	ulong_t count;

	mutex_enter(&sc->lock);
	count = avl_numnodes(&sc->ns_tree);
	mutex_exit(&sc->lock);
	if (count >= nvmf_max_namespaces) {
		dev_err(sc->dip, CE_WARN,
		    "!ns%u: ignored, %u namespaces already attached", nsid,
		    nvmf_max_namespaces);
		return;
	}

	if (!nvmf_ns_fmt(data, &nblks, &lba_size, sc->dip, nsid))
		return;

	path = nvmf_mpath_add_path(sc, nsid, data, nblks, lba_size);
	if (path == NULL)
		return;

	ns = kmem_zalloc(sizeof (*ns), KM_SLEEP);
	ns->id = nsid;
	ns->lba_size = lba_size;
	ns->size = nblks * lba_size;
	bcopy(data->id_nguid, ns->nguid, sizeof (ns->nguid));
	bcopy(data->id_eui64, ns->eui64, sizeof (ns->eui64));
	ns->path = path;

	mutex_enter(&sc->lock);
	avl_add(&sc->ns_tree, ns);
	mutex_exit(&sc->lock);
}

/*
 * Bring one namespace in line with fresh Identify data.  A namespace whose
 * identity or block size changed is removed and added again, never altered.
 */
void
nvmf_ns_reconcile(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data)
{
	nvmf_namespace_t *ns;
	uint64_t nblks;
	uint32_t lba_size;

	mutex_enter(&sc->lock);
	ns = nvmf_ns_lookup(sc, nsid, NULL);
	mutex_exit(&sc->lock);

	/* As in nvme_ns_construct, a size of zero is an inactive namespace. */
	if (data->id_nsize == 0) {
		if (ns != NULL)
			nvmf_ns_remove(sc, ns);
		return;
	}

	if (ns != NULL) {
		if (nvmf_ns_fmt(data, &nblks, &lba_size, NULL, nsid) &&
		    nvmf_mpath_path_update(ns->path, data, nblks, lba_size)) {
			mutex_enter(&sc->lock);
			ns->size = nblks * lba_size;
			mutex_exit(&sc->lock);
			return;
		}
		nvmf_ns_remove(sc, ns);
	}

	nvmf_ns_add(sc, nsid, data);
}

/* Remove every namespace with first <= NSID < end. */
void
nvmf_ns_purge(nvmf_softc_t *sc, uint32_t first, uint32_t end)
{
	nvmf_namespace_t *ns;
	avl_index_t where;

	for (;;) {
		mutex_enter(&sc->lock);
		ns = nvmf_ns_lookup(sc, first, &where);
		if (ns == NULL)
			ns = avl_nearest(&sc->ns_tree, where, AVL_AFTER);
		mutex_exit(&sc->lock);

		if (ns == NULL || ns->id >= end)
			break;
		nvmf_ns_remove(sc, ns);
	}
}

void
nvmf_ns_remove_all(nvmf_softc_t *sc)
{
	nvmf_ns_purge(sc, 0, NVME_NSID_BCAST);
}

/*
 * Compare a namespace with what a new association reports for its NSID.  If
 * it is not the same namespace, or data is NULL because it is not there at
 * all, I/O to it fails from here on.  The next rescan replaces it.
 */
void
nvmf_ns_revalidate(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data)
{
	nvmf_namespace_t *ns;
	uint64_t nblks;
	uint32_t lba_size = 0;
	boolean_t same;

	same = data != NULL && data->id_nsize != 0 &&
	    nvmf_ns_fmt(data, &nblks, &lba_size, NULL, nsid);

	mutex_enter(&sc->lock);
	ns = nvmf_ns_lookup(sc, nsid, NULL);
	if (ns != NULL && (!same || lba_size != ns->lba_size ||
	    !nvmf_mpath_path_matches(ns->path, data))) {
		dev_err(sc->dip, CE_WARN,
		    "!ns%u: namespace changed while disconnected", nsid);
		nvmf_mpath_path_set_gone(ns->path);
	}
	mutex_exit(&sc->lock);
}

/*
 * Copy up to max namespaces into info and return how many there are in all.
 */
uint_t
nvmf_ns_snapshot(nvmf_softc_t *sc, nvmf_ns_info_t *info, uint_t max)
{
	nvmf_namespace_t *ns;
	uint_t count, i = 0;

	mutex_enter(&sc->lock);
	count = (uint_t)avl_numnodes(&sc->ns_tree);
	for (ns = avl_first(&sc->ns_tree); ns != NULL && i < max;
	    ns = AVL_NEXT(&sc->ns_tree, ns), i++) {
		info[i].ni_nsid = ns->id;
		info[i].ni_blksize = ns->lba_size;
		info[i].ni_size = ns->size;
		bcopy(ns->nguid, info[i].ni_nguid, sizeof (ns->nguid));
		bcopy(ns->eui64, info[i].ni_eui64, sizeof (ns->eui64));
	}
	mutex_exit(&sc->lock);
	return (count);
}
