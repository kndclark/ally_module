KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

LINUXINCLUDE := -I$(PWD)/include $(LINUXINCLUDE)

subdir-ccflags-y := -I$(src)/include -I$(src)/sound/soc/amd -I$(src)/sound/soc/codecs -I$(src)/sound/soc/amd/acp -I$(src)/sound/soc/sdw_utils -I$(src)/sound/soc/amd/ps
ccflags-y := $(subdir-ccflags-y)

obj-m += snd-acp-config.o
snd-acp-config-y := sound/soc/amd/acp-config.o

obj-m += snd-soc-acpi-amd-match.o
snd-soc-acpi-amd-match-y := sound/soc/amd/acp/amd-acp63-acpi-match.o sound/soc/amd/acp/amd-acp70-acpi-match.o sound/soc/amd/acp/soc-acpi-amd-sdca-quirks.o

obj-m += snd-soc-sdw-utils.o
snd-soc-sdw-utils-y := sound/soc/sdw_utils/soc_sdw_utils.o

obj-m += snd-soc-tas2783-sdw.o
snd-soc-tas2783-sdw-y := sound/soc/codecs/tas2783-sdw.o

obj-m += snd-soc-rt721-sdca.o
snd-soc-rt721-sdca-y := sound/soc/codecs/rt721-sdca.o sound/soc/codecs/rt721-sdca-sdw.o

obj-m += snd-acp-sdw-legacy-mach.o
snd-acp-sdw-legacy-mach-y := sound/soc/amd/acp/acp-sdw-mach-common.o sound/soc/amd/acp/acp-sdw-legacy-mach.o

obj-m += snd-pci-ps.o
snd-pci-ps-y := sound/soc/amd/ps/pci-ps.o sound/soc/amd/ps/ps-common.o

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
