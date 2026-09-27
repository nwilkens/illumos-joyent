#!/usr/bin/env python3
"""Check the connection manager's locking.

1. No call out while a lock is held.  Provider operations and verbs call
   back into the CM or wait on the device, consumer handlers may call any
   rdk_cm operation, and sockets and ARP resolution block.  So no rdk_cm
   function may reach one of them while it holds a CM lock.  Two locks are
   meant to be held across a call out: rdk_cm_gid_lock serializes GID
   table passes, device commands included, and a QP's mod_lock serializes
   its modifies.
2. No lock order cycle.  An edge A -> B is recorded wherever B is taken,
   directly or by a callee, while A is held; a cycle is a possible
   deadlock.
3. The GSI completion handlers and the connection timer run in the
   provider's interrupt thread or a callout, so nothing they reach may
   block.
"""

import re
import sys

from cm_test import CXGBE_TESTS, RDMA

sys.path.insert(0, str(CXGBE_TESTS))
import intr_locks as il  # noqa: E402
from c_src import calls, functions, strip  # noqa: E402

FILES = ("rdk_cm.c", "rdk_cm_iw.c", "rdk_cm_addr.c", "rdk_cm_listen.c",
         "rdk_cm_gid.c", "rdk_cm_roce.c", "rdk_cm_roce_rx.c", "rdk_gsi.c",
         "rdk_verbs.c")
MUTEX_INIT = re.compile(r"mutex_init\s*\(\s*&[^,]*?(\w+)\s*,")
OUTSIDE = re.compile(r"^(iw_\w+|ct_\w+|rao_\w+|gto_\w+|handler|done|"
                     r"ksocket_\w+|ip2mac\w*|rdk_iw_cm_event|taskq_wait\w*|"
                     r"rdk_modify_qp|rdk_query_qp|rdk_create_qp|"
                     r"rdk_destroy_qp|rdk_create_ah|rdk_destroy_ah|"
                     r"rdk_post_send|rdk_post_recv|rdk_alloc_cq|rdk_free_cq|"
                     r"rdk_add_gid|rdk_del_gid|rdk_gsi_send|"
                     r"rdk_dma_buf_alloc|rdk_dma_buf_free|untimeout|"
                     r"modify_qp|destroy_qp|create_ah|destroy_ah)$")
HELD_ACROSS = {"rdk_cm_gid_lock", "mod_lock"}
NOBLOCK_ENTRIES = ("rdk_gsi_recv_done", "rdk_gsi_send_done",
                   "rdk_ibconn_timer")
BLOCKERS = {"rdk_modify_qp", "rdk_query_qp", "rdk_create_qp",
            "rdk_destroy_qp", "rdk_create_ah", "rdk_destroy_ah",
            "rdk_gsi_send", "untimeout", "ksocket_socket", "ip2mac",
            "taskq_wait", "taskq_destroy", "delay", "kmem_cache_destroy"}
TAKE = re.compile(r"\bmutex_enter\s*\(\s*&?([^)]*?)\)")


def reaching(bodies, pattern):
    reach = {n for n, b in bodies.items() if any(pattern.match(c) for c in
                                                 calls(b))}
    changed = True
    while changed:
        changed = False
        for name, body in bodies.items():
            if name not in reach and calls(body) & reach:
                reach.add(name)
                changed = True
    return reach


def acquired(bodies, locks):
    """The locks each function takes, itself or through its callees."""
    own = {n: {lock_of(e, locks) for e in TAKE.findall(b)} - {None}
           for n, b in bodies.items()}
    got = {n: set(s) for n, s in own.items()}
    changed = True
    while changed:
        changed = False
        for name, body in bodies.items():
            for c in calls(body) & got.keys():
                if not got[c] <= got[name]:
                    got[name] |= got[c]
                    changed = True
    return got


def lock_of(expr, locks):
    return il.lock_of(expr.strip().lstrip("&"), locks)


def walk(body, locks):
    """Yield ("call", held, callee) for each call and ("take", held, lock)
    for each mutex_enter, tracking the held set as
    intr_locks.held_calls() does; a global lock counts too."""
    held = []
    for m in il.ASSERT_RE.finditer(body):
        if m.group(1) is not None:
            name = lock_of(m.group(1), locks)
            if name is not None and name not in held:
                held.append(name)
    saved, last_exit, pending = [], [False], False
    for match in il.TOKEN_RE.finditer(body):
        tok = match.group()
        if tok == "{":
            saved.append(list(held))
            last_exit.append(False)
            pending = False
        elif tok == "}":
            state = saved.pop() if saved else []
            if len(last_exit) > 1 and last_exit.pop():
                held = state
            pending = False
        elif tok in ("return", "goto"):
            pending = True
        elif tok == ";":
            if pending:
                last_exit[-1] = True
                pending = False
        else:
            op = tok[:tok.index("(")].strip()
            if op in ("mutex_enter", "mutex_exit"):
                name = lock_of(tok[tok.index("(") + 1:tok.rindex(")")],
                               locks)
                if name is not None:
                    if op == "mutex_enter":
                        yield "take", list(held), name
                        held.append(name)
                    elif name in held:
                        held.remove(name)
                last_exit[-1] = False
                pending = False
                continue
            if op in il.KEYWORDS:
                continue
            if not pending:
                last_exit[-1] = False
            yield "call", list(held), op


