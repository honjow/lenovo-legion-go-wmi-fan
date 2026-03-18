# SPDX-License-Identifier: GPL-2.0

obj-m := lenovo-legion-wmi-fan.o

KVER    ?= $(shell uname -r)
KERNELDIR ?= /lib/modules/$(KVER)/build
PWD     := $(shell pwd)

.PHONY: all clean install uninstall

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
	depmod -a $(KVER)

uninstall:
	rm -f /lib/modules/$(KVER)/extra/lenovo-legion-wmi-fan.ko
	depmod -a $(KVER)
