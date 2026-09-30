#!/usr/bin/env python3
"""Run the RDMA transport's queue and data path against a model of the
device and the host (rdma_datapath.c), then check that the run fails once
each guard it depends on is taken out."""

from concurrent.futures import ThreadPoolExecutor
import sys

from rdma_host import HostFailure, mutant_fails, run

TEST = "rdma_datapath.c"

MUTANTS = (
    ("a transfer outside its SGL is refused", "plan", {
        "nvmf_rdma_xfer.c": (
            ("!nvmf_rdma_range_ok(off, (uint32_t)len, c->nc_sgl.nsl_len))\n"
             "\t\treturn (EFBIG);\n\tif (mem->", "B_FALSE)\n"
             "\t\treturn (EFBIG);\n\tif (mem->"),
            ("!nvmf_rdma_range_ok(off, (uint32_t)io->io_len, "
             "c->nc_sgl.nsl_len))", "B_FALSE)"),
        )}),
    ("SEND_WITH_INV only for a key the host gave up", "plan", {
        "nvmf_rdma.c": (
            ("c->nc_sgl.nsl_keyed && c->nc_sgl.nsl_invalidate &&",
             "c->nc_sgl.nsl_keyed &&"),),
        "nvmf_rdma_xfer.c": (
            ("if (ret == 0 && c->nc_sgl.nsl_invalidate)", "if (ret == 0)"),)}),
    ("an inline response names the CQE by its VA", "plan", {
        "nvmf_rdma.c": (
            ("\t\tc->nc_ssge.addr = (uint64_t)(uintptr_t)c->nc_cqe;\n", ""),)}),
    ("a command freed unanswered waits for its response", "plan", {
        "nvmf_rdma.c": (
            ("c->nc_wrs != 0 || c->nc_state != NR_C_DONE)",
             "c->nc_wrs != 0 || c->nc_state == NR_C_FREE)"),)}),
    ("a deferred response keeps its command", "plan", {
        "nvmf_rdma.c": (
            ("c->nc_state == NR_C_ACTIVE && !nc->nc_deferred)",
             "c->nc_state == NR_C_ACTIVE)"),)}),
    ("admin data is out before its response", "plan", {
        "nvmf_rdma_xfer.c": (
            ("\t\t\twhile (!w.nw_done)\n\t\t\t\tcv_wait(&w.nw_cv, &w.nw_lock);\n"
             "\t\t\tstatus = w.nw_status;",
             "\t\t\tstatus = NVME_CQE_SC_GEN_SUCCESS;"),)}),
    ("other commands freed unanswered give their contexts back", "plan", {
        "nvmf_rdma.c": (
            ("\tif (c->nc_state == NR_C_ACTIVE && !nc->nc_deferred)\n"
             "\t\tc->nc_state = NR_C_DONE;\n", ""),)}),
    ("a CID in use is fatal", "teardown", {
        "nvmf_rdma.c": (
            ("\tif (nr_cid_find_locked(q, c->nc_cid) != NULL) {",
             "\tif (B_FALSE) {"),)}),
    ("a held RECV goes back before the response", "credit", {
        "nvmf_rdma.c": (
            ("\t\treturn (EALREADY);\n\tnr_cmd_unhold_locked(c);\n",
             "\t\treturn (EALREADY);\n"),)}),
    ("the send queue has room for every transfer", "credit", {
        "nvmf_rdma_subr.c": (
            ("sz->nrs_sq = (uint32_t)(xfers * wrs + base_sq);",
             "sz->nrs_sq = (uint32_t)(xfers + base_sq);"),)}),
    ("a drained queue fails its transfers before the destroy", "teardown", {
        "nvmf_rdma.c": (
            ("\tif (drained)\n\t\tnr_xfer_fail_all(q);\n",
             "\t(void) drained;\n"),)}),
    ("an undrained queue fails its transfers only after it", "teardown", {
        "nvmf_rdma.c": (
            ("\tif (drained)\n\t\tnr_xfer_fail_all(q);\n",
             "\t(void) drained;\n\tnr_xfer_fail_all(q);\n"),)}),
    ("a completion only starts the teardown", "teardown", {
        "nvmf_rdma_xfer.c": (
            ("\t\tif (wc->status != RDK_WC_WR_FLUSH_ERR)\n"
             "\t\t\tnr_queue_fail_locked(q, EIO);\n\t\tnr_xreq_fail_locked",
             "\t\tif (wc->status != RDK_WC_WR_FLUSH_ERR) {\n"
             "\t\t\tnr_queue_fail_locked(q, EIO);\n"
             "\t\t\tmutex_exit(&q->nq_lock);\n"
             "\t\t\tnr_queue_teardown(q);\n"
             "\t\t\tmutex_enter(&q->nq_lock);\n\t\t}\n"
             "\t\tnr_xreq_fail_locked"),)}),
)


def main():
    try:
        print(run(TEST), end="")
    except HostFailure as error:
        print(error)
        return 1
    status = 0
    # A mutant that loses a response waits out the host's deadline.
    with ThreadPoolExecutor(max_workers=6) as pool:
        failed = list(pool.map(
            lambda m: mutant_fails(TEST, m[2], (m[1],)), MUTANTS))
    for (what, _, _), fails in zip(MUTANTS, failed):
        if fails:
            print(f"PASS: {what} (a build without it fails)")
        else:
            print(f"FAIL: a build without the guard '{what}' passed")
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
