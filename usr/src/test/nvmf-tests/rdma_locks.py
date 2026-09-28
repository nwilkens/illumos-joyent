#!/usr/bin/env python3
"""Static checks of the RDMA transport's locking and callback rules:

- with a queue's nq_lock held, nothing calls into nvmft or its callbacks,
  drops a queue reference, waits, or destroys an rdmak object;
- every post to the QP happens with nq_lock held, and every call to a
  *_locked function too;
- nothing a completion or QP event handler can reach drains, destroys or
  frees what the callback serves, or waits for a teardown; those run in the
  teardown the handler starts.

Then check that each rule catches a build that breaks it."""

import re
import sys

from nvmf_test import NVMF

FILES = ("nvmf_rdma.c", "nvmf_rdma_xfer.c", "nvmf_rdma_cm.c",
         "nvmf_rdma_pool.c")

LOCK = re.compile(r"mutex_(enter|exit)\(&(?:\w+(?:->|\.))+nq_lock\)")
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
JUMP = re.compile(r"^\s*(return\b|goto\b|break;|continue;)")
# A *_locked function that works on a queue expects its nq_lock.
QUEUE = re.compile(r"nq_lock|nr_queue_t|nr_cmd_t|nr_xreq_t")

UNDER_LOCK = {
    "nvmf_capsule_received", "nvmf_qpair_error", "nvmf_complete_io_request",
    "io_cb", "send_cb", "nr_xreq_callbacks", "nr_queue_rele",
    "nr_xreq_send_done", "rdk_drain_qp", "rdk_drain_sq", "rdk_drain_rq",
    "rdk_destroy_qp", "rdk_free_cq", "rdk_dereg_mr", "rdk_teardown_wait",
    "rdk_cm_destroy_id", "rdk_cm_accept", "untimeout", "cv_wait",
    "nr_queue_fail", "nr_buf_free",
}
POSTS = {"rdk_post_send", "rdk_post_recv", "rdk_rw_post"}
IN_CALLBACK = {
    "rdk_drain_qp", "rdk_drain_sq", "rdk_drain_rq", "rdk_destroy_qp",
    "rdk_destroy_cq", "rdk_free_cq", "rdk_dereg_mr", "rdk_teardown_wait",
    "rdk_teardown_free", "rdk_cm_destroy_id", "rdk_dma_buf_free",
    "nr_queue_teardown", "nr_xfer_fini", "nr_pool_fini",
    "nr_queue_destroy_unadopted",
}


def functions(text):
    """name -> body of each function definition in illumos style."""
    out = {}
    for m in re.finditer(r"^([A-Za-z_]\w*)\(([^;{]*?)\)\n\{\n(.*?)^\}\n",
                         text, re.MULTILINE | re.DOTALL):
        out[m.group(1)] = m.group(3)
    return out


def strip(line):
    line = re.sub(r"/\*.*?\*/", "", line)
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)


def lock_walk(name, body):
    """Yield (held, line) for each line of a body.  A block that ends in a
    jump leaves the lock as it found it."""
    held = name.endswith("_locked")
    stack = []
    lines = [strip(l) for l in body.split("\n")]
    for i, line in enumerate(lines):
        yield held, line
        m = LOCK.search(line)
        if m:
            held = m.group(1) == "enter"
        for ch in line:
            if ch == "{":
                stack.append(held)
            elif ch == "}" and stack:
                entry = stack.pop()
                prev = next((p for p in reversed(lines[:i])
                             if p.strip() not in ("", "{", "}")), "")
                if JUMP.match(prev):
                    held = entry


def load(replace=None):
    replace = replace or {}
    funcs = {}
    for name in FILES:
        text = (NVMF / name).read_text(encoding="utf-8")
        for old, new in replace.get(name, ()):
            assert old in text, f"{name}: {old!r}"
            text = text.replace(old, new)
        funcs.update(functions(text))
        funcs["__callbacks__" + name] = text
    return funcs


def roots(funcs):
    """The rdmak callbacks: done() functions and the QP event handler."""
    out = set()
    for key, text in funcs.items():
        if not key.startswith("__callbacks__"):
            continue
        out.update(re.findall(r"\.done = (\w+);", text))
        out.update(re.findall(r"\.event_handler = (\w+);", text))
    return out


def check(replace=None):
    funcs = load(replace)
    errors = []
    for name, body in funcs.items():
        if name.startswith("__"):
            continue
        for held, line in lock_walk(name, body):
            for call in CALL.findall(line):
                if held and call in UNDER_LOCK:
                    errors.append(f"{name}: {call}() with nq_lock held")
                if not held and call in POSTS:
                    errors.append(f"{name}: {call}() without nq_lock")
                if not held and call.endswith("_locked") and \
                        call in funcs and QUEUE.search(funcs[call]):
                    errors.append(f"{name}: {call}() without nq_lock")
    cbs = roots(funcs)
    if not cbs >= {"nr_recv_done", "nr_send_done", "nr_rw_done",
                   "nr_qp_event"}:
        errors.append(f"callbacks not found: {sorted(cbs)}")
    for root in sorted(cbs):
        seen, todo = set(), [(root, (root,))]
        while todo:
            fn, path = todo.pop()
            if fn in seen or fn not in funcs:
                continue
            seen.add(fn)
            for call in set(CALL.findall(strip(funcs[fn]))):
                if call in IN_CALLBACK:
                    errors.append(f"{' -> '.join(path + (call,))}: "
                                  "from a callback")
                elif call in funcs and call != "rdk_teardown_start":
                    todo.append((call, path + (call,)))
    return errors


MUTANTS = (
    ("nvmft is called only with nq_lock dropped", {
        "nvmf_rdma.c": (("\t\tc = nr_cmd_start_locked(q, r, wc->byte_len);\n",
                         "\t\tc = nr_cmd_start_locked(q, r, wc->byte_len);\n"
                         "\t\tif (c != NULL)\n\t\t\tnvmf_capsule_received("
                         "&q->nq_nq, &c->nc_nc);\n"),)}),
    ("posts happen with nq_lock held", {
        "nvmf_rdma.c": (("\tconnected = !connected && q->nq_connected;\n"
                         "\tmutex_exit(&q->nq_lock);\n",
                         "\tconnected = !connected && q->nq_connected;\n"
                         "\tmutex_exit(&q->nq_lock);\n"
                         "\t(void) rdk_post_send(q->nq_qp, NULL, NULL);\n"),)}),
    ("a *_locked function is called with nq_lock held", {
        "nvmf_rdma.c": (("\tmutex_enter(&q->nq_lock);\n\tVERIFY(c->nc_capsule);",
                         "\tVERIFY(c->nc_capsule);"),
                        ("\tnr_cmd_rele_locked(c);\n\tmutex_exit(&q->nq_lock);"
                         "\n\tnr_queue_rele(q);\n}",
                         "\tnr_cmd_rele_locked(c);\n\tnr_queue_rele(q);\n}"))}),
    ("a callback does not destroy what it serves", {
        "nvmf_rdma_xfer.c": (("\tnr_xreq_run_locked(q, &done);\n"
                              "\tmutex_exit(&q->nq_lock);\n",
                              "\tnr_xreq_run_locked(q, &done);\n"
                              "\tmutex_exit(&q->nq_lock);\n"
                              "\tif (list_is_empty(&q->nq_free_xfers))\n"
                              "\t\tnr_xfer_fini(q);\n"),)}),
)


def main():
    errors = check()
    for error in errors:
        print(f"FAIL: {error}")
    if errors:
        return 1
    print("PASS: nq_lock and callback rules hold")
    status = 0
    for what, replace in MUTANTS:
        if check(replace):
            print(f"PASS: {what} (a build that breaks it is caught)")
        else:
            print(f"FAIL: a build that breaks '{what}' passed")
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
