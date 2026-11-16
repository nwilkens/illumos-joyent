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
 * Feed libnvmf's TCP transport forged PDUs over a socketpair, as a host and as
 * a controller.  Each case queues the peer's bytes, runs the library, and then
 * checks what it returned, what it wrote back, that the socket is still open,
 * how much it tried to allocate and that it freed all of it.
 */

/* The library is built with these names pointing at the counters below. */
#undef malloc
#undef calloc
#undef free

#include <sys/types.h>
#include <sys/socket.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libnvmf.h"
#include "nvme_reg.h"

#define	SUBNQN	"nqn.2024-01.com.example:disk0"
#define	HOSTNQN	"nqn.2014-08.org.nvmexpress:uuid:host"

/* No case has a reason to allocate more than a PDU of in-capsule data. */
#define	ALLOC_LIMIT	(64 * 1024)
#define	NPTRS		4096

/* SQHD of a Connect response that turns submission queue flow control off. */
#define	NO_SQHD		0xffff

#define	SGL_ICD		0x01
#define	SGL_CMD_BUFFER	0x5a

static void *live[NPTRS];
static size_t max_alloc;
static const char *test;

void *test_malloc(size_t);
void *test_calloc(size_t, size_t);
void test_free(void *);

static void *
track(void *p, size_t n)
{
	int i;

	if (n > max_alloc)
		max_alloc = n;
	if (p == NULL)
		return (NULL);
	for (i = 0; i < NPTRS; i++) {
		if (live[i] == NULL) {
			live[i] = p;
			return (p);
		}
	}
	abort();
}

void *
test_malloc(size_t n)
{
	/* A refused request is as good as a granted one for these checks. */
	if (n > ALLOC_LIMIT)
		return (track(NULL, n));
	return (track(malloc(n), n));
}

void *
test_calloc(size_t n, size_t sz)
{
	return (track(calloc(n, sz), n * sz));
}

void
test_free(void *p)
{
	int i;

	for (i = 0; p != NULL && i < NPTRS; i++) {
		if (live[i] == p) {
			live[i] = NULL;
			break;
		}
	}
	free(p);
}

#define	CHECK(cond)	do {						\
	if (!(cond)) {							\
		(void) fprintf(stderr, "%s: line %d: %s\n", test,	\
		    __LINE__, #cond);					\
		exit(1);						\
	}								\
} while (0)

static void
check_clean(void)
{
	int i;

	CHECK(max_alloc <= ALLOC_LIMIT);
	for (i = 0; i < NPTRS; i++)
		CHECK(live[i] == NULL);
}

/* The peer's side of the connection: a buffer of bytes to send. */
static uint8_t peer[16384];
static size_t peer_len;

static void
put(const void *buf, size_t len)
{
	assert(peer_len + len <= sizeof (peer));
	(void) memcpy(peer + peer_len, buf, len);
	peer_len += len;
}

static void
put_zero(size_t len)
{
	assert(peer_len + len <= sizeof (peer));
	(void) memset(peer + peer_len, 0, len);
	peer_len += len;
}

static nvmf_tcp_common_pdu_hdr_t
common(uint8_t type, uint8_t flags, uint8_t hlen, uint8_t pdo, uint32_t plen)
{
	nvmf_tcp_common_pdu_hdr_t ch;

	ch.ntcph_pdu_type = type;
	ch.ntcph_flags = flags;
	ch.ntcph_hlen = hlen;
	ch.ntcph_pdo = pdo;
	ch.ntcph_plen = plen;
	return (ch);
}

static nvmf_tcp_ic_req_t
ic_req(void)
{
	nvmf_tcp_ic_req_t ic;

	(void) memset(&ic, 0, sizeof (ic));
	ic.nfic_common = common(NVMF_TCP_PDU_TYPE_IC_REQ, 0, sizeof (ic), 0,
	    sizeof (ic));
	return (ic);
}

