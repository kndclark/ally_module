#include <linux/module.h>
#include "asus-wmi.h"

int asus_hid_register_listener(struct asus_hid_listener *cdev) { return 0; }
EXPORT_SYMBOL_GPL(asus_hid_register_listener);

void asus_hid_unregister_listener(struct asus_hid_listener *cdev) {}
EXPORT_SYMBOL_GPL(asus_hid_unregister_listener);

int asus_hid_event(enum asus_hid_event event) { return 0; }
EXPORT_SYMBOL_GPL(asus_hid_event);

void set_ally_mcu_hack(enum asus_ally_mcu_hack status) {}
EXPORT_SYMBOL_GPL(set_ally_mcu_hack);

void set_ally_mcu_powersave(bool enabled) {}
EXPORT_SYMBOL_GPL(set_ally_mcu_powersave);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Stub to satisfy hid-asus dependencies on older SteamOS kernels");
