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
 * rdmat - rdmak test client
 *
 * A pseudo driver that is an rdmak client and lets a privileged process in
 * the global zone drive RDMA verbs through ioctls, for acceptance tests
 * between two hosts or between two QPs of one device.  rdmat_ioctl.h is the
 * interface; rdmat_run.c does the work.
 *
 * Each open of /dev/rdmat gets its own minor and session.  When rdmak
 * removes a device, every session on it is torn down: a running ioctl sees
 * ts_dying, returns, and the session's objects are destroyed before the
 * remove callback returns.  The session stays until it is closed.
 */

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/cred.h>
#include <sys/policy.h>
#include <sys/zone.h>
#include <sys/id_space.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/mkdev.h>

#include "rdmat_impl.h"

static dev_info_t *rdmat_dip;
static void *rdmat_state;
static id_space_t *rdmat_minors;
static kmutex_t rdmat_lock;
static kcondvar_t rdmat_cv;
static list_t rdmat_devs;
static boolean_t rdmat_registered;

static int rdmat_client_add(struct rdk_device *);
static void rdmat_client_remove(struct rdk_device *, void *);

static struct rdk_client rdmat_client = {
	.name = "rdmat",
	.add = rdmat_client_add,
	.remove = rdmat_client_remove
};

static int
rdmat_client_add(struct rdk_device *dev)
{
	rdmat_dev_t *td;

	td = kmem_zalloc(sizeof (*td), KM_SLEEP);
	td->td_dev = dev;
	list_create(&td->td_sessions, sizeof (rdmat_sess_t),
	    offsetof(rdmat_sess_t, ts_node));
	mutex_enter(&rdmat_lock);
	list_insert_tail(&rdmat_devs, td);
	mutex_exit(&rdmat_lock);
	rdk_set_client_data(dev, &rdmat_client, td);
	return (0);
}

/* Stop the session's ioctl, if any, and destroy its objects. */
static void
rdmat_sess_kill(rdmat_sess_t *ts, boolean_t removing)
{
	uint_t i;

	mutex_enter(&ts->ts_lock);
	ts->ts_dying = B_TRUE;
	for (i = 0; i < RDMAT_MAX_QPS; i++) {
		if (ts->ts_qp[i].tq_sess == NULL)
			continue;
		mutex_enter(&ts->ts_qp[i].tq_lock);
		cv_broadcast(&ts->ts_qp[i].tq_cv);
		mutex_exit(&ts->ts_qp[i].tq_lock);
	}
	while (ts->ts_busy)
		cv_wait(&ts->ts_cv, &ts->ts_lock);
	if (!ts->ts_dead) {
		rdmat_teardown(ts, removing);
		ts->ts_dead = B_TRUE;
	}
	mutex_exit(&ts->ts_lock);
}

static void
rdmat_client_remove(struct rdk_device *dev, void *arg)
{
	rdmat_dev_t *td = arg;
	rdmat_sess_t *ts;

	_NOTE(ARGUNUSED(dev));
	mutex_enter(&rdmat_lock);
	list_remove(&rdmat_devs, td);
	while ((ts = list_remove_head(&td->td_sessions)) != NULL) {
		/* The hold keeps close from freeing the session under us. */
		ts->ts_tdev = NULL;
		ts->ts_holds++;
		mutex_exit(&rdmat_lock);
		rdmat_sess_kill(ts, B_TRUE);
		mutex_enter(&rdmat_lock);
		if (--ts->ts_holds == 0)
			cv_broadcast(&rdmat_cv);
	}
	mutex_exit(&rdmat_lock);
	list_destroy(&td->td_sessions);
	kmem_free(td, sizeof (*td));
}