def order_edges(bodies, locks):
    got = acquired(bodies, locks)
    edges = {}
    for func, body in bodies.items():
        for kind, held, what in walk(body, locks):
            takes = got.get(what, ()) if kind == "call" else (what,)
            for a in held:
                for b in takes:
                    edges.setdefault((a, b), f"{func}()" +
                                     (f" via {what}()" if kind == "call"
                                      else ""))
    return edges


def cycles(edges):
    graph = {}
    for a, b in edges:
        graph.setdefault(a, set()).add(b)
    found = []
    for a, b in edges:
        if a == b:
            found.append(f"{a} taken while held: {edges[(a, b)]}")
    state = {}

    def visit(n, path):
        state[n] = 1
        for m in graph.get(n, ()):
            if m == n:
                continue
            if state.get(m) == 1:
                cyc = path[path.index(m):] + [m]
                found.append(" -> ".join(cyc) + ": " + "; ".join(
                    edges[(cyc[i], cyc[i + 1])] for i in range(len(cyc) - 1)))
            elif m not in state:
                visit(m, path + [m])
        state[n] = 2

    for n in sorted(graph):
        if n not in state:
            visit(n, [n])
    return found


def check(texts):
    locks = set()
    bodies = {}
    for text in texts.values():
        locks.update(MUTEX_INIT.findall(strip(text)))
        bodies.update(functions(text))
    locks.update({"cm_lock", "mod_lock"})
    bad = []

    reach = reaching(bodies, OUTSIDE)
    for name, text in texts.items():
        for func, body in sorted(functions(text).items()):
            for kind, held, callee in walk(body, locks):
                held = [h for h in held if h not in HELD_ACROSS]
                if kind == "call" and held and (OUTSIDE.match(callee) or
                                                callee in reach):
                    bad.append(f"{name}: {func}() calls {callee}() holding "
                               f"{', '.join(held)}")

    bad.extend(cycles(order_edges(bodies, locks)))

    seeds = il.direct_blockers(bodies) | (BLOCKERS & set().union(
        *[calls(b) for b in bodies.values()]))
    blocking = il.blocking(bodies, seeds) | BLOCKERS
    for entry in NOBLOCK_ENTRIES:
        todo, seen = [entry], set()
        while todo:
            n = todo.pop()
            if n in seen or n not in bodies:
                continue
            seen.add(n)
            for c in calls(bodies[n]):
                if c in blocking and c not in bodies:
                    bad.append(f"{entry}() reaches {c}(), which blocks, "
                               f"through {n}()")
                elif c in bodies:
                    todo.append(c)
            if il.SLEEP_ARG.search(bodies[n]):
                bad.append(f"{entry}() reaches a sleeping allocation in "
                           f"{n}()")
    return locks, bad


def mutated(texts, name, old, new):
    if texts[name].count(old) != 1:
        raise SystemExit(f"mutation anchor not found in {name}: {old!r}")
    m = dict(texts)
    m[name] = texts[name].replace(old, new)
    return m


def main():
    texts = {f: (RDMA / f).read_text(encoding="utf-8") for f in FILES}
    locks, bad = check(texts)
    if bad:
        print("\n".join(bad))
        return 1

    cases = (
        ("a provider call under the ID lock", "rdk_cm.c",
         "\tid->rci_destroying = B_TRUE;\n\tmutex_exit(&id->rci_lock);\n"
         "\trdk_cm_destroy_common(id, B_FALSE);\n",
         "\tid->rci_destroying = B_TRUE;\n"
         "\trdk_cm_iw_disconnect(id, B_TRUE);\n\tmutex_exit(&id->rci_lock);\n"
         "\trdk_cm_destroy_common(id, B_FALSE);\n", "rdk_cm_iw_disconnect"),
        ("a MAD sent under a connection lock", "rdk_cm_roce.c",
         "\t\tmutex_exit(&c->ic_lock);\n\n\t\trdk_ibconn_timer_cancel(c, old);",
         "\t\t(void) rdk_gsi_send(c->ic_gsi, &path, buf);\n"
         "\t\tmutex_exit(&c->ic_lock);\n\n\t\trdk_ibconn_timer_cancel(c, old);",
         "rdk_gsi_send"),
        ("a connection lock under an ID lock", "rdk_cm_roce.c",
         "\tmutex_enter(&id->rci_lock);\n\tc = id->rci_conn;\n"
         "\tid->rci_conn = NULL;\n\tmutex_exit(&id->rci_lock);",
         "\tmutex_enter(&id->rci_lock);\n\tc = id->rci_conn;\n"
         "\tid->rci_conn = NULL;\n\tif (c != NULL) {\n"
         "\t\tmutex_enter(&c->ic_lock);\n\t\tmutex_exit(&c->ic_lock);\n"
         "\t}\n\tmutex_exit(&id->rci_lock);", "rci_lock -> ic_lock"),
        ("a sleeping allocation in a completion handler", "rdk_gsi.c",
         "\tif ((rx = kmem_cache_alloc(rdk_gsi_rx_cache, KM_NOSLEEP)) == "
         "NULL) {",
         "\tif ((rx = kmem_cache_alloc(rdk_gsi_rx_cache, KM_SLEEP)) == "
         "NULL) {", "sleeping allocation"),
    )
    for what, name, old, new, expect in cases:
        _, found = check(mutated(texts, name, old, new))
        if not any(expect in f for f in found):
            print(f"missed {what}: {found}")
            return 1
    print(f"PASS: no call out of rdk_cm holding its locks, no order cycle "
          f"among {len(locks)} locks, nothing blocks in the GSI handlers; "
          f"{len(cases)} mutations caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
