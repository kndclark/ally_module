#!/bin/bash
set -e

MODULE_NAME="hid-asus-ally"
INSTALL_PATH="/lib/modules/$(uname -r)/kernel/drivers/hid/"
# Get the actual user if running via sudo
TARGET_USER="${SUDO_USER:-$(whoami)}"

log() { echo -e "\033[0;32m[INFO]\033[0m $1"; }
error() { echo -e "\033[0;31m[ERROR]\033[0m $1"; exit 1; }

if [ "$EUID" -ne 0 ]; then
    error "Please run this script with sudo: sudo ./install-ally.sh"
fi

# Check if user password is set (required for pacman/sudo)
PW_STATUS=$(passwd --status "$TARGET_USER" | awk '{print $2}')
if [ "$PW_STATUS" != "P" ]; then
    log "--------------------------------------------------------"
    warn "User '$TARGET_USER' does not appear to have a password set."
    warn "SteamOS requires a user password for 'sudo' and 'pacman' operations."
    warn "If you haven't set one, run 'passwd' in a new terminal first."
    log "--------------------------------------------------------"
    read -p "Continue anyway? (y/N) " confirm
    if [[ ! $confirm =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# --- 2. Unlock Filesystem ---

if [ -f /usr/bin/steamos-readonly ]; then
    log "Ensuring filesystem is writeable..."
    steamos-readonly disable
fi

# --- 3. Initialize Keys ---

KEYRING_DB="/etc/pacman.d/gnupg/pubring.kbx"
if [ ! -d /etc/pacman.d/gnupg ] || [ ! -f "$KEYRING_DB" ] || [ $(stat -c%s "$KEYRING_DB" 2>/dev/null || echo 0) -le 32 ]; then
    log "Initializing/Populating pacman keys (this may take a minute)..."
    rm -f /etc/pacman.d/gnupg/*.lock 2>/dev/null
    pacman-key --init
    pacman-key --populate archlinux holo
fi

# --- 4. Install Dependencies ---

log "Checking dependencies..."
KERNEL_VER=$(uname -r)
HEADER_SUFFIX=$(echo "$KERNEL_VER" | grep -o 'neptune-[0-9]*' || echo "")
if [ -n "$HEADER_SUFFIX" ]; then
    HEADER_PKG="linux-${HEADER_SUFFIX}-headers"
else
    HEADER_PKG="linux-headers"
fi
log "Detected required headers: $HEADER_PKG"
pacman -Sy --needed --noconfirm base-devel "$HEADER_PKG" zstd

# --- 5. Build and Install ---

log "Building $MODULE_NAME..."
make clean
make all

if [ ! -f "${MODULE_NAME}.ko" ]; then
    error "Build failed!"
fi

log "Compressing module..."
zstd -f "${MODULE_NAME}.ko"

log "Checking for conflicting hid-asus changes..."
# If hid-asus was replaced by our unified driver, we should ideally restore it
# or at least warn the user.
ASUS_BACKUP="${INSTALL_PATH}/hid-asus.ko.zst.bak"
if [ -f "$ASUS_BACKUP" ]; then
    log "Restoring original hid-asus to avoid conflicts..."
    cp -f "$ASUS_BACKUP" "${INSTALL_PATH}/hid-asus.ko.zst"
fi

log "Installing $MODULE_NAME to $INSTALL_PATH..."
cp -f "${MODULE_NAME}.ko.zst" "$INSTALL_PATH"

log "Updating dependency map..."
depmod -a

log "Reloading modules..."
modprobe -r hid-asus || true
modprobe -r hid-asus-ally || true
modprobe hid-asus
modprobe hid-asus-ally

log "Verifying installation..."
if lsmod | grep -q "hid_asus_ally"; then
    log "Module '$MODULE_NAME' loaded successfully."
else
    error "Module failed to load."
fi

log "Installation complete!"
