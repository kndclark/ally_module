# Out-of-tree build of OGC linux-unstable PR #17 (Dynamic Lighting LED class
# + hid-asus Aura/LampArray) for the SteamOS 7.2.x kernel on the ROG Ally.
#
# hid-asus.c, hid-ids.h and led-class-dynamic.c are the PR files; only
# led-class-dynamic.c and the header carry a marked shim for the
# led_classdev back-pointer, which an out-of-tree build cannot add to the
# kernel's struct. See dynlight_compat.h for the two missing defines.

KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m += led-class-dynamic.o
obj-m += hid-asus.o

# CONFIG_LEDS_CLASS_DYNAMIC is not in the kernel .config; tell the PR code
# the class is reachable as a module.
ccflags-y += -I$(src)/include
ccflags-y += -include $(src)/dynlight_compat.h
ccflags-y += -DCONFIG_LEDS_CLASS_DYNAMIC_MODULE=1

# asus-wmi-stub exists only for kernels whose asus-wmi does not export the
# hid-asus listener API (SteamOS 7.2 does). Decide per kernel.
STUB_NEEDED := $(shell grep -qE '[[:space:]]asus_hid_register_listener[[:space:]]' \
	$(KDIR)/Module.symvers 2>/dev/null && echo no || echo yes)
ifeq ($(STUB_NEEDED),yes)
obj-m += asus-wmi-stub.o
endif

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules CONFIG_DEBUG_INFO_BTF_MODULES=

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
