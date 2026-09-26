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
 * DMA memory for the child.  A buffer the device may still reach is kept in a
 * quarantine until the SGE is stopped.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>

#include "common/common.h"
#include "t4_ofld.h"

int
t4_ofld_dma_alloc(t4_ofld_t *of, size_t len, size_t align,
    t4_rdma_dma_t **dmap)
{
	struct adapter *sc = of->of_sc;
	ddi_dma_attr_t attr = sc->sge.dma_attr_desc;
	ddi_device_acc_attr_t acc = sc->sge.acc_attr_desc;
	ddi_dma_cookie_t dc;
	t4_ofld_buf_t *ob;
	size_t real;
	uint_t ncookies;
	uint32_t gen;
	int rc;

	if (dmap == NULL || len == 0 || len > T4_OFLD_DMA_MAX_LEN ||
	    align == 0 || !ISP2(align) || align > T4_OFLD_DMA_MAX_ALIGN)
		return (EINVAL);
	*dmap = NULL;
	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);

	mutex_enter(&of->of_dma_lock);
	if (of->of_dma_bytes + of->of_quar_bytes + len > T4_OFLD_DMA_LIMIT) {
		mutex_exit(&of->of_dma_lock);
		return (ENOMEM);
	}
	of->of_dma_bytes += len;
	mutex_exit(&of->of_dma_lock);

	ob = kmem_zalloc(sizeof (*ob), KM_SLEEP);
	attr.dma_attr_align = MAX(align, attr.dma_attr_align);
	if (ddi_dma_alloc_handle(sc->dip, &attr, DDI_DMA_SLEEP, NULL,
	    &ob->ob_dhdl) != DDI_SUCCESS)
		goto fail;
	if (ddi_dma_mem_alloc(ob->ob_dhdl, len, &acc, DDI_DMA_CONSISTENT,
	    DDI_DMA_SLEEP, NULL, &ob->ob_pub.trd_va, &real, &ob->ob_ahdl) !=
	    DDI_SUCCESS)
		goto fail;
	if (ddi_dma_addr_bind_handle(ob->ob_dhdl, NULL, ob->ob_pub.trd_va,
	    real, DDI_DMA_RDWR | DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL, &dc,
	    &ncookies) != DDI_DMA_MAPPED)
		goto fail;
	if (ncookies != 1) {
		(void) ddi_dma_unbind_handle(ob->ob_dhdl);
		goto fail;
	}
	bzero(ob->ob_pub.trd_va, real);
	ob->ob_pub.trd_pa = dc.dmac_laddress;
	ob->ob_pub.trd_len = len;

	mutex_enter(&of->of_dma_lock);
	list_insert_tail(&of->of_bufs, ob);
	mutex_exit(&of->of_dma_lock);
	*dmap = &ob->ob_pub;
	return (0);
fail:
	if (ob->ob_ahdl != NULL)
		ddi_dma_mem_free(&ob->ob_ahdl);
	if (ob->ob_dhdl != NULL)
		ddi_dma_free_handle(&ob->ob_dhdl);
	kmem_free(ob, sizeof (*ob));
	mutex_enter(&of->of_dma_lock);
	of->of_dma_bytes -= len;
	mutex_exit(&of->of_dma_lock);
	return (ENOMEM);
}

static void
t4_ofld_buf_free(t4_ofld_buf_t *ob)
{
	(void) ddi_dma_unbind_handle(ob->ob_dhdl);
	ddi_dma_mem_free(&ob->ob_ahdl);
	ddi_dma_free_handle(&ob->ob_dhdl);
	kmem_free(ob, sizeof (*ob));
}

/*
 * Free a buffer now if the client says the device is done with it and the
 * adapter is healthy; otherwise keep it until the adapter is stopped.  An
 * unknown pointer is ignored.
 */
void
t4_ofld_dma_free(t4_ofld_t *of, t4_rdma_dma_t *dma, boolean_t quiesced)
{
	t4_ofld_buf_t *ob;
	boolean_t now;

	if (dma == NULL)
		return;
	mutex_enter(&of->of_dma_lock);
	for (ob = list_head(&of->of_bufs); ob != NULL;
	    ob = list_next(&of->of_bufs, ob)) {
		if (&ob->ob_pub == dma)
			break;
	}
	if (ob == NULL) {
		mutex_exit(&of->of_dma_lock);
		return;
	}
	list_remove(&of->of_bufs, ob);
	of->of_dma_bytes -= ob->ob_pub.trd_len;
	mutex_enter(&of->of_lock);
	now = quiesced && !of->of_fatal;
	mutex_exit(&of->of_lock);
	if (!now) {
		of->of_quar_bytes += ob->ob_pub.trd_len;
		list_insert_tail(&of->of_quar, ob);
	}
	mutex_exit(&of->of_dma_lock);
	if (now)
		t4_ofld_buf_free(ob);
}

/* The client left: whatever it did not free may still be in use. */
void
t4_ofld_dma_close(t4_ofld_t *of)
{
	t4_ofld_buf_t *ob;

	mutex_enter(&of->of_dma_lock);
	while ((ob = list_remove_head(&of->of_bufs)) != NULL) {
		of->of_dma_bytes -= ob->ob_pub.trd_len;
		of->of_quar_bytes += ob->ob_pub.trd_len;
		list_insert_tail(&of->of_quar, ob);
	}
	mutex_exit(&of->of_dma_lock);
}

/*
 * Free the quarantine once the SGE is stopped; if it could not be stopped,
 * leak the buffers rather than hand memory the device may write to back.
 */
void
t4_ofld_dma_fini(t4_ofld_t *of, boolean_t stopped)
{
	t4_ofld_buf_t *ob;

	t4_ofld_dma_close(of);
	mutex_enter(&of->of_dma_lock);
	if (!stopped && !list_is_empty(&of->of_quar)) {
		cxgb_printf(of->of_sc->dip, CE_WARN, "leaking %" PRIu64
		    " bytes of RDMA memory the device may still reach",
		    of->of_quar_bytes);
		while (list_remove_head(&of->of_quar) != NULL)
			;
	}
	while ((ob = list_remove_head(&of->of_quar)) != NULL) {
		mutex_exit(&of->of_dma_lock);
		t4_ofld_buf_free(ob);
		mutex_enter(&of->of_dma_lock);
	}
	of->of_quar_bytes = 0;
	mutex_exit(&of->of_dma_lock);
}
