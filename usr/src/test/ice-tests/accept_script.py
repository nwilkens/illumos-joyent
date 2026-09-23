#!/usr/bin/env python3
"""Run datapath_accept.sh against stub system commands."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from c_test import TESTDIR


SCRIPT = TESTDIR / "datapath_accept.sh"

# Each stub keeps the state it changes in $STUB_STATE and logs its arguments.
STUBS = {
    "dladm": r'''
case "$1" in
show-phys)
	if [ "$2" = "-p" ]; then cat "$S/device"; exit 0; fi
	echo "LINK MEDIA STATE SPEED DUPLEX DEVICE"
	echo "$(cat "$S/link") Ethernet up 25000 full $(cat "$S/device")" ;;
show-linkprop)
	cat "$S/mtu" ;;
set-linkprop)
	[ -e "$S/if" ] && exit 1
	for a in "$@"; do
		case "$a" in mtu=*) echo "${a#mtu=}" > "$S/mtu" ;; esac
	done ;;
esac
''',
    "ipadm": r'''
case "$1" in
show-if)
	[ -e "$S/if" ] || exit 1
	case "$*" in *persistent*) cat "$S/persist" 2>/dev/null ||
	    echo "--" ;; esac ;;
show-addr)
	obj=$(eval echo "\${$#}")
	case "$*" in
	*"-o addrobj"*) cut -d'|' -f1 "$S/addrs" 2>/dev/null ;;
	*"-o type"*) grep "^$obj|" "$S/addrs" | cut -d'|' -f2 ;;
	*"-o addr"*) grep "^$obj|" "$S/addrs" | cut -d'|' -f3 ;;
	esac ;;
create-if)
	[ -e "$S/if" ] && exit 1
	touch "$S/if" ;;
delete-if)
	[ -e "$S/if" ] || exit 1
	rm -f "$S/if" "$S/addrs" ;;
create-addr)
	[ -e "$S/if" ] || exit 1
	type=""; addr=""
	while [ $# -gt 1 ]; do
		case "$1" in -T) type=$2; shift ;; -a) addr=$2; shift ;; esac
		shift
	done
	echo "$1|$type|$addr" >> "$S/addrs" ;;
delete-addr)
	grep -q "^$2|" "$S/addrs" 2>/dev/null || exit 1
	grep -v "^$2|" "$S/addrs" > "$S/addrs.new"
	mv "$S/addrs.new" "$S/addrs" ;;
esac
''',
    "kstat": r'''
n=$(cat "$S/kcount" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$S/kcount"
case "$2" in
*bytes|*lso_packets) echo "$2	$n" ;;
*) echo "$2	0" ;;
esac
''',
    "netstat": r'''
case "$*" in
*inet6*) cat "$S/routes6" 2>/dev/null ;;
*) cat "$S/routes" 2>/dev/null ;;
esac
''',
    "route": r'''
case "$*" in
*inet6*) echo "default $4 UG 1 0" >> "$S/routes6" ;;
*) echo "default $4 UG 1 0" >> "$S/routes" ;;
esac
''',
    "modinfo": 'echo " 99 fffffffff 1000 1 1 ice (Intel E800 Series Ethernet)"',
    "ping": '[ -e "$S/term_on_ping" ] && rm "$S/term_on_ping" && kill -TERM $PPID\nexit 0',
    "iperf": 'echo "[SUM]  0.0-10.0 sec  11.0 GBytes  9.40 Gbits/sec"',
    "sleep": "exit 0",
}


class Host:
    """A stub host with one link, its device, MTU and IP configuration."""

    def __init__(self, work, link="ice0", device="ice0", mtu=1500,
                 addrs=(), persistent=False, routes=()):
        self.state = work / "state"
        self.bin = work / "bin"
        self.state.mkdir()
        self.bin.mkdir()
        for name, body in STUBS.items():
            path = self.bin / name
            path.write_text(f'#!/bin/sh\nS="$STUB_STATE"\n'
                            f'echo "{name} $*" >> "$S/log"\n{body}\n')
            path.chmod(0o755)
        (self.state / "link").write_text(link + "\n")
        (self.state / "device").write_text(device + "\n")
        (self.state / "mtu").write_text(f"{mtu}\n")
        if addrs:
            (self.state / "if").touch()
            (self.state / "addrs").write_text(
                "".join(f"{link}/{o}|{t}|{a}\n" for o, t, a in addrs))
        if persistent:
            (self.state / "persist").write_text("46\n")
        if routes:
            (self.state / "routes").write_text(
                "".join(f"default {gw} UG 1 0\n" for gw in routes))
        self.link = link

    def run(self, mtu="9000", **env):
        full = dict(os.environ, PATH=f"{self.bin}:/usr/bin:/bin",
                    STUB_STATE=str(self.state), ICE_TEST_LINK=self.link,
                    ICE_TEST_IPERF="iperf", **env)
        return subprocess.run(["bash", str(SCRIPT), "192.0.2.2", mtu],
                              env=full, capture_output=True, text=True,
                              timeout=60)

    def read(self, name):
        path = self.state / name
        return path.read_text() if path.exists() else ""


def check_instance(work):
    """A renamed link takes its kstat instance from the ice device."""
    host = Host(work, link="net0", device="ice3")
    result = host.run()
    assert result.returncode == 0, result.stdout + result.stderr
    log = host.read("log")
    assert "kstat -p ice:3:" in log, log
    assert "ice:net0" not in log and "ice::" not in log, log

    shutil.rmtree(work)
    work.mkdir()
    host = Host(work, link="net0", device="ice3")
    result = host.run(ICE_TEST_DEVICE="ice1")
    assert result.returncode == 1 and "ICE_TEST_DEVICE" in result.stdout
    assert "set-linkprop" not in host.read("log")

    shutil.rmtree(work)
    work.mkdir()
    host = Host(work, link="net0", device="e1000g0")
    result = host.run()
    assert result.returncode == 1 and "not an ice link" in result.stdout
    assert "set-linkprop" not in host.read("log")


def main():
    if shutil.which("bash") is None:
        print("SKIP: bash is not installed")
        return
    with tempfile.TemporaryDirectory(prefix="ice-accept-") as tmp:
        work = Path(tmp) / "host"
        work.mkdir()
        check_instance(work)
    print("PASS: datapath_accept.sh resolves the kstat instance from the "
          "device")


if __name__ == "__main__":
    main()