static int
rdmat_devices(intptr_t arg, int mode)
{
	rdmat_devices_t *out;
	struct rdk_port_attr pa;
	rdmat_devinfo_t *di;
	rdmat_dev_t *td;
	int ret = 0;

	out = kmem_zalloc(sizeof (*out), KM_SLEEP);
	mutex_enter(&rdmat_lock);
	for (td = list_head(&rdmat_devs); td != NULL &&
	    out->rdd_count < RDMAT_MAX_DEVS; td = list_next(&rdmat_devs, td)) {
		struct rdk_device *dev = td->td_dev;

		di = &out->rdd_devs[out->rdd_count++];
		(void) strlcpy(di->rdi_name, dev->rd_name,
		    sizeof (di->rdi_name));
		if (rdk_query_port(dev, 1, &pa) == 0) {
			di->rdi_port_state = pa.state;
			di->rdi_active_mtu =
			    (uint32_t)rdk_mtu_enum_to_int(pa.active_mtu);
			di->rdi_phys_mtu = pa.phys_mtu;
			bcopy(pa.mac, di->rdi_mac, sizeof (di->rdi_mac));
			di->rdi_speed = pa.speed;
		}
		di->rdi_max_qp = (uint32_t)dev->rd_attr.max_qp;
		di->rdi_max_qp_wr = (uint32_t)dev->rd_attr.max_qp_wr;
		di->rdi_max_sge = (uint32_t)dev->rd_attr.max_send_sge;
		di->rdi_max_mr_pages = dev->rd_attr.max_fast_reg_page_list_len;
	}
	mutex_exit(&rdmat_lock);
	if (ddi_copyout(out, (void *)arg, sizeof (*out), mode) != 0)
		ret = EFAULT;
	kmem_free(out, sizeof (*out));
	return (ret);
}

/*
 * Bind the session to the named device.  The session holds no reference:
 * rdmak's remove callback tears it down before the device goes.
 */
static int
rdmat_bind(rdmat_sess_t *ts, const char *name)
{
	rdmat_dev_t *td;

	mutex_enter(&rdmat_lock);
	for (td = list_head(&rdmat_devs); td != NULL;
	    td = list_next(&rdmat_devs, td)) {
		if (strcmp(td->td_dev->rd_name, name) == 0)
			break;
	}
	if (td == NULL) {
		mutex_exit(&rdmat_lock);
		return (ENXIO);
	}
	ts->ts_tdev = td;
	ts->ts_dev = td->td_dev;
	list_insert_tail(&td->td_sessions, ts);
	mutex_exit(&rdmat_lock);
	return (0);
}

static void
rdmat_unbind(rdmat_sess_t *ts)
{
	mutex_enter(&rdmat_lock);
	if (ts->ts_tdev != NULL) {
		list_remove(&ts->ts_tdev->td_sessions, ts);
		ts->ts_tdev = NULL;
	}
	mutex_exit(&rdmat_lock);
}

static int
rdmat_open(dev_t *devp, int flag, int otyp, cred_t *cr)
{
	rdmat_sess_t *ts;
	id_t minor;

	_NOTE(ARGUNUSED(flag));
	if (otyp != OTYP_CHR)
		return (EINVAL);
	if (crgetzoneid(cr) != GLOBAL_ZONEID ||
	    secpolicy_sys_config(cr, B_FALSE) != 0)
		return (EPERM);
	if (getminor(*devp) != 0 || rdmat_dip == NULL)
		return (ENXIO);

	if ((minor = id_alloc_nosleep(rdmat_minors)) == -1)
		return (EAGAIN);
	if (ddi_soft_state_zalloc(rdmat_state, minor) != DDI_SUCCESS) {
		id_free(rdmat_minors, minor);
		return (ENOMEM);
	}
	ts = ddi_get_soft_state(rdmat_state, minor);
	ts->ts_minor = (minor_t)minor;
	mutex_init(&ts->ts_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&ts->ts_cv, NULL, CV_DRIVER, NULL);
	*devp = makedevice(getmajor(*devp), (minor_t)minor);
	return (0);
}

