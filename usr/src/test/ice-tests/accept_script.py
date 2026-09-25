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
	*) grep -q "^$obj|" "$S/addrs" 2>/dev/null || exit 1 ;;
	esac ;;
create-if)
	[ -e "$S/if" ] && exit 1
	touch "$S/if" ;;
delete-if)
	[ -e "$S/fail_delete" ] && exit 1
	[ -e "$S/if" ] || exit 1
	rm -f "$S/if" "$S/addrs"
	# Routes over the link go with it; with drop_routes, every route does.
	for r in routes routes6; do
		[ -e "$S/$r" ] || continue
		if [ -e "$S/drop_routes" ]; then
			: > "$S/$r"
		else
			grep -v " $(cat "$S/link")\$" "$S/$r" > "$S/$r.new"
			mv "$S/$r.new" "$S/$r"
		fi
	done ;;
create-addr)
	[ -e "$S/if" ] || exit 1
	[ -e "$S/fail_create" ] && exit 1
	type=""; addr=""
	while [ $# -gt 1 ]; do
		case "$1" in -T) type=$2; shift ;; -a) addr=$2; shift ;; esac
		shift
	done
	case "$1" in */rs*) [ -e "$S/lose_restored" ] && exit 0 ;; esac
	echo "$1|$type|$addr" >> "$S/addrs" ;;
delete-addr)
	[ -e "$S/fail_delete" ] && exit 1
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
[ -e "$S/if" ] || exit 1
# The kernel picks the interface; route_ifp makes it pick another one, and
# route_twice leaves a second copy.
ifp=$(cat "$S/route_ifp" 2>/dev/null || cat "$S/link")
n=1; [ -e "$S/route_twice" ] && n=2
while [ $n -gt 0 ]; do
	case "$*" in
	*inet6*) echo "default $5 UG 1 0 $ifp" >> "$S/routes6" ;;
	*) echo "default $4 UG 1 0 $ifp" >> "$S/routes" ;;
	esac
	n=$((n - 1))
done
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
            # A route is a gateway over the link, or (dest, gw, flags, if).
            (self.state / "routes").write_text("".join(
                f"default {r} UG 1 0 {link}\n" if isinstance(r, str) else
                f"{r[0]} {r[1]} {r[2]} 1 0 {r[3]}\n" for r in routes))
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

    def ip(self):
        """The interface, its (type, address) set and the MTU."""
        addrs = {tuple(line.split("|")[1:]) for line in
                 self.read("addrs").splitlines()}
        return ((self.state / "if").exists(), addrs, self.read("mtu").strip(),
                self.read("routes").split())

    def changes(self):
        return [line for line in self.read("log").splitlines()
                if line.split()[1:2] in (["set-linkprop"], ["delete-if"],
                                         ["create-if"], ["create-addr"],
                                         ["delete-addr"])]


def fresh(work, **kwargs):
    shutil.rmtree(work)
    work.mkdir()
    return Host(work, **kwargs)


def check_instance(work):
    """A renamed link takes its kstat instance from the ice device."""
    host = Host(work, link="net0", device="ice3")
    result = host.run()
    assert result.returncode == 0, result.stdout + result.stderr
    log = host.read("log")
    assert "kstat -p ice:3:" in log, log
    assert "ice:net0" not in log and "ice::" not in log, log

    host = fresh(work, link="net0", device="ice3")
    result = host.run(ICE_TEST_DEVICE="ice1")
    assert result.returncode == 1 and "ICE_TEST_DEVICE" in result.stdout
    assert host.changes() == []

    host = fresh(work, link="net0", device="e1000g0")
    result = host.run()
    assert result.returncode == 1 and "not an ice link" in result.stdout
    assert host.changes() == []


ADDRS = (("v4", "static", "10.1.2.3/24"), ("v6", "addrconf", "fe80::1/10"))


