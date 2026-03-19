obj-m += servo_driver.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

load: all
	sudo insmod servo_driver.ko

unload:
	sudo rmmod servo_driver || true

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all load unload clean
