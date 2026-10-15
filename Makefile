obj-m := snd-dante-pcie.o

KDIR ?= /lib/modules/$(shell uname -r)/build
DKMS_NAME := snd-dante-pcie
DKMS_VERSION := 0.0.2

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

uninstall:
	rm -f /lib/modules/$(shell uname -r)/updates/dkms/snd-dante-pcie.ko*
	depmod -a

dkms-install:
	mkdir -p /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)
	cp -f snd-dante-pcie.c Makefile dkms.conf /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)/
	dkms add $(DKMS_NAME)/$(DKMS_VERSION)
	dkms build $(DKMS_NAME)/$(DKMS_VERSION)
	dkms install --force $(DKMS_NAME)/$(DKMS_VERSION)

dkms-uninstall:
	dkms remove $(DKMS_NAME)/$(DKMS_VERSION) --all
	rm -rf /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

.PHONY: all clean install uninstall dkms-install dkms-uninstall
