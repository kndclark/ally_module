obj-m := asus-armoury.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	make -C $(KDIR) M=$(PWD) modules CONFIG_DEBUG_INFO_BTF_MODULES=
clean:
	make -C $(KDIR) M=$(PWD) clean