static nvmf_tcp_ic_resp_t
ic_resp(uint8_t cpda)
{
	nvmf_tcp_ic_resp_t ic;

	(void) memset(&ic, 0, sizeof (ic));
	ic.nfir_common = common(NVMF_TCP_PDU_TYPE_IC_RESP, 0, sizeof (ic), 0,
	    sizeof (ic));
	ic.nfir_cpda = cpda;
	ic.nfir_maxh2cdata = 4096;
	return (ic);
}

static nvmf_fabric_connect_data_t
connect_data(void)
{
	nvmf_fabric_connect_data_t d;

	(void) memset(&d, 0, sizeof (d));
	d.nfcd_hostid[0] = 1;
	d.nfcd_cntlid = NVMF_CNTLID_DYNAMIC;
	(void) strcpy((char *)d.nfcd_subnqn, SUBNQN);
	(void) strcpy((char *)d.nfcd_hostnqn, HOSTNQN);
	return (d);
}

/* A Connect command capsule header for the admin queue. */
static nvmf_tcp_cmd_t
connect_capsule(uint8_t psdt, uint8_t sgl_type, uint32_t icd_len)
{
	nvmf_tcp_cmd_t cmd;
	uint8_t *sqe = (uint8_t *)cmd.ntc_ccsqe;
	uint32_t len = sizeof (nvmf_fabric_connect_data_t);

	(void) memset(&cmd, 0, sizeof (cmd));
	cmd.ntc_common = common(NVMF_TCP_PDU_TYPE_CAPSULE_CMD, 0, sizeof (cmd),
	    icd_len != 0 ? sizeof (cmd) : 0, sizeof (cmd) + icd_len);
	sqe[0] = 0x7f;
	sqe[1] = psdt << 6;
	sqe[4] = NVMF_FCTYPE_CONNECT;
	(void) memcpy(sqe + 32, &len, sizeof (len));
	sqe[39] = sgl_type;
	sqe[44] = 31;
	return (cmd);
}

/* A response capsule; as a Connect response it grants controller ID 1. */
static nvmf_tcp_rsp_t
response(uint16_t cid, uint16_t sqhd)
{
	nvmf_tcp_rsp_t rsp;
	uint8_t *cqe = (uint8_t *)rsp.ntr_rccqe;

	(void) memset(&rsp, 0, sizeof (rsp));
	rsp.ntr_common = common(NVMF_TCP_PDU_TYPE_CAPSULE_RESP, 0,
	    sizeof (rsp), 0, sizeof (rsp));
	cqe[0] = 1;
	(void) memcpy(cqe + 8, &sqhd, sizeof (sqhd));
	(void) memcpy(cqe + 12, &cid, sizeof (cid));
	return (rsp);
}

static nvmf_tcp_r2t_hdr_t
r2t(uint32_t off, uint32_t len)
{
	nvmf_tcp_r2t_hdr_t h;

	(void) memset(&h, 0, sizeof (h));
	h.ntr2t_common = common(NVMF_TCP_PDU_TYPE_R2T, 0, sizeof (h), 0,
	    sizeof (h));
	h.ntr2t_r2to = off;
	h.ntr2t_r2tl = len;
	return (h);
}

static nvmf_tcp_c2h_data_hdr_t
c2h(uint16_t cid, uint32_t off, uint32_t len, uint8_t flags)
{
	nvmf_tcp_c2h_data_hdr_t h;

	(void) memset(&h, 0, sizeof (h));
	h.ntc2c_common = common(NVMF_TCP_PDU_TYPE_C2H_DATA, flags, sizeof (h),
	    sizeof (h), sizeof (h) + len);
	h.ntc2c_cccid = cid;
	h.ntc2c_datao = off;
	h.ntc2c_datal = len;
	return (h);
}

static nvmf_tcp_h2c_data_hdr_t
h2c(uint32_t off, uint32_t len, uint8_t flags)
{
	nvmf_tcp_h2c_data_hdr_t h;

	(void) memset(&h, 0, sizeof (h));
	h.nth2c_common = common(NVMF_TCP_PDU_TYPE_H2C_DATA, flags, sizeof (h),
	    sizeof (h), sizeof (h) + len);
	h.nth2c_datao = off;
	h.nth2c_datal = len;
	return (h);
}

/* One end for the library, the other preloaded with the peer's bytes. */
static int sv[2];
static struct nvmf_association *na;
static uint8_t reply[16384];
static size_t reply_len;
static bool flow_control;

static void
start(const char *name, bool controller, bool keep_open)
{
	nvmf_association_params_t ap;
	int bufsz = 256 * 1024;

	test = name;
	max_alloc = 0;
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	(void) setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof (bufsz));
	(void) setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof (bufsz));
	CHECK(write(sv[1], peer, peer_len) == (ssize_t)peer_len);
	peer_len = 0;
	if (!keep_open)
		CHECK(shutdown(sv[1], SHUT_WR) == 0);

	(void) memset(&ap, 0, sizeof (ap));
	ap.nap_sq_flow_control = flow_control;
	ap.nap_dynamic_controller_model = true;
	ap.nap_max_admin_qsize = 4096;
	ap.nap_max_io_qsize = controller ? 0 : 1024;
	ap.nap_tcp.maxh2cdata = 4096;
	ap.nap_tcp.maxr2t = 1;
	na = nvmf_allocate_association(NVMF_TRTYPE_TCP, controller, &ap);
	CHECK(na != NULL);
}

/* Collect what the library wrote and check that it left the socket open. */
static void
finish(void)
{
	ssize_t n;

	CHECK(fcntl(sv[0], F_GETFD) != -1);
	nvmf_free_association(na);
	check_clean();

	(void) close(sv[0]);
	reply_len = 0;
	while ((n = read(sv[1], reply + reply_len,
	    sizeof (reply) - reply_len)) > 0)
		reply_len += n;
	(void) close(sv[1]);
}

/* The last PDU the library wrote must be a termination request. */
static void
check_term_req(uint8_t type, uint16_t fes)
{
	nvmf_tcp_term_req_hdr_t hdr;
	size_t off = 0, last = 0;
	nvmf_tcp_common_pdu_hdr_t ch;

	while (off + sizeof (ch) <= reply_len) {
		(void) memcpy(&ch, reply + off, sizeof (ch));
		last = off;
		CHECK(ch.ntcph_plen >= sizeof (ch));
		off += ch.ntcph_plen;
	}
	CHECK(off == reply_len && reply_len >= sizeof (hdr));
	(void) memcpy(&hdr, reply + last, sizeof (hdr));
	CHECK(hdr.nttr_common.ntcph_pdu_type == type);
	CHECK(hdr.nttr_fes == fes);
}

static struct nvmf_qpair *
do_accept(struct nvmf_capsule **ccp, uint_t timeout_ms)
{
	nvmf_fabric_connect_data_t data;
	nvmf_qpair_params_t qparams;

	(void) memset(&qparams, 0, sizeof (qparams));
	qparams.nqp_tcp.fd = sv[0];
	qparams.nqp_timeout_ms = timeout_ms;
	return (nvmf_accept(na, &qparams, ccp, &data));
}

static struct nvmf_qpair *
do_connect(uint16_t qid)
{
	static const uint8_t hostid[16] = { 1 };
	nvmf_qpair_params_t qparams;

	(void) memset(&qparams, 0, sizeof (qparams));
	qparams.nqp_admin = qid == 0;
	qparams.nqp_tcp.fd = sv[0];
	qparams.nqp_timeout_ms = 5000;
	return (nvmf_connect(na, &qparams, qid, 32, hostid,
	    NVMF_CNTLID_DYNAMIC, SUBNQN, HOSTNQN, 0));
}

