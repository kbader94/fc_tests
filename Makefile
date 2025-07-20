KDIR := /lib/modules/$(shell uname -r)/build

obj-m := uart_probe.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install: all
	@sudo rmmod uart_probe 2>/dev/null || true
	@sudo insmod ./uart_probe.ko
	@echo "uart_probe installed"