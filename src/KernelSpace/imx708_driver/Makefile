obj-m += imx708_custom.o

KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	sudo insmod imx708_custom.ko

uninstall:
	sudo rmmod imx708_custom

reload: uninstall install