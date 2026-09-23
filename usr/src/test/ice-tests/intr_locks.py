#!/usr/bin/env python3
"""Fail if the glue sleeps, frees DMA or frees messages under an interrupt lock.

An interrupt-priority mutex is held by interrupt handlers and by threads that
an interrupt can preempt, so nothing done under it may block: no KM_SLEEP or
DDI_DMA_SLEEP allocation, no DMA memory or handle allocation or release, and
no freemsg(), whose free routine can enter another lock.  The glue is walked
as in aq_locks.py.  A function that asserts it holds one of the locks starts
with that lock held, and a call reaches a forbidden operation if the callee,
or any glue or core function it calls, performs one.
"""

import argparse
from pathlib import Path
import re
import sys

from aq_locks import KEYWORDS, WATCHED, functions, lock_name
from c_test import DRIVER


# Operations that can block, release DMA resources, or run a free routine.
FORBIDDEN = ("ddi_dma_alloc_handle", "ddi_dma_mem_alloc",
             "ddi_dma_free_handle", "ddi_dma_mem_free", "ice_dma_alloc",
             "ice_dma_free", "freemsg", "freemsgchain", "freeb", "kmem_free",
             "ddi_intr_alloc", "ddi_intr_free", "ddi_intr_add_handler",
             "ddi_intr_remove_handler", "mac_ring_intr_set")
SLEEP_ARGS = re.compile(r"\b(KM_SLEEP|DDI_DMA_SLEEP)\b")

LOCK_RE = re.compile(r"\bmutex_(enter|exit)\s*\(\s*&?([^;]*?)\)\s*;")
TOKEN_RE = re.compile(r"[{};]|\bmutex_(?:enter|exit)\s*\([^;]*\)\s*;|"
                      r"\b(?:return|goto)\b|\b\w+\s*\(")
ASSERT_RE = re.compile(r"ASSERT\(MUTEX_HELD\(\s*&?([^)]*?)\)\)")


def arguments(body, start):
    """The text between the parenthesis at start and its match."""
    depth = 0
    for i in range(start, len(body)):
        if body[i] == "(":
            depth += 1
        elif body[i] == ")":
            depth -= 1
            if depth == 0:
                return body[start + 1:i]
    return body[start + 1:]


def held_calls(body):
    """Yield (lock, callee, arguments) for each call under a watched lock."""
    held = []
    for match in ASSERT_RE.finditer(body):
        name = lock_name(match.group(1))
        if name is not None and name not in held:
            held.append(name)
    saved = []
    last_exit = [False]
    pending = False
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
        lock = LOCK_RE.match(tok)
        if lock is not None:
            name = lock_name(lock.group(2))
            if name is not None:
                if lock.group(1) == "enter":
                    held.append(name)
                elif name in held:
                    held.remove(name)
            last_exit[-1] = False
            pending = False
            continue
        callee = tok[:tok.index("(")].strip()
        if callee in KEYWORDS:
            continue
        if not pending:
            last_exit[-1] = False
        args = arguments(body, match.end() - 1)
        for name in held:
            yield name, callee, args


def blocking(bodies):
    """Every function that performs, or calls one that performs, a forbidden
    operation."""
    graph = {}
    reach = set(FORBIDDEN)
    for name, body in bodies.items():
        graph[name] = {c for c in re.findall(r"\b(\w+)\s*\(", body)
                       if c not in KEYWORDS}
        if SLEEP_ARGS.search(body):
            reach.add(name)
    changed = True
    while changed:
        changed = False
        for name, callees in graph.items():
            if name not in reach and callees & reach:
                reach.add(name)
                changed = True
    return reach


def check(glue_paths, core_paths):
    bodies = {}
    for path in core_paths:
        bodies.update(functions(path.read_text(encoding="utf-8",
                                               errors="replace")))
    glue = {}
    for path in glue_paths:
        for name, body in functions(path.read_text(encoding="utf-8")).items():
            glue[(path.name, name)] = body
            bodies[name] = body
    reach = blocking(bodies)
    bad = []
    for (file, func), body in sorted(glue.items()):
        for lock, callee, args in held_calls(body):
            if callee in reach or SLEEP_ARGS.search(args):
                bad.append(f"{file}: {func}() calls {callee}() holding "
                           f"{lock}")
    return bad


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, action="append",
                        help="check this glue file instead of the tree")
    args = parser.parse_args()
    glue = args.source or sorted(DRIVER.glob("*.c"))
    core = sorted((DRIVER / "core").glob("*.c"))

    # Self-test: each kind of violation is seen, directly and through a
    # helper, and an asserted lock counts as held.
    planted = ("static void\nh(void)\n{\n\tice_dma_free(&b);\n}\n"
               "static void\nf(ice_rx_ring_t *irr)\n{\n"
               "\tmutex_enter(&irr->irxr_lock);\n"
               "\tp = kmem_zalloc(n, KM_SLEEP);\n\th();\n"
               "\tfreemsg(mp);\n\tmutex_exit(&irr->irxr_lock);\n"
               "\tfreemsg(mp);\n}\n"
               "static void\ng(ice_tx_ring_t *itr)\n{\n"
               "\tASSERT(MUTEX_HELD(&itr->itxr_lock));\n"
               "\t(void) ddi_dma_mem_alloc(h, n, a, f, DDI_DMA_DONTWAIT, "
               "NULL, &v, &l, &ah);\n}\n")
    bodies = functions(planted)
    reach = blocking(bodies)
    found = [(lock, callee) for lock, callee, a in held_calls(bodies["f"])
             if callee in reach or SLEEP_ARGS.search(a)]
    assert found == [("irxr_lock", "kmem_zalloc"), ("irxr_lock", "h"),
                     ("irxr_lock", "freemsg")], found
    found = [(lock, callee) for lock, callee, a in held_calls(bodies["g"])
             if callee in reach]
    assert found == [("itxr_lock", "ddi_dma_mem_alloc")], found
    assert set(WATCHED) >= {"ice_lock", "ice_lse_lock", "itxr_lock",
                            "itxr_tcb_lock", "irxr_lock"}

    bad = check(glue, core)
    if bad:
        print("\n".join(bad))
        return 1
    print(f"PASS: no sleeping allocation, DMA allocation or release, or "
          f"message free under {', '.join(WATCHED)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
