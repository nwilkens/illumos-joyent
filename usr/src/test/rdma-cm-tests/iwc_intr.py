#!/usr/bin/env python3
"""Check what iwcxgbe does in interrupt context.

t4nex calls the provider's CPL and CQ callbacks from its interrupt
handlers.  Nothing they reach may block, directly or through a t4nex
operation, and every mutex they take must be initialized at the interrupt
priority.  Elsewhere in the provider, no call made while an interrupt
priority lock is held may block.  The call graph and blocking set are the
cxgbe-tests ones, with each t4nex operation resolved through the ops table.
"""

import re
import sys

from cm_test import CXGBE_TESTS, IWC, T4NEX

sys.path.insert(0, str(CXGBE_TESTS))
import intr_locks as il  # noqa: E402
from c_src import COMMON, calls, functions, strip  # noqa: E402

MUTEX_INIT = re.compile(r"mutex_init\s*\(\s*&[^,]*?(\w+)\s*,")
ENTER = re.compile(r"\bmutex_enter\s*\(([^;]*)\)\s*;")


def client_entries(texts):
    """The callbacks t4nex runs in interrupt context."""
    for text in texts:
        m = re.search(r"t4_rdma_client_t \w+ = \{(.*?)\};", text, re.DOTALL)
        if m is not None:
            cb = dict(re.findall(r"\.(trcl_\w+) = (\w+)", m.group(1)))
            return [cb["trcl_cpl"], cb["trcl_cq"]]
    raise ValueError("no t4_rdma_client_t in iwcxgbe")


def graph(iwc_texts, t4_texts):
    bodies = {}
    for text in t4_texts + iwc_texts:
        bodies.update(functions(text))
    peer = (T4NEX / "t4_rdma_peer.c").read_text(encoding="utf-8")
    init = re.search(r"const t4_rdma_ops_t t4_rdma_ops = \{(.*?)\};", peer,
                     re.DOTALL).group(1)
    for member, impl in re.findall(r"\.(tro_\w+) = (\w+)", init):
        bodies[member] = f"{{\n\t{impl}();\n}}"
    return bodies


def intr_reach(bodies, entries):
    guarded = {n for n, b in bodies.items() if "servicing_interrupt" in b}
    seen, todo = set(), [e for e in entries if e in bodies]
    while todo:
        name = todo.pop()
        if name in seen or name in guarded:
            continue
        seen.add(name)
        todo.extend(c for c in calls(bodies[name]) if c in bodies)
    return seen


def lock_violations(bodies, reached, all_locks, watched):
    bad = []
    for name in sorted(reached):
        for expr in ENTER.findall(bodies[name]):
            lock = il.lock_of(expr, all_locks)
            if lock is not None and lock not in watched:
                bad.append(f"{name}() takes {lock} in interrupt context; "
                           f"it is not an interrupt priority lock")
    return bad


def check(iwc_texts, t4_texts):
    texts = t4_texts + iwc_texts
    watched = il.watched_locks(texts)
    all_locks = set()
    for text in texts:
        all_locks.update(MUTEX_INIT.findall(strip(text)))
    bodies = graph(iwc_texts, t4_texts)
    entries = client_entries(iwc_texts)
    reach = il.blocking(bodies, il.direct_blockers(bodies))
    cvonly = il.cv_only(bodies, watched)
    bad = il.intr_violations(bodies, entries)
    bad += lock_violations(bodies, intr_reach(bodies, entries), all_locks,
                           watched)
    for text in iwc_texts:
        for func, body in sorted(functions(text).items()):
            bad.extend(il.violations("iwcxgbe", func, body, reach, watched,
                                     cvonly))
    return entries, watched & all_locks, bad


def main():
    iwc_paths = sorted(IWC.glob("*.c"))
    iwc_texts = [p.read_text(encoding="utf-8") for p in iwc_paths]
    t4_texts = [p.read_text(encoding="utf-8", errors="replace") for p in
                sorted(T4NEX.glob("*.c")) + sorted(COMMON.glob("*.c"))]
    entries, _, bad = check(iwc_texts, t4_texts)
    if bad:
        print("\n".join(bad))
        return 1

    # The check must see a sleeping allocation in the CPL callback, a
    # non-interrupt lock in the CQ callback, and a wait the CQ callback
    # reaches through another function.
    names = [p.name for p in iwc_paths]
    anchors = (
        ("iwc_cm.c", "\tq = kmem_alloc(sizeof (*q), KM_NOSLEEP);\n",
         "\tq = kmem_alloc(sizeof (*q), KM_SLEEP);\n", "blocks"),
        ("iwc_cq.c", "\tmutex_enter(&iwc->iwc_obj_lock);\n\tfor (uint_t i",
         "\tmutex_enter(&iwc->iwc_ep_lock);\n\tmutex_exit("
         "&iwc->iwc_ep_lock);\n\tmutex_enter(&iwc->iwc_obj_lock);\n"
         "\tfor (uint_t i", "iwc_ep_lock"),
        ("iwc_cq.c", "id - iwc->iwc_qid_start]) != NULL)\n"
         "\t\t\tiwc_cq_schedule(iwc, cq);\n",
         "id - iwc->iwc_qid_start]) != NULL)\n"
         "\t\t\tiwc_cq_wait_idle(cq);\n", "iwc_cq_wait_idle() blocks"),
    )
    for name, old, new, want in anchors:
        i = names.index(name)
        if iwc_texts[i].count(old) != 1:
            print(f"mutation anchor not found: {old!r}")
            return 1
        mutated = list(iwc_texts)
        mutated[i] = iwc_texts[i].replace(old, new)
        _, _, found = check(mutated, t4_texts)
        if not any(want in f for f in found):
            print(f"missed a mutation: {new!r}: {found}")
            return 1
    print(f"PASS: {' and '.join(entries)} never block and take only "
          f"interrupt priority locks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