static int
rdmat_close(dev_t dev, int flag, int otyp, cred_t *cr)
{
	minor_t minor = getminor(dev);
	rdmat_sess_t *ts;

	_NOTE(ARGUNUSED(flag, otyp, cr));
	if ((ts = ddi_get_soft_state(rdmat_state, minor)) == NULL)
		return (0);
	rdmat_unbind(ts);
	rdmat_sess_kill(ts, B_FALSE);
	mutex_enter(&rdmat_lock);
	while (ts->ts_holds != 0)
		cv_wait(&rdmat_cv, &rdmat_lock);
	mutex_exit(&rdmat_lock);
	cv_destroy(&ts->ts_cv);
	mutex_destroy(&ts->ts_lock);
	ddi_soft_state_free(rdmat_state, minor);
	id_free(rdmat_minors, (id_t)minor);
	return (0);
}

/* Copy in, run and copy out one request of a fixed size. */
static int
rdmat_call(rdmat_sess_t *ts, int cmd, intptr_t arg, int mode)
{
	union {
		rdmat_setup_t	setup;
		rdmat_connect_t	connect;
		rdmat_run_t	run;
		rdmat_buf_t	buf;
		rdmat_query_t	query;
	} *u;
	size_t len;
	int ret;

	switch (cmd) {
	case RDMAT_IOC_SETUP:
		len = sizeof (rdmat_setup_t);
		break;
	case RDMAT_IOC_CONNECT:
		len = sizeof (rdmat_connect_t);
		break;
	case RDMAT_IOC_RUN:
		len = sizeof (rdmat_run_t);
		break;
	case RDMAT_IOC_BUF:
		len = sizeof (rdmat_buf_t);
		break;
	case RDMAT_IOC_QUERY:
		len = sizeof (rdmat_query_t);
		break;
	default:
		return (ENOTTY);
	}

	u = kmem_zalloc(sizeof (*u), KM_SLEEP);
	if (ddi_copyin((void *)arg, u, len, mode) != 0) {
		kmem_free(u, sizeof (*u));
		return (EFAULT);
	}

	switch (cmd) {
	case RDMAT_IOC_SETUP:
		u->setup.rs_dev[RDMAT_NAME_MAX - 1] = '\0';
		if (ts->ts_setup) {
			ret = EBUSY;
			break;
		}
		if ((ret = rdmat_bind(ts, u->setup.rs_dev)) != 0)
			break;
		if ((ret = rdmat_setup(ts, &u->setup)) != 0)
			rdmat_unbind(ts);
		break;
	case RDMAT_IOC_CONNECT:
		ret = ts->ts_setup ? rdmat_connect(ts, &u->connect) : ENXIO;
		break;
	case RDMAT_IOC_RUN:
		ret = ts->ts_setup ? rdmat_run(ts, &u->run) : ENXIO;
		break;
	case RDMAT_IOC_BUF:
		ret = ts->ts_setup ? rdmat_buf(ts, &u->buf) : ENXIO;
		break;
	default:
		ret = ts->ts_setup ? rdmat_query(ts, &u->query) : ENXIO;
		break;
	}

	/* A run reports its counts even when it fails. */
	if ((ret == 0 || cmd == RDMAT_IOC_RUN) &&
	    ddi_copyout(u, (void *)arg, len, mode) != 0)
		ret = EFAULT;
	kmem_free(u, sizeof (*u));
	return (ret);
}

static int
rdmat_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *cr,
    int *rvalp)
{
	rdmat_sess_t *ts;
	int ret;

	_NOTE(ARGUNUSED(rvalp));
	if (crgetzoneid(cr) != GLOBAL_ZONEID ||
	    secpolicy_sys_config(cr, B_FALSE) != 0)
		return (EPERM);
	if (cmd == RDMAT_IOC_DEVICES)
		return (rdmat_devices(arg, mode));
	if ((ts = ddi_get_soft_state(rdmat_state, getminor(dev))) == NULL)
		return (ENXIO);

	mutex_enter(&ts->ts_lock);
	if (ts->ts_dead || ts->ts_dying) {
		mutex_exit(&ts->ts_lock);
		return (ENXIO);
	}
	if (ts->ts_busy) {
		mutex_exit(&ts->ts_lock);
		return (EBUSY);
	}
	ts->ts_busy = B_TRUE;
	mutex_exit(&ts->ts_lock);

	ret = rdmat_call(ts, cmd, arg, mode);

	mutex_enter(&ts->ts_lock);
	ts->ts_busy = B_FALSE;
	cv_broadcast(&ts->ts_cv);
	mutex_exit(&ts->ts_lock);
	return (ret);
}

