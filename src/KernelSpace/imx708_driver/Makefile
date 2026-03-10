obj-m += imx708_custom.o

KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	sudo modprobe regmap-i2c
	-sudo rmmod imx708_custom
	-echo "10-001a" | sudo tee /sys/bus/i2c/drivers/imx708/unbind
	sudo insmod imx708_custom.ko

uninstall:
	sudo rmmod imx708_custom
	-echo "10-001a" | sudo tee /sys/bus/i2c/drivers/imx708/bind

reload: uninstall install