How to rebuild the module (run these from the ROG Ally)

```
cd /home/deck/staging/ally_module
make clean

# This will use the newly installed system headers!
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

zstd -f hid-asus-ally.ko
sudo cp hid-asus-ally.ko.zst /lib/modules/$(uname -r)/kernel/drivers/hid/
sudo depmod -a
sudo modprobe hid-asus-ally
```