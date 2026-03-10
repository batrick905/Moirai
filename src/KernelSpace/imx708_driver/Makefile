obj-m += imx708_custom.o

KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	sudo modprobe regmap-i2c
	-sudo rmmod imx708
	-sudo rmmod imx708_custom
	sudo insmod imx708_custom.ko

uninstall:
	sudo rmmod imx708_custom
	sudo modprobe imx708

reload: uninstall install