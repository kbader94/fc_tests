KDIR := /lib/modules/$(shell uname -r)/build
obj-m := uart_probe.o

USER_PROGRAM := rtt_test
USER_SOURCE := rtt_test.c

all: $(USER_PROGRAM)
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

$(USER_PROGRAM): $(USER_SOURCE)
	$(CC) -Wall -O2 -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(RM) $(USER_PROGRAM)

install: all
	@sudo rmmod uart_probe 2>/dev/null || true
	@sudo insmod ./uart_probe.ko
	@echo "uart_probe installed"