def check_refusal(work):
    """A link with IP configuration is left alone without the override."""
    host = fresh(work, addrs=ADDRS, routes=("10.1.2.1",))
    before = host.ip()
    result = host.run()
    assert result.returncode == 1, result.stdout
    assert "ICE_TEST_ALLOW_IP=1" in result.stdout, result.stdout
    assert host.changes() == [] and host.ip() == before

    # The override does not cover what the test cannot put back.
    host = fresh(work, addrs=ADDRS, persistent=True)
    result = host.run(ICE_TEST_ALLOW_IP="1")
    assert result.returncode == 1 and "persistent" in result.stdout
    assert host.changes() == []
    host = fresh(work, addrs=(("_a", "from-gz", "10.1.2.3/24"),))
    result = host.run(ICE_TEST_ALLOW_IP="1")
    assert result.returncode == 1 and "from-gz" in result.stdout
    assert host.changes() == []


def check_restore(work):
    """The MTU and IP configuration come back on exit and on a signal."""
    for interrupt in (False, True):
        host = fresh(work)
        if interrupt:
            (host.state / "term_on_ping").touch()
        result = host.run()
        assert result.returncode == (143 if interrupt else 0), result.stdout
        assert host.ip() == (False, set(), "1500", []), host.ip()
        assert "delete-if ice0" in host.read("log")
        for line in host.changes():
            if line.split()[1] in ("set-linkprop", "create-if",
                                   "create-addr"):
                assert " -t " in line, line

        host = fresh(work, addrs=ADDRS, routes=("10.1.2.1",))
        if interrupt:
            (host.state / "term_on_ping").touch()
        result = host.run(ICE_TEST_ALLOW_IP="1")
        assert result.returncode == (143 if interrupt else 0), result.stdout
        present, addrs, mtu, routes = host.ip()
        assert present and mtu == "1500", host.ip()
        assert addrs == {(t, a if t == "static" else "")
                         for _, t, a in ADDRS}, addrs
        assert routes[:2] == ["default", "10.1.2.1"], routes
        assert "RESTORE FAILED" not in result.stdout


def check_restore_failure(work):
    """A restore that fails makes the run fail."""
    host = fresh(work)
    (host.bin / "dladm").write_text((host.bin / "dladm").read_text().replace(
        'echo "${a#mtu=}" > "$S/mtu"',
        '[ "${a#mtu=}" = 1500 ] && exit 1; echo "${a#mtu=}" > "$S/mtu"'))
    result = host.run()
    assert result.returncode == 1, result.stdout
    assert "RESTORE FAILED: mtu 1500" in result.stdout, result.stdout


def check_delete_failure(work):
    """A delete that fails while the object remains makes the run fail."""
    host = fresh(work)
    (host.state / "term_on_ping").touch()
    (host.state / "fail_delete").touch()
    result = host.run()
    assert result.returncode == 1, result.stdout
    assert "RESTORE FAILED: ipadm delete-addr ice0/v4accept" in \
        result.stdout, result.stdout
    assert "RESTORE FAILED: ice0 still has an IP interface" in \
        result.stdout, result.stdout
    assert "was not fully restored" in result.stdout

    # An object that is confirmed absent is not a failure.
    host = fresh(work)
    (host.state / "fail_create").touch()
    result = host.run()
    assert "could not plumb" in result.stdout, result.stdout
    assert "RESTORE FAILED" not in result.stdout, result.stdout
    assert host.ip() == (False, set(), "1500", []), host.ip()

    # The final state is checked, not only each command's status.
    host = fresh(work, addrs=ADDRS, routes=("10.1.2.1",))
    (host.state / "lose_restored").touch()
    result = host.run(ICE_TEST_ALLOW_IP="1")
    assert result.returncode == 1, result.stdout
    assert "RESTORE FAILED: ice0 addresses are" in result.stdout, \
        result.stdout


