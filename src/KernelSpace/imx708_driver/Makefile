obj-m += imx708_custom.o

KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	sudo modprobe regmap-i2c
	sudo modprobe v4l2-common
	sudo modprobe videobuf2-v4l2
	sudo modprobe v4l2-async
	sudo insmod imx708_custom.ko

uninstall:
	sudo rmmod imx708_custom

reload: uninstall install