/* A controller must refuse every malformed ICReq and say why. */
static void
bad_icreq(void)
{
	struct nvmf_capsule *cc;
	nvmf_tcp_ic_req_t ic;
	int i;

	for (i = 0; i < 7; i++) {
		uint16_t fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		uint16_t unsup =
		    NVMF_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER;

		ic = ic_req();
		switch (i) {
		case 0:
			ic.nfic_common.ntcph_pdu_type =
			    NVMF_TCP_PDU_TYPE_IC_RESP;
			break;
		case 1:
			ic.nfic_common.ntcph_flags = 1;
			break;
		case 2:
			ic.nfic_common.ntcph_hlen = 127;
			break;
		case 3:
			ic.nfic_common.ntcph_pdo = 4;
			break;
		case 4:
			ic.nfic_common.ntcph_plen = 0xffffffff;
			break;
		case 5:
			ic.nfic_pfv = 1;
			fes = unsup;
			break;
		case 6:
			ic.nfic_hpda = 32;
			break;
		}
		put(&ic, sizeof (ic));
		start("bad ICReq", true, false);
		CHECK(do_accept(&cc, 0) == NULL);
		CHECK(nvmf_association_error(na) != NULL);
		finish();
		check_term_req(NVMF_TCP_PDU_TYPE_C2H_TERM_REQ, fes);
	}
}

/* A length nobody could mean is refused from the eight header bytes alone. */
static void
huge_plen(void)
{
	struct nvmf_capsule *cc;
	nvmf_tcp_ic_req_t ic = ic_req();
	nvmf_tcp_common_pdu_hdr_t ch;

	put(&ic, sizeof (ic));
	ch = common(NVMF_TCP_PDU_TYPE_CAPSULE_CMD, 0, sizeof (nvmf_tcp_cmd_t),
	    sizeof (nvmf_tcp_cmd_t), 0xffffffff);
	put(&ch, sizeof (ch));
	start("plen 0xffffffff", true, false);
	CHECK(do_accept(&cc, 0) == NULL);
	finish();
	check_term_req(NVMF_TCP_PDU_TYPE_C2H_TERM_REQ,
	    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE);

	/* The largest in-capsule payload a controller takes, plus one. */
	put(&ic, sizeof (ic));
	ch = common(NVMF_TCP_PDU_TYPE_CAPSULE_CMD, 0, sizeof (nvmf_tcp_cmd_t),
	    sizeof (nvmf_tcp_cmd_t), sizeof (nvmf_tcp_cmd_t) + 8193);
	put(&ch, sizeof (ch));
	start("ICD over the limit", true, false);
	CHECK(do_accept(&cc, 0) == NULL);
	finish();
	check_term_req(NVMF_TCP_PDU_TYPE_C2H_TERM_REQ,
	    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE);
}

/* A PDU that stops short is freed, whether the peer hangs up or stalls. */
static void
truncated(void)
{
	struct nvmf_capsule *cc;
	nvmf_tcp_ic_req_t ic = ic_req();
	nvmf_tcp_cmd_t cmd = connect_capsule(1, SGL_ICD,
	    sizeof (nvmf_fabric_connect_data_t));
	struct timespec t0, t1;

	put(&ic, sizeof (ic));
	put(&cmd, sizeof (cmd));
	put_zero(100);
	start("truncated PDU", true, false);
	CHECK(do_accept(&cc, 0) == NULL);
	finish();

	put(&ic, sizeof (ic));
	put(&cmd, sizeof (cmd) - 10);
	start("stalled PDU", true, true);
	(void) clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(do_accept(&cc, 200) == NULL);
	(void) clock_gettime(CLOCK_MONOTONIC, &t1);
	CHECK(t1.tv_sec - t0.tv_sec < 5);
	finish();

	put(&ic, 60);
	start("stalled ICReq", true, true);
	CHECK(do_accept(&cc, 200) == NULL);
	finish();
}

