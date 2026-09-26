#!/usr/bin/env python3
"""Fail if the offload core blocks while holding an interrupt-priority lock,
or blocks in interrupt context.

The watched locks are found, not listed: every mutex t4nex initializes with
DDI_INTR_PRI() and the queue locks behind EQ_LOCK(), IQ_LOCK(), FL_LOCK() and
TXQ_LOCK().  A call graph over t4nex and the common code finds every function
that can block: a sleeping allocation, a mailbox command, a DMA or interrupt
setup call, a taskq wait, a device tree change, a delay or a condition
variable wait.  Each offload source file is walked, tracking the locks by
brace depth, and no call made while a watched lock is held may reach that
set; a function that only waits on the lock its caller holds and asserts is
the exception.  Separately, nothing reachable from an interrupt handler may
block, except behind a servicing_interrupt() early return.
"""

import argparse
from pathlib import Path
import re
import sys

from c_src import COMMON, KEYWORDS, OFLD_FILES, T4NEX, calls, functions, strip


# DMA allocation blocks only when asked to sleep; SLEEP_ARG covers that.
ROOTS = ("t4_wr_mbox", "t4_wr_mbox_meat", "t4_wr_mbox_meat_timeout",
         "t4_wr_mbox_ns", "t4_query_params", "t4_set_params", "delay",
         "ddi_intr_alloc", "ddi_intr_free", "ddi_intr_add_handler",
         "ddi_intr_remove_handler", "ddi_taskq_wait", "ddi_taskq_destroy",
         "ddi_taskq_create", "ndi_devi_online", "ndi_devi_offline",
         "ndi_devi_alloc", "kstat_create", "kstat_delete", "ddi_copyin",
         "ddi_copyout")
SLEEP_ARG = re.compile(r"\b(KM_SLEEP|DDI_DMA_SLEEP|DDI_SLEEP)\b")
MACRO_LOCKS = {"EQ_LOCK": "tse_lock", "EQ_UNLOCK": "tse_lock",
               "IQ_LOCK": "tsi_lock", "IQ_UNLOCK": "tsi_lock",
               "FL_LOCK": "tse_lock", "FL_UNLOCK": "tse_lock",
               "TXQ_LOCK": "tse_lock", "TXQ_UNLOCK": "tse_lock"}
INTR_INIT = re.compile(r"mutex_init\s*\(\s*&[^,]*?(\w+)\s*,[^;]*"
                       r"DDI_INTR_PRI", re.DOTALL)


def watched_locks(texts):
    found = set()
    for text in texts:
        found.update(INTR_INIT.findall(strip(text)))
    return found | set(MACRO_LOCKS.values())


CV_WAITS = ("cv_wait", "cv_timedwait", "cv_reltimedwait", "cv_wait_sig")
CV_RE = re.compile(r"\bcv_(?:wait|timedwait|reltimedwait|wait_sig)\s*\("
                   r"[^,]*,\s*&?([^,)]*)")
HELD_RE = re.compile(r"ASSERT\(MUTEX_HELD\(\s*&?([^)]*?)\)\)")


def direct_blockers(bodies):
    return {n for n, b in bodies.items()
            if SLEEP_ARG.search(b) or calls(b) & set(ROOTS + CV_WAITS)}


def cv_only(bodies, watched):
    """Functions whose only wait is on a lock they assert their caller
    holds, mapped to that lock."""
    found = {}
    for name, body in bodies.items():
        if SLEEP_ARG.search(body) or calls(body) & set(ROOTS):
            continue
        waits = {lock_of(m, watched) for m in CV_RE.findall(body)}
        held = {lock_of(m, watched) for m in HELD_RE.findall(body)}
        if len(waits) == 1 and waits <= held and None not in waits:
            found[name] = waits.pop()
    return found


def blocking(bodies, seeds):
    """Every function that performs, or calls one that performs, a
    blocking operation."""
    reach = set(ROOTS) | set(CV_WAITS) | set(seeds)
    graph = {n: calls(b) for n, b in bodies.items()}
    changed = True
    while changed:
        changed = False
        for name, callees in graph.items():
            if name not in reach and callees & reach:
                reach.add(name)
                changed = True
    return reach


INTR_ENTRIES = ("t4_intr_ofld", "t4_intr_all", "t4_intr_err", "t4_intr_fwq",
                "t4_intr_port_queue", "t4_ot_cpl")


def intr_violations(bodies, entries):
    """Blocking calls reachable from interrupt handlers.  A function with a
    servicing_interrupt() check is not entered."""
    guarded = {n for n, b in bodies.items() if "servicing_interrupt" in b}
    direct = direct_blockers(bodies)
    seen, todo, bad = set(), [e for e in entries if e in bodies], []
    while todo:
        name = todo.pop()
        if name in seen or name in guarded:
            continue
        seen.add(name)
        if name in direct:
            bad.append(f"{name}() blocks and is reachable from an "
                       f"interrupt handler")
        todo.extend(c for c in calls(bodies[name]) if c in bodies)
    return bad


TOKEN_RE = re.compile(r"[{};]|\b(?:mutex_enter|mutex_exit|EQ_LOCK|EQ_UNLOCK|"
                      r"IQ_LOCK|IQ_UNLOCK|FL_LOCK|FL_UNLOCK|TXQ_LOCK|"
                      r"TXQ_UNLOCK)\s*\([^;]*\)\s*;|"
                      r"\b(?:return|goto)\b|\b\w+\s*\(")
ASSERT_RE = re.compile(r"ASSERT\(MUTEX_HELD\(\s*&?([^)]*?)\)\)|"
                       r"(EQ|IQ|FL|TXQ)_LOCK_ASSERT_OWNED\(")


def lock_of(expr, watched):
    expr = expr.replace(" ", "")
    for lock in watched:
        if re.search(rf"(->|\.|^){lock}$", expr):
            return lock
    return None


def arguments(body, start):
    depth = 0
    for i in range(start, len(body)):
        if body[i] == "(":
            depth += 1
        elif body[i] == ")":
            depth -= 1
            if depth == 0:
                return body[start + 1:i]
    return body[start + 1:]


def held_calls(body, watched):
    """Yield (held locks, callee, arguments) for each call.

    A block that ends in return or goto leaves the lock state as it found
    it, which covers the usual unlock-and-bail path.
    """
    held = []
    for m in ASSERT_RE.finditer(body):
        if m.group(1) is not None:
            name = lock_of(m.group(1), watched)
        else:
            name = MACRO_LOCKS[m.group(2) + "_LOCK"]
        if name is not None and name not in held:
            held.append(name)
    saved, last_exit, pending = [], [False], False
    for match in TOKEN_RE.finditer(body):
        tok = match.group()
        if tok == "{":
            saved.append(list(held))
            last_exit.append(False)
            pending = False
            continue
        if tok == "}":
            state = saved.pop() if saved else []
            if len(last_exit) > 1 and last_exit.pop():
                held = state
            pending = False
            continue
        if tok in ("return", "goto"):
            pending = True
            continue
        if tok == ";":
            if pending:
                last_exit[-1] = True
                pending = False
            continue
        op = tok[:tok.index("(")].strip()
        if op in ("mutex_enter", "mutex_exit") or op in MACRO_LOCKS:
            if op in MACRO_LOCKS:
                name = MACRO_LOCKS[op]
                enter = not op.endswith("UNLOCK")
            else:
                name = lock_of(tok[tok.index("(") + 1:tok.rindex(")")],
                               watched)
                enter = op == "mutex_enter"
            if name is not None:
                if enter:
                    held.append(name)
                elif name in held:
                    held.remove(name)
            last_exit[-1] = False
            pending = False
            continue
        if op in KEYWORDS:
            continue
        if not pending:
            last_exit[-1] = False
        yield list(held), op, arguments(body, match.end() - 1)


def violations(file, func, body, reach, watched, cvonly):
    for held, callee, args in held_calls(body, watched):
        if not held:
            continue
        if callee in CV_WAITS:
            parts = args.split(",")
            mutex = lock_of(parts[1].strip() if len(parts) > 1 else "",
                            watched)
            others = [h for h in held if h != mutex]
            if others:
                yield f"{file}: {func}() waits on a cv holding {others}"
            continue
        if callee in cvonly and held == [cvonly[callee]]:
            continue
        if callee in reach or SLEEP_ARG.search(args):
            yield f"{file}: {func}() calls {callee}() holding {held}"


def check(sources, graph_sources):
    texts = [p.read_text(encoding="utf-8", errors="replace")
             for p in graph_sources]
    watched = watched_locks(texts)
    bodies = {}
    for text in texts:
        bodies.update(functions(text))
    cvonly = cv_only(bodies, watched)
    reach = blocking(bodies, direct_blockers(bodies))
    bad = []
    for path in sources:
        for func, body in sorted(functions(path.read_text(
                encoding="utf-8")).items()):
            bad.extend(violations(path.name, func, body, reach, watched,
                                  cvonly))
    bad.extend(intr_violations(bodies, INTR_ENTRIES))
    return watched, reach, bad


