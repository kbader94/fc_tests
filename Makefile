obj-m += uart_probe.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean


install: all
	@sudo rmmod uart_probe 2>/dev/null || true
	@sudo insmod ./uart_probe.ko
	@echo "uart_probe installed"