/* PSDT is 01b for SGLs; a controller refuses the other three values. */
static void
psdt(void)
{
	struct nvmf_capsule *cc;
	struct nvmf_qpair *qp;
	nvmf_tcp_ic_req_t ic = ic_req();
	nvmf_fabric_connect_data_t data = connect_data();
	nvmf_tcp_cmd_t cmd;
	uint8_t v;

	for (v = 0; v < 4; v++) {
		cmd = connect_capsule(v, SGL_ICD, sizeof (data));
		put(&ic, sizeof (ic));
		put(&cmd, sizeof (cmd));
		put(&data, sizeof (data));
		start("PSDT", true, false);
		qp = do_accept(&cc, 0);
		CHECK((qp != NULL) == (v == 1));
		if (qp != NULL) {
			CHECK(nvmf_finish_accept(cc, 1) == 0);
			nvmf_free_capsule(cc);
			nvmf_free_qpair(qp);
		}
		finish();
	}
}

/*
 * A controller that asked for Connect data with an R2T takes neither another
 * command nor data outside the range it asked for.
 */
static void
r2t_wait(void)
{
	struct nvmf_capsule *cc;
	struct nvmf_qpair *qp;
	nvmf_tcp_ic_req_t ic = ic_req();
	nvmf_fabric_connect_data_t data = connect_data();
	nvmf_tcp_cmd_t cmd = connect_capsule(1, SGL_CMD_BUFFER, 0);
	nvmf_tcp_h2c_data_hdr_t h;

	put(&ic, sizeof (ic));
	put(&cmd, sizeof (cmd));
	put(&cmd, sizeof (cmd));
	start("command while H2C data is pending", true, false);
	CHECK(do_accept(&cc, 0) == NULL);
	finish();
	check_term_req(NVMF_TCP_PDU_TYPE_C2H_TERM_REQ,
	    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR);

	put(&ic, sizeof (ic));
	put(&cmd, sizeof (cmd));
	h = h2c(0, 0xffffff00, 0);
	put(&h, sizeof (h));
	start("H2C data past the buffer", true, false);
	CHECK(do_accept(&cc, 0) == NULL);
	finish();
	check_term_req(NVMF_TCP_PDU_TYPE_C2H_TERM_REQ,
	    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE);

	put(&ic, sizeof (ic));
	put(&cmd, sizeof (cmd));
	h = h2c(0, sizeof (data), NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU);
	put(&h, sizeof (h));
	put(&data, sizeof (data));
	start("Connect data by R2T", true, false);
	qp = do_accept(&cc, 0);
	CHECK(qp != NULL);
	nvmf_free_capsule(cc);
	nvmf_free_qpair(qp);
	finish();
}

/* A host refuses a CPDA above 31 before it computes any padding. */
static void
cpda(void)
{
	nvmf_tcp_ic_resp_t ic = ic_resp(63);

	put(&ic, sizeof (ic));
	start("CPDA 63", false, false);
	CHECK(do_connect(0) == NULL);
	CHECK(nvmf_association_error(na) != NULL);
	finish();
	check_term_req(NVMF_TCP_PDU_TYPE_H2C_TERM_REQ,
	    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD);
}

/* A PDA of 12 bytes is legal and is not a power of two. */
static void
padding(void)
{
	struct nvmf_qpair *qp;
	nvmf_tcp_ic_resp_t ic = ic_resp(2);
	nvmf_tcp_rsp_t rsp = response(0, NO_SQHD);
	nvmf_tcp_cmd_t cmd;

	put(&ic, sizeof (ic));
	put(&rsp, sizeof (rsp));
	start("PDA of 12", false, false);
	qp = do_connect(0);
	CHECK(qp != NULL);
	nvmf_free_qpair(qp);
	finish();

	CHECK(reply_len == sizeof (nvmf_tcp_ic_req_t) + sizeof (cmd) +
	    sizeof (nvmf_fabric_connect_data_t));
	(void) memcpy(&cmd, reply + sizeof (nvmf_tcp_ic_req_t), sizeof (cmd));
	CHECK(cmd.ntc_common.ntcph_pdo == 72);
	CHECK(cmd.ntc_common.ntcph_pdo % 12 == 0);
	CHECK(((uint8_t *)cmd.ntc_ccsqe)[1] >> 6 == 1);
}

