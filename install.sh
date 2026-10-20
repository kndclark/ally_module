#!/bin/bash
# Build OGC PR #17's hid-asus + Dynamic Lighting class out of tree and install
# them on SteamOS, displacing the ally_module build of hid-asus for the test.
# ./uninstall.sh puts the previous hid-asus.ko.zst back.
set -euo pipefail

KVER=$(uname -r)
KDIR="/lib/modules/${KVER}/build"
DEST="/lib/modules/${KVER}/updates/ally/drivers"
HID_DST="${DEST}/hid/hid-asus.ko.zst"
LED_DST="${DEST}/leds/led-class-dynamic.ko.zst"
BACKUP="${HID_DST}.bak-ally_module"

log()   { echo "[dynlight] $*"; }
error() { echo "[dynlight] ERROR: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] && error "run as the normal user; sudo is used where needed"
[ -d "$KDIR" ] || error "no headers at $KDIR (ally_module/install.sh installs them)"

log "Building against ${KVER}"
make -s clean KDIR="$KDIR" >/dev/null 2>&1 || true
make KDIR="$KDIR"
[ -f hid-asus.ko ] && [ -f led-class-dynamic.ko ] || error "build produced no modules"

# Pre-flight: every undefined symbol must exist in the running kernel or be
# exported by the sibling module. A missing symbol would fail modprobe at
# boot and leave the controller without a driver.
log "Pre-flight symbol check"
sudo true
exports=$(nm --defined-only led-class-dynamic.ko hid-asus.ko | awk '$2 ~ /[TD]/ {print $3}')
for ko in led-class-dynamic.ko hid-asus.ko; do
    for sym in $(nm -u "$ko" | awk '{print $2}'); do
        grep -qw "$sym" <<<"$exports" && continue
        sudo grep -qE " [TtDdRrBb] ${sym}$| [TtDdRrBb] ${sym}\.[a-z]" /proc/kallsyms && continue
        [[ "$sym" == __fentry__ || "$sym" == __stack_chk_fail || "$sym" == __x86_* || "$sym" == _printk ]] && continue
        error "$ko needs '$sym', which this kernel does not export. Not installing."
    done
done

zstd -qf hid-asus.ko led-class-dynamic.ko

if [ -f /usr/bin/steamos-readonly ]; then
    sudo steamos-readonly disable
fi
sudo mkdir -p "${DEST}/hid" "${DEST}/leds"
if [ -f "$HID_DST" ] && [ ! -f "$BACKUP" ]; then
    log "Keeping the current hid-asus.ko.zst as $(basename "$BACKUP")"
    sudo cp -a "$HID_DST" "$BACKUP"
fi
sudo install -m 644 hid-asus.ko.zst "$HID_DST"
sudo install -m 644 led-class-dynamic.ko.zst "$LED_DST"
sudo depmod -a "$KVER"

# Same arrangement as linux-unstable: hid-asus alone handles the Ally.
if ! grep -rqsE '^[[:space:]]*blacklist[[:space:]]+hid[-_]asus[-_]ally' /etc/modprobe.d/; then
    echo "blacklist hid_asus_ally" | sudo tee /etc/modprobe.d/ally-dynlight.conf >/dev/null
fi

log "Reloading hid-asus"
sudo modprobe -r hid_asus 2>/dev/null || true
sudo modprobe -r hid_asus_ally 2>/dev/null || true
sudo modprobe -r led_class_dynamic 2>/dev/null || true
sudo modprobe hid_asus
sleep 2
log "Loaded: $(lsmod | awk '$1 ~ /^(hid_asus|led_class_dynamic)$/ {printf "%s ", $1}')"
log "LED nodes:"
ls -1 /sys/class/leds/ | sed 's/^/    /'
log "hid-asus messages since load:"
sudo dmesg | grep -iE 'asus|lamparray|dynamic' | tail -25 | sed 's/^/    /'
log "Done. Run tests/dynlight_smoke.sh for the walk-through; ./uninstall.sh restores the previous driver."
