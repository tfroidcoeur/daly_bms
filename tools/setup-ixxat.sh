#!/usr/bin/env bash
#
# Build, install and bring up the IXXAT USB-to-CAN adapter as SocketCAN can0.
#
# The IXXAT driver is NOT in the mainline kernel - the patches are still in
# review - so it has to be built out of tree from HMS's own repository. This
# script is idempotent: re-run it after a kernel update, or when the interface
# is down.
#
# Needs root for the install and the link configuration. Run it with sudo:
#
#     sudo ./tools/setup-ixxat.sh [bitrate]        default 250000
#
set -euo pipefail

BITRATE="${1:-250000}"
IFACE="${IFACE:-can0}"
SRC="${IXXAT_SRC:-/usr/local/src/ixxat-socketcan-usb}"
REPO="https://github.com/hms-networks/ixxat-socketcan-usb.git"

if [ "$(id -u)" -ne 0 ]; then
    echo "needs root: sudo $0 $*" >&2
    exit 1
fi

# --- is the adapter even plugged in? ---------------------------------------
if ! lsusb -d 08d8: >/dev/null 2>&1; then
    echo "No IXXAT device on USB (vendor 08d8). Plug it in first." >&2
    lsusb | grep -i ixxat || true
    exit 1
fi
echo "found: $(lsusb -d 08d8: | head -1)"

# --- prerequisites ----------------------------------------------------------
# dkms matters: without it the module is installed by hand and silently stops
# existing at the next kernel upgrade.
need=()
command -v dkms >/dev/null || need+=(dkms)
command -v mokutil >/dev/null || need+=(mokutil)
command -v gcc  >/dev/null || need+=(build-essential)
[ -d "/lib/modules/$(uname -r)/build" ] || need+=("linux-headers-$(uname -r)")
if [ ${#need[@]} -gt 0 ]; then
    echo "installing: ${need[*]}"
    apt-get install -y "${need[@]}"
fi

# --- source -----------------------------------------------------------------
if [ -d "$SRC/.git" ]; then
    echo "updating $SRC"
    git -C "$SRC" pull --ff-only
else
    echo "cloning into $SRC"
    mkdir -p "$(dirname "$SRC")"
    git clone --depth 1 "$REPO" "$SRC"
fi

# --- build and install ------------------------------------------------------
make -C "$SRC" all
make -C "$SRC" install

# --- Secure Boot ------------------------------------------------------------
# DKMS signs the module with a key it generates at /var/lib/shim-signed/mok/,
# but a freshly generated key is not yet trusted by the firmware. Until it is
# enrolled, the kernel refuses the module with
#   "Loading of module with unavailable key is rejected"
# which looks nothing like a signing problem in the logs.
MOK=/var/lib/shim-signed/mok/MOK.der
if ! modprobe ix_usb_can 2>/dev/null; then
    if [ "$(mokutil --sb-state 2>/dev/null)" = "SecureBoot enabled" ] &&
       [ -f "$MOK" ] && ! mokutil --test-key "$MOK" 2>/dev/null | grep -q "already enrolled"; then
        cat >&2 <<MSG

The driver built and installed, but Secure Boot will not load it: DKMS signed it
with a key the firmware does not trust yet. Enrol that key once:

    sudo mokutil --import $MOK

It asks you to invent a password, twice. That password is used once, at the next
boot, and then never again - it is not your login password.

Then reboot. A blue "MOK Manager" screen appears before the OS:

    Enroll MOK  ->  Continue  ->  Yes  ->  (the password)  ->  Reboot

Then re-run this script. Every later kernel update rebuilds through DKMS and
signs with the same, now-trusted key, so this is a one-time step.

MSG
        exit 1
    fi
    echo "modprobe ix_usb_can failed. Check: dmesg | tail -20" >&2
    exit 1
fi

# udev takes a moment to rename the new netdev.
for _ in $(seq 20); do
    ip link show "$IFACE" >/dev/null 2>&1 && break
    sleep 0.25
done

if ! ip link show "$IFACE" >/dev/null 2>&1; then
    echo "module loaded but $IFACE did not appear. Check: dmesg | tail -20" >&2
    exit 1
fi

# --- keep it coming up by itself --------------------------------------------
# Without this the interface returns after every unplug as STOPPED and
# unconfigured, which from the far end of the bus is indistinguishable from a
# broken cable.
RULES=/etc/udev/rules.d/90-ixxat-can.rules
if ! cmp -s "$(dirname "$0")/90-ixxat-can.rules" "$RULES"; then
    install -m 0644 "$(dirname "$0")/90-ixxat-can.rules" "$RULES"
    udevadm control --reload-rules
    echo "installed $RULES"
fi

# --- bring the interface up -------------------------------------------------
# Down first so a re-run can change the bit rate.
ip link set "$IFACE" down 2>/dev/null || true
ip link set "$IFACE" type can bitrate "$BITRATE" restart-ms 100
ip link set "$IFACE" up

echo
ip -details -brief link show "$IFACE"
echo
echo "ready:  ./build/sim_headless $IFACE --raw"