/* An R2T whose offset and length wrap past the end of the Connect data. */
static void
wrapped_r2t(void)
{
	nvme_identify_ctrl_t cdata;
	nvmf_tcp_ic_resp_t ic = ic_resp(0);
	nvmf_tcp_r2t_hdr_t h;
	uint32_t ioccsz = 4;

	put(&ic, sizeof (ic));
	h = r2t(0, 100);
	put(&h, sizeof (h));
	h = r2t(100, 0xffffffa0);
	put(&h, sizeof (h));
	start("wrapped R2T", false, false);
	(void) memset(&cdata, 0, sizeof (cdata));
	(void) memcpy(&cdata.id_nvmof[0], &ioccsz, sizeof (ioccsz));
	nvmf_update_association(na, &cdata);
	CHECK(do_connect(1) == NULL);
	finish();
	check_term_req(NVMF_TCP_PDU_TYPE_H2C_TERM_REQ,
	    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE);
}

/* C2H data whose offset and length wrap, or just overrun the buffer. */
static void
wrapped_c2h(void)
{
	nvme_identify_ctrl_t cdata;
	struct nvmf_qpair *qp;
	nvmf_tcp_ic_resp_t ic = ic_resp(0);
	nvmf_tcp_rsp_t rsp = response(0, NO_SQHD);
	nvmf_tcp_c2h_data_hdr_t h;
	uint32_t second[] = { 0xffffffffU - 3904 + 1, 200, 0 };
	int i;

	for (i = 0; i < 3; i++) {
		put(&ic, sizeof (ic));
		put(&rsp, sizeof (rsp));
		h = c2h(0, 0, 4000, 0);
		put(&h, sizeof (h));
		put_zero(4000);
		h = c2h(0, 4000, second[i], NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU);
		if (second[i] == 0)
			h.ntc2c_common.ntcph_pdo = 0;
		put(&h, sizeof (h));
		start("wrapped C2H data", false, false);
		qp = do_connect(0);
		CHECK(qp != NULL);
		CHECK(nvmf_host_identify_controller(qp, &cdata) != 0);
		nvmf_free_qpair(qp);
		finish();
		check_term_req(NVMF_TCP_PDU_TYPE_H2C_TERM_REQ,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE);
	}

	/* The same exchange with the data in range must still work. */
	put(&ic, sizeof (ic));
	put(&rsp, sizeof (rsp));
	h = c2h(0, 0, 4000, 0);
	put(&h, sizeof (h));
	put_zero(4000);
	h = c2h(0, 4000, 96, NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU);
	put(&h, sizeof (h));
	put_zero(96);
	put(&rsp, sizeof (rsp));
	start("C2H data", false, false);
	qp = do_connect(0);
	CHECK(qp != NULL);
	CHECK(nvmf_host_identify_controller(qp, &cdata) == 0);
	nvmf_free_qpair(qp);
	finish();
}

/* A target cannot make a host queue responses without limit. */
static void
response_flood(void)
{
	nvme_identify_ctrl_t cdata;
	struct nvmf_qpair *qp;
	nvmf_tcp_ic_resp_t ic = ic_resp(0);
	nvmf_tcp_rsp_t rsp = response(0, 0);
	int i;

	put(&ic, sizeof (ic));
	put(&rsp, sizeof (rsp));
	rsp = response(7, 0);
	for (i = 0; i < 200; i++)
		put(&rsp, sizeof (rsp));
	flow_control = true;
	start("response flood", false, false);
	flow_control = false;
	qp = do_connect(0);
	CHECK(qp != NULL);
	CHECK(nvmf_host_identify_controller(qp, &cdata) == EPROTO);
	nvmf_free_qpair(qp);
	finish();
}

int
main(void)
{
	(void) signal(SIGPIPE, SIG_IGN);

	bad_icreq();
	huge_plen();
	truncated();
	psdt();
	r2t_wait();
	cpda();
	padding();
	wrapped_r2t();
	wrapped_c2h();
	response_flood();
	(void) printf("pdu_validate: ok\n");
	return (0);
}
