#!/bin/bash

# ROG Ally X LED Module Uninstaller for SteamOS
# This script reverts the driver patch and restores the original system driver.

set -e

# --- Configuration ---
MODULE_NAME="hid-asus"
MODULE_FILE="${MODULE_NAME}.ko"
MODULE_ZST="${MODULE_FILE}.zst"
STUB_NAME="asus-wmi-stub"
STUB_ZST="${STUB_NAME}.ko.zst"
UPDATES_PATH="/lib/modules/$(uname -r)/updates/ally"
TARGET_USER="${SUDO_USER:-$(whoami)}"

# --- Colors ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# --- 1. Environment Checks ---

log "Checking environment..."

if [ "$EUID" -ne 0 ]; then
    error "Please run this script with sudo: sudo ./uninstall.sh"
fi

if [ -f /usr/bin/steamos-readonly ]; then
    log "Ensuring filesystem is writeable..."
    steamos-readonly disable
fi

# --- 2. Restoration Logic ---

# Turn off LEDs while driver is still active
log "Turning off LED rings..."
for led_path in /sys/class/leds/*:rgb:joystick_rings/brightness; do
    if [ -f "$led_path" ]; then
        echo 0 > "$led_path" 2>/dev/null || warn "Could not turn off LED at $led_path"
    fi
done

# Remove out-of-tree modules from updates/ally
if [ -d "$UPDATES_PATH" ]; then
    log "Removing out-of-tree modules from $UPDATES_PATH..."
    rm -rf "$UPDATES_PATH"
fi
rm -rf /lib/modules/*/updates/ally 2>/dev/null || true

# Restore stock kernel modules under /lib/modules/$(uname -r)/kernel/ if modified
base_kdir="/lib/modules/$(uname -r)/kernel"
stock_dir="/home/${TARGET_USER}/stock_modules"
stock_files=(
    "drivers/hid/hid-asus.ko.zst"
    "sound/soc/amd/snd-acp-config.ko.zst"
    "sound/soc/amd/acp/snd-soc-acpi-amd-match.ko.zst"
    "sound/soc/sdw_utils/snd-soc-sdw-utils.ko.zst"
    "sound/soc/codecs/snd-soc-tas2783-sdw.ko.zst"
    "sound/soc/codecs/snd-soc-rt721-sdca.ko.zst"
    "sound/soc/amd/acp/snd-acp-sdw-legacy-mach.ko.zst"
    "sound/soc/amd/ps/snd-pci-ps.ko.zst"
)
for rel in "${stock_files[@]}"; do
    dst="${base_kdir}/${rel}"
    bak="${dst}.bak"
    if [ -f "$bak" ]; then
        log "Restoring stock $rel from backup..."
        mv -f "$bak" "$dst"
    elif [ -d "$stock_dir" ]; then
        cand=$(find "$stock_dir" -type f -name "$(basename "$dst")" 2>/dev/null | head -n 1)
        if [ -n "$cand" ] && [ -f "$cand" ]; then
            log "Restoring pristine stock $rel from package extract..."
            cp -f "$cand" "$dst"
        fi
    fi
done
rm -f "${base_kdir}/drivers/hid/asus-wmi-stub.ko.zst"

# Remove TAS2783 firmware symlinks
fw_dir="/lib/firmware/ti/audio/tas2783"
for fw in 1954-1-0-0x8.bin.zst 1954-1-0-0xB.bin.zst 1954-0-0-0x8.bin.zst 1954-0-0-0xB.bin.zst; do
    if [ -L "$fw_dir/$fw" ]; then
        log "Removing firmware symlink $fw..."
        rm -f "$fw_dir/$fw"
    fi
done

# Remove TAS2783 UCM profiles
log "Removing TAS2783 UCM profiles..."
rm -f /usr/share/alsa/ucm2/sof-soundwire/tas2783.conf
rm -f /usr/share/alsa/ucm2/codecs/tas2783/init.conf
rmdir /usr/share/alsa/ucm2/codecs/tas2783 2>/dev/null || true

# Remove emergency recovery guard service
log "Removing emergency recovery guard service..."
rm -f /etc/systemd/system/emergency.target.wants/ally-emergency-recovery.service \
      /etc/systemd/system/rescue.target.wants/ally-emergency-recovery.service \
      /etc/systemd/system/ally-emergency-recovery.service

# --- 3. Remove Blacklist ---

BLACKLIST_FILE="/etc/modprobe.d/hid-asus-ally-blacklist.conf"
if [ -f "$BLACKLIST_FILE" ]; then
    log "Removing modprobe blacklist for hid_asus_ally..."
    rm -f "$BLACKLIST_FILE"
fi

# A 'blacklist hid_asus' left over from testing blocks the stock ASUS driver
# too, so the machine would come back with no LEDs and no working side buttons
# and nothing to explain why. Match our module only, never hid_asus_ally.
STRAY=$(grep -rlE "^[[:space:]]*blacklist[[:space:]]+hid[-_]asus([[:space:]]|$)" \
    /etc/modprobe.d/ 2>/dev/null || true)
if [ -n "$STRAY" ]; then
    warn "These files blacklist hid_asus, which also blocks the stock driver:"
    printf '%s\n' "$STRAY" | while read -r f; do warn "    $f"; done
    read -p "Remove them? (y/N) " confirm_bl
    if [[ $confirm_bl =~ ^[Yy]$ ]]; then
        printf '%s\n' "$STRAY" | xargs -r rm -f
        log "Removed."
    else
        warn "Left in place - the stock driver will not load until they are gone."
    fi
fi

# --- 4. System Sync ---

log "Updating dependency map..."
depmod -a

log "Reloading stock module..."
if lsmod | grep -q "${MODULE_NAME//-/_}"; then
    modprobe -r "${MODULE_NAME//-/_}"
fi
modprobe "${MODULE_NAME//-/_}" || warn "Original module failed to load. It may have been removed."

# Report what actually owns the hardware now. A successful modprobe does not
# mean the interfaces got bound, and an unbound Ally is indistinguishable from
# broken hardware from userspace.
log "Verifying driver binding on every ROG Ally interface..."
sleep 1
found=0
for dev in /sys/bus/hid/devices/*; do
    real=$(readlink -f "$dev")
    usbdev=$(dirname "$(dirname "$real")")
    vid=$(cat "$usbdev/idVendor" 2>/dev/null)
    pid=$(cat "$usbdev/idProduct" 2>/dev/null)
    case "$vid:$pid" in
        0b05:1abe|0b05:1b4c) ;;
        *) continue ;;
    esac
    found=1
    if [ -e "$dev/driver" ]; then
        log "  $(basename "$dev") -> $(basename "$(readlink -f "$dev/driver")")"
    else
        warn "  $(basename "$dev") has NO driver bound"
    fi
done
[ "$found" -eq 1 ] || warn "No ROG Ally HID interfaces found - is the controller connected?"

log "Uninstallation complete!"
warn "Note: If you re-enabled read-only mode manually, remember to lock it if desired: sudo steamos-readonly enable"