static int
rdmat_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	int ret;

	if (cmd != DDI_ATTACH || ddi_get_instance(dip) != 0 ||
	    rdmat_dip != NULL)
		return (DDI_FAILURE);
	if (ddi_create_minor_node(dip, "rdmat", S_IFCHR, 0, DDI_PSEUDO, 0) !=
	    DDI_SUCCESS)
		return (DDI_FAILURE);
	rdmat_dip = dip;
	if ((ret = rdk_register_client(&rdmat_client)) != 0) {
		dev_err(dip, CE_WARN, "!failed to register with rdmak: %d",
		    ret);
		ddi_remove_minor_node(dip, NULL);
		rdmat_dip = NULL;
		return (DDI_FAILURE);
	}
	rdmat_registered = B_TRUE;
	return (DDI_SUCCESS);
}

/* Open sessions hold the module; by now none is left. */
static int
rdmat_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);
	if (rdmat_registered) {
		rdk_unregister_client(&rdmat_client);
		rdmat_registered = B_FALSE;
	}
	ddi_remove_minor_node(dip, NULL);
	rdmat_dip = NULL;
	return (DDI_SUCCESS);
}

static int
rdmat_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	_NOTE(ARGUNUSED(dip, arg));
	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		if (rdmat_dip == NULL)
			return (DDI_FAILURE);
		*result = rdmat_dip;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)0;
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}
}

static struct cb_ops rdmat_cb_ops = {
	.cb_open = rdmat_open,
	.cb_close = rdmat_close,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = rdmat_ioctl,
	.cb_devmap = nodev,
	.cb_mmap = nodev,
	.cb_segmap = nodev,
	.cb_chpoll = nochpoll,
	.cb_prop_op = ddi_prop_op,
	.cb_str = NULL,
	.cb_flag = D_MP,
	.cb_rev = CB_REV,
	.cb_aread = nodev,
	.cb_awrite = nodev
};

static struct dev_ops rdmat_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = rdmat_getinfo,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = rdmat_attach,
	.devo_detach = rdmat_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &rdmat_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	.devo_quiesce = ddi_quiesce_not_needed
};

static struct modldrv rdmat_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "RDMA verbs test client",
	.drv_dev_ops = &rdmat_dev_ops
};

static struct modlinkage rdmat_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &rdmat_modldrv, NULL }
};

int
_init(void)
{
	int ret;

	if ((ret = ddi_soft_state_init(&rdmat_state, sizeof (rdmat_sess_t),
	    4)) != 0)
		return (ret);
	rdmat_minors = id_space_create("rdmat_minors", 1, MAXMIN32);
	mutex_init(&rdmat_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&rdmat_cv, NULL, CV_DRIVER, NULL);
	list_create(&rdmat_devs, sizeof (rdmat_dev_t),
	    offsetof(rdmat_dev_t, td_node));
	if ((ret = mod_install(&rdmat_modlinkage)) != 0) {
		list_destroy(&rdmat_devs);
		cv_destroy(&rdmat_cv);
		mutex_destroy(&rdmat_lock);
		id_space_destroy(rdmat_minors);
		ddi_soft_state_fini(&rdmat_state);
	}
	return (ret);
}

int
_info(struct modinfo *mi)
{
	return (mod_info(&rdmat_modlinkage, mi));
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&rdmat_modlinkage)) != 0)
		return (ret);
	list_destroy(&rdmat_devs);
	cv_destroy(&rdmat_cv);
	mutex_destroy(&rdmat_lock);
	id_space_destroy(rdmat_minors);
	ddi_soft_state_fini(&rdmat_state);
	return (0);
}