def check_routes(work):
    """The routes must come back exactly: same interface, nothing extra."""
    other = ("default", "10.9.0.1", "UG", "net9")
    host = fresh(work, addrs=ADDRS, routes=("10.1.2.1", other))
    result = host.run(ICE_TEST_ALLOW_IP="1")
    assert result.returncode == 0, result.stdout
    assert "RESTORE FAILED" not in result.stdout, result.stdout
    assert "default 10.9.0.1 UG 1 0 net9" in host.read("routes")

    # Restored over another interface: present, but not the same route.
    host = fresh(work, addrs=ADDRS, routes=("10.1.2.1",))
    (host.state / "route_ifp").write_text("net9\n")
    result = host.run(ICE_TEST_ALLOW_IP="1")
    assert result.returncode == 1, result.stdout
    assert "RESTORE FAILED: routes differ" in result.stdout, result.stdout
    assert "missing inet|default|10.1.2.1|ice0" in result.stdout
    assert "extra   inet|default|10.1.2.1|net9" in result.stdout

    # A route restored twice.
    host = fresh(work, addrs=ADDRS, routes=("10.1.2.1",))
    (host.state / "route_twice").touch()
    result = host.run(ICE_TEST_ALLOW_IP="1")
    assert result.returncode == 1, result.stdout
    assert "extra   inet|default|10.1.2.1|ice0" in result.stdout

    # A default route over another interface that the test lost.
    host = fresh(work, addrs=ADDRS, routes=("10.1.2.1", other))
    (host.state / "drop_routes").touch()
    result = host.run(ICE_TEST_ALLOW_IP="1")
    assert result.returncode == 1, result.stdout
    assert "missing inet|default|10.9.0.1|net9" in result.stdout

    # One that appeared during the test, with a link that had no IP.
    host = fresh(work)
    (host.bin / "ipadm").write_text((host.bin / "ipadm").read_text().replace(
        'create-if)\n', 'create-if)\n\techo "default 10.9.9.9 UG 1 0 '
        'net9" >> "$S/routes"\n'))
    result = host.run()
    assert result.returncode == 1, result.stdout
    assert "extra   inet|default|10.9.9.9|net9" in result.stdout

    # A static gateway route over the link cannot be put back, so the test
    # refuses the link.
    host = fresh(work, addrs=ADDRS,
                 routes=("10.1.2.1", ("10.20.0.0", "10.1.2.1", "UG", "ice0")))
    result = host.run(ICE_TEST_ALLOW_IP="1")
    assert result.returncode == 1 and "cannot restore" in result.stdout
    assert host.changes() == []


def check_wrapper(work):
    """icetest refuses a configured link before it runs a datapath test."""
    host = fresh(work, addrs=ADDRS)
    (host.bin / "dladm").write_text(
        '#!/bin/sh\ncase "$*" in *link,device*) echo "ice0:ice0" ;; '
        '*) echo ice0 ;; esac\n')
    env = dict(os.environ, PATH=f"{host.bin}:/usr/bin:/bin",
               STUB_STATE=str(host.state))
    env.pop("ICE_TEST_ALLOW_IP", None)
    wrapper = TESTDIR / "cmd/icetest.ksh"
    result = subprocess.run(["ksh", str(wrapper), "-p", "192.0.2.2"],
                            env=env, capture_output=True, text=True,
                            timeout=60)
    assert result.returncode == 1, result.stderr
    assert "ICE_TEST_ALLOW_IP=1" in result.stderr, result.stderr

    led = (TESTDIR / "tests/led.ksh").read_text()
    trap = led.index("trap '$dlled -s default")
    assert trap < led.index('$dlled -s ident "$link"')


def main():
    if shutil.which("bash") is None:
        print("SKIP: bash is not installed")
        return
    with tempfile.TemporaryDirectory(prefix="ice-accept-") as tmp:
        work = Path(tmp) / "host"
        work.mkdir()
        check_instance(work)
        check_refusal(work)
        check_restore(work)
        check_restore_failure(work)
        check_delete_failure(work)
        check_routes(work)
        if shutil.which("ksh") is not None:
            check_wrapper(work)
    print("PASS: datapath_accept.sh resolves the kstat instance from the "
          "device, refuses configured links and restores and verifies the "
          "link and its routes")


if __name__ == "__main__":
    main()
