#!/usr/bin/env bash
# Bring up the virtual CAN interface used by the simulator and the GUI sim.
# Idempotent: safe to re-run.
set -euo pipefail
IFACE="${1:-vcan0}"

if ip link show "$IFACE" >/dev/null 2>&1; then
    echo "$IFACE already exists"
else
    sudo modprobe vcan
    sudo ip link add dev "$IFACE" type vcan
    echo "created $IFACE"
fi
sudo ip link set up "$IFACE"
ip -brief link show "$IFACE"
