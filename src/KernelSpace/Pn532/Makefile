obj-m += pn532_spi.o

# Point at your kernel build dir if cross-compiling, e.g.:
# KDIR=/lib/modules/$(shell uname -r)/build
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean