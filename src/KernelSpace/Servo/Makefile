# Makefile for servo_driver LKM + user-space application
#
# Usage (on the Raspberry Pi 4):
#   make            – build both the kernel module and the app
#   make load       – insert the module
#   make unload     – remove the module
#   make demo       – run the demo application
#   make clean      – remove build artefacts

# ── Kernel module ────────────────────────────────────────────────────────
obj-m += servo_driver.o

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

# ── User-space app ───────────────────────────────────────────────────────
APP     = servo_app
APP_SRC = servo_app.c
CFLAGS  = -Wall -Wextra -O2

all: module app

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

app: $(APP_SRC)
	$(CC) $(CFLAGS) -o $(APP) $(APP_SRC) -lpthread

load: module
	sudo insmod servo_driver.ko
	@echo "Module loaded. Device node: $$(ls -l /dev/servo)"

unload:
	sudo rmmod servo_driver || true

reload: unload load

demo: app
	sudo ./$(APP) --demo

interactive: app
	sudo ./$(APP) --interactive

stats:
	@cat /proc/servo_stats 2>/dev/null || echo "/proc/servo_stats not found – is the module loaded?"

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(APP)

.PHONY: all module app load unload reload demo interactive stats clean
