#!/usr/bin/env python3
"""Fail if the glue holds an interrupt-priority lock across a firmware command.

A call graph of the glue and the vendored core finds every function that can
reach ice_sq_send_cmd(), the one routine that submits an admin or sideband
queue command, or one of the reset polls.  The glue is then walked function
by function, tracking mutex_enter() and mutex_exit() by brace depth, and no
call made while ice_lock, ice_lse_lock or a ring lock is held may reach that
set.  Adaptive locks (ice_rebuild_lock and the other thread-only locks) are
not checked.
"""

import argparse
from pathlib import Path
import re
import sys

from c_test import DRIVER


# Interrupt-priority mutexes: the OICR or a queue vector takes each of them.
WATCHED = ("ice_lock", "ice_lse_lock", "itxr_lock", "itxr_tcb_lock",
           "irxr_lock")

# Submitting a command, or waiting out a reset, can take a second or more.
ROOTS = ("ice_sq_send_cmd", "ice_check_reset", "ice_reset", "ice_pf_reset")

# Entry points the checker must classify as sleeping; a loss here means the
# call graph stopped seeing the core.
EXPECTED = ("ice_aq_send_cmd", "ice_ena_vsi_txq", "ice_dis_vsi_txq",
            "ice_aq_sff_eeprom", "ice_aq_get_phy_caps", "ice_aq_set_phy_cfg",
            "ice_update_link_info", "ice_aq_set_event_mask",
            "ice_fwlog_get", "ice_fwlog_set", "ice_fwlog_register",
            "ice_aq_get_internal_data", "ice_add_vsi", "ice_update_vsi",
            "ice_add_mac", "ice_remove_mac", "ice_set_vsi_promisc",
            "ice_clear_vsi_promisc", "ice_cfg_vsi_lan", "ice_get_caps",
            "ice_clear_pf_cfg", "ice_init_pkg", "ice_aq_set_mac_loopback",
            "ice_aq_set_port_id_led", "ice_sched_init_port", "ice_init_hw",
            "ice_aq_set_rss_lut", "ice_aq_set_rss_key", "ice_reset")

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do",
            "case", "else", "goto", "defined"}


def strip(text):
    """Blank comments, strings and preprocessor lines, keeping offsets."""
    out = []
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if line_start and c == "#":
            j = i
            while j < n:
                if text[j] == "\n" and text[j - 1] != "\\":
                    break
                j += 1
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
            continue
        if text.startswith("/*", i):
            j = text.index("*/", i + 2) + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        if c in "\"'":
            j = i + 1
            while text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (j - i - 1) + c)
            i = j + 1
            line_start = False
            continue
        out.append(c)
        line_start = c == "\n" or (line_start and c in " \t")
        i += 1
    return "".join(out)


def functions(text):
    """Map each function defined at file scope to its body text."""
    code = strip(text)
    found = {}
    depth = 0
    start = 0
    head = 0
    for i, c in enumerate(code):
        if c == "{":
            if depth == 0:
                start = i
                names = re.findall(r"(\w+)\s*\(", code[head:i])
                names = [x for x in names if x not in KEYWORDS]
                is_func = bool(names) and code[head:i].rstrip().endswith(")")
                name = names[0] if is_func else None
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                if name is not None:
                    found[name] = code[start:i + 1]
                head = i + 1
        elif c == ";" and depth == 0:
            head = i + 1
    return found


def calls(body):
    return {name for name in re.findall(r"\b(\w+)\s*\(", body)
            if name not in KEYWORDS}


def sleepers(bodies):
    """Every function that can reach one of ROOTS."""
    graph = {name: calls(body) for name, body in bodies.items()}
    reach = set(ROOTS)
    changed = True
    while changed:
        changed = False
        for name, callees in graph.items():
            if name not in reach and callees & reach:
                reach.add(name)
                changed = True
    return reach


LOCK_RE = re.compile(r"\bmutex_(enter|exit)\s*\(\s*&?([^;]*?)\)\s*;")
TOKEN_RE = re.compile(r"[{};]|\bmutex_(?:enter|exit)\s*\([^;]*\)\s*;|"
                      r"\b(?:return|goto)\b|\b\w+\s*\(")


def lock_name(expr):
    expr = expr.replace(" ", "")
    for lock in WATCHED:
        if re.search(rf"(->|\.|^){lock}$", expr):
            return lock
    return None


def held_calls(body):
    """Yield (lock, callee) for each call made while a watched lock is held.

    The walk is linear with one refinement: a block that ends in return or
    goto leaves the lock state as it found it, which covers the usual
    unlock-and-bail path.
    """
    held = []
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
        for name in held:
            yield name, callee


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
    reach = sleepers(bodies)
    missing = [name for name in EXPECTED if name not in reach]
    assert not missing, f"call graph lost sight of {missing}"

    bad = []
    for (file, func), body in sorted(glue.items()):
        for lock, callee in held_calls(body):
            if callee in reach:
                bad.append(f"{file}: {func}() calls {callee}() holding "
                           f"{lock}")
    return reach, bad


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, action="append",
                        help="check this glue file instead of the tree")
    parser.add_argument("--list", action="store_true",
                        help="print the sleeping core entry points")
    args = parser.parse_args()
    glue = args.source or sorted(DRIVER.glob("*.c"))
    core = sorted((DRIVER / "core").glob("*.c"))
    reach, bad = check(glue, core)

    # Self-test: the checker must see a planted violation.
    planted = ("static void\nf(ice_t *ice)\n{\n"
               "\tmutex_enter(&ice->ice_lock);\n"
               "\tif (x) {\n\t\tmutex_exit(&ice->ice_lock);\n"
               "\t\treturn (g(x));\n\t}\n"
               "\t(void) ice_dis_vsi_txq(pi);\n"
               "\tmutex_exit(&ice->ice_lock);\n}\n")
    found = list(held_calls(functions(planted)["f"]))
    assert ("ice_lock", "ice_dis_vsi_txq") in found, found

    # So must it see the TX queue disable put back under ice_lock.
    tx = (DRIVER / "ice_tx.c").read_text(encoding="utf-8")
    call = tx.index("status = ice_dis_vsi_txq(")
    end = tx.index(";", call) + 1
    mutated = (tx[:call] + "mutex_enter(&ice->ice_lock);\n\t" +
               tx[call:end] + "\n\tmutex_exit(&ice->ice_lock);" + tx[end:])
    body = functions(mutated)["ice_tx_ring_unprogram"]
    assert ("ice_lock", "ice_dis_vsi_txq") in list(held_calls(body))

    if args.list:
        for name in sorted(n for n in reach if n.startswith("ice_")):
            print(name)
    if bad:
        print("\n".join(bad))
        return 1
    print(f"PASS: no glue call reaches one of {len(reach)} firmware-command "
          f"or reset-poll functions while holding {', '.join(WATCHED)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