def self_test():
    watched = {"of_lock", "td_lock", "tse_lock", "tsi_lock"}
    planted = ("static void\nh(void)\n{\n\t(void) t4_wr_mbox(sc);\n}\n"
               "static void\ng(void)\n{\n\tif (servicing_interrupt())\n"
               "\t\treturn;\n\tdelay(1);\n}\n"
               "static void\nf(t4_ofld_t *of, t4_sge_eq_t *eq)\n{\n"
               "\tmutex_enter(&of->of_lock);\n"
               "\tp = kmem_zalloc(n, KM_SLEEP);\n\th();\n\tg();\n"
               "\tmutex_exit(&of->of_lock);\n"
               "\tEQ_LOCK(eq);\n\tcv_wait(&cv, &of->td_lock);\n"
               "\tEQ_UNLOCK(eq);\n\th();\n}\n"
               "static void\nk(t4_ofld_t *of)\n{\n"
               "\tmutex_enter(&of->of_lock);\n\tif (x) {\n"
               "\t\tmutex_exit(&of->of_lock);\n\t\treturn;\n\t}\n"
               "\tmutex_exit(&of->of_lock);\n\th();\n}\n")
    planted += ("static void\nw(t4_ofld_t *of)\n{\n"
                "\tASSERT(MUTEX_HELD(&of->td_lock));\n"
                "\tcv_wait(&cv, &of->td_lock);\n}\n"
                "static void\nu(t4_ofld_t *of)\n{\n"
                "\tmutex_enter(&of->td_lock);\n\tw(of);\n"
                "\tmutex_exit(&of->td_lock);\n"
                "\tmutex_enter(&of->of_lock);\n\tw(of);\n"
                "\tmutex_exit(&of->of_lock);\n}\n"
                "uint_t\nt4_intr_ofld(caddr_t a, caddr_t b)\n{\n"
                "\tg();\n\tk(a);\n\treturn (0);\n}\n")
    bodies = functions(planted)
    cvonly = cv_only(bodies, watched)
    reach = blocking(bodies, direct_blockers(bodies))
    assert cvonly == {"w": "td_lock"}, cvonly
    assert {"h", "g", "w", "k"} <= reach, reach
    found = list(violations("x.c", "f", bodies["f"], reach, watched, cvonly))
    assert len(found) == 4, found
    assert "kmem_zalloc" in found[0] and "h()" in found[1], found
    assert "g()" in found[2], found
    assert "cv holding ['tse_lock']" in found[3], found
    assert list(violations("x.c", "k", bodies["k"], reach, watched,
                           cvonly)) == []
    found = list(violations("x.c", "u", bodies["u"], reach, watched, cvonly))
    assert found == ["x.c: u() calls w() holding ['of_lock']"], found
    found = intr_violations(bodies, ("t4_intr_ofld",))
    assert found == ["h() blocks and is reachable from an interrupt "
                     "handler"], found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, action="append",
                        help="check this file instead of the offload core")
    args = parser.parse_args()
    self_test()
    sources = args.source or [T4NEX / f for f in OFLD_FILES]
    graph = sorted(T4NEX.glob("*.c")) + sorted(COMMON.glob("*.c"))
    if args.source:
        graph += args.source
    watched, reach, bad = check(sources, graph)

    # Mutations of the real code must be caught: a sleeping L2T write under
    # the TID lock, and a completion wait in the CPL dispatch path.
    ops = (T4NEX / "t4_ofld_ops.c").read_text(encoding="utf-8")
    anchor = "e = t4_tid_owned(of, T4_TID_HW, a->trac_tid, gen);"
    assert ops.count(anchor) == 1
    mutated = ops.replace(anchor, anchor + "\n\t(void) t4_l2t_get(of, 0, "
                          "0, NULL, NULL);")
    texts = [p.read_text(encoding="utf-8", errors="replace") for p in graph]
    bodies = {}
    for text in texts:
        bodies.update(functions(text))
    cvonly = cv_only(bodies, watched)
    hits = list(violations("t4_ofld_ops.c", "t4_ofld_accept",
                           functions(mutated)["t4_ofld_accept"], reach,
                           watched, cvonly))
    assert any("t4_l2t_get" in h for h in hits), hits
    cpl = (T4NEX / "t4_ofld_cpl.c").read_text(encoding="utf-8")
    anchor = "\t\tt4_ofld_wr_rpl(of, cpl);\n"
    assert cpl.count(anchor) == 1
    bodies.update(functions(cpl.replace(anchor, anchor +
                                        "\t\t(void) t4_ofld_waiter_wait(of, 0);\n")))
    assert "t4_ofld_waiter_wait() blocks and is reachable from an " \
        "interrupt handler" in intr_violations(bodies, INTR_ENTRIES)
    expected = {"of_lock", "of_wlock", "td_lock", "l2_lock", "tse_lock",
                "tsi_lock", "ot_qlock"}
    assert expected <= watched, f"lost sight of {expected - watched}"
    lost = [n for n in ("t4_clip_cmd", "t4_ofld_queues_init",
                        "t4_ofld_params", "t4_alloc_iq") if n not in reach]
    assert not lost, f"call graph lost sight of {lost}"
    if bad:
        print("\n".join(bad))
        return 1
    print(f"PASS: no offload call blocks while holding "
          f"{', '.join(sorted(watched))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
