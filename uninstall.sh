#!/bin/bash
# Restore the hid-asus.ko.zst that install.sh displaced and drop the LED class.
set -euo pipefail
KVER=$(uname -r)
DEST="/lib/modules/${KVER}/updates/ally/drivers"
HID_DST="${DEST}/hid/hid-asus.ko.zst"
BACKUP="${HID_DST}.bak-ally_module"

[ -f /usr/bin/steamos-readonly ] && sudo steamos-readonly disable
if [ -f "$BACKUP" ]; then
    sudo mv -f "$BACKUP" "$HID_DST"
    echo "[dynlight] restored previous hid-asus.ko.zst"
else
    sudo rm -f "$HID_DST"
    echo "[dynlight] no backup found; removed our hid-asus.ko.zst (run ally_module/install.sh to rebuild yours)"
fi
sudo rm -f "${DEST}/leds/led-class-dynamic.ko.zst" /etc/modprobe.d/ally-dynlight.conf
sudo depmod -a "$KVER"
sudo sync -f "$DEST"   # this filesystem only: a global sync waits on the SD card too
echo "[dynlight] files restored and synced; reboot to switch back (no hot-unload: it crashed this kernel once)."
