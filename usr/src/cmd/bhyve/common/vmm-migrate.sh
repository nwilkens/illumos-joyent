#!/usr/bin/bash
#
# vmm-migrate — warm migration of a bhyve VM between compute nodes.
#
# Usage:
#   vmm-migrate <uuid> <dest_host>
#
# Runs in the global zone on the source host. Requires:
#   - SSH key access to the destination host
#   - bhyve binary with checkpoint support deployed on both hosts
#   - Destination VM already exists (same UUID, created via vmadm)
#
# Flow:
#   1. ZFS snapshot @mig-base → send to dest (VM running)
#   2. kill -USR1 bhyve → pause + checkpoint (VM stays paused)
#   3. ZFS snapshot @mig-final → incremental send (tiny, VM paused)
#   4. Transfer checkpoint file to dest
#   5. Start dest VM (auto-detects checkpoint, restores state)
#   6. Stop source VM
#

set -o pipefail

UUID="$1"
DEST="$2"
SSH_KEY="${SSH_KEY:-/root/.ssh/sdc.id_rsa}"
SSH="ssh -i $SSH_KEY -o StrictHostKeyChecking=no -o IdentitiesOnly=yes -o ConnectTimeout=5"
SCP="scp -i $SSH_KEY -o StrictHostKeyChecking=no -o IdentitiesOnly=yes"

die() { echo "ABORT: $*" >&2; exit 1; }

[[ -n "$UUID" && -n "$DEST" ]] || die "Usage: vmm-migrate <uuid> <dest_host>"

STATE=$(vmadm get "$UUID" 2>/dev/null | json state)
[[ "$STATE" == "running" ]] || die "VM $UUID not running (state=$STATE)"

ZONE_ROOT="/zones/$UUID"
DISK_DS="zones/$UUID/disk0"
CKPT="${ZONE_ROOT}/root/checkpoints/vm.checkpoint"

# Find bhyve PID (GZ process, not zone PID)
PID=$(ps -ef | grep "bhyve.rust.*$UUID" | grep -v grep | awk '{print $2}' | head -1)
[[ -n "$PID" ]] || die "cannot find bhyve PID"

echo "=== vmm-migrate $UUID → $DEST (PID $PID) ==="

# --- Phase 1: ZFS base send (VM running) ---
echo "[1/6] ZFS base snapshot..."
zfs destroy "${DISK_DS}@mig-base" 2>/dev/null
zfs destroy "${DISK_DS}@mig-final" 2>/dev/null
zfs snapshot "${DISK_DS}@mig-base" || die "snapshot failed"
echo "[1/6] Sending $(zfs send -nP ${DISK_DS}@mig-base 2>&1 | grep size | awk '{print $2}') bytes..."
zfs send "${DISK_DS}@mig-base" | $SSH "root@${DEST}" "zfs recv -F ${DISK_DS}" \
    || die "zfs send failed"

# --- Phase 2: Pause + checkpoint ---
echo "[2/6] Pausing VM + checkpoint..."
rm -f "$CKPT"
kill -USR1 "$PID"
for i in $(seq 1 30); do [[ -f "$CKPT" ]] && break; sleep 0.5; done
[[ -f "$CKPT" ]] || die "checkpoint not created (check zone log)"
echo "      $(ls -la "$CKPT" | awk '{print $5}') bytes, VM PAUSED"

# --- Phase 3: ZFS final incremental (VM paused) ---
echo "[3/6] ZFS final incremental..."
zfs snapshot "${DISK_DS}@mig-final" || die "final snapshot failed"
zfs send -i "${DISK_DS}@mig-base" "${DISK_DS}@mig-final" | \
    $SSH "root@${DEST}" "zfs recv -F ${DISK_DS}" \
    || die "incremental send failed (source VM still paused!)"

# --- Phase 4: Transfer checkpoint ---
echo "[4/6] Transferring checkpoint..."
$SCP -q "$CKPT" "root@${DEST}:${CKPT}" || die "checkpoint transfer failed"

# --- Phase 5: Start destination ---
echo "[5/6] Starting dest VM..."
# Handle "down" state from previous failed migration by halting zone first
$SSH "root@${DEST}" "
    STATE=\$(vmadm get $UUID 2>/dev/null | json state)
    if [[ \"\$STATE\" == 'down' ]]; then
        zoneadm -z $UUID halt 2>/dev/null
        sleep 1
    fi
    vmadm start $UUID" 2>&1 || die "dest VM start failed"

# --- Phase 6: Stop source ---
echo "[6/6] Stopping source VM..."
vmadm stop "$UUID" -F 2>/dev/null

# Cleanup
zfs destroy "${DISK_DS}@mig-base" 2>/dev/null
zfs destroy "${DISK_DS}@mig-final" 2>/dev/null

echo "=== Migration complete: $UUID now on $DEST ==="
