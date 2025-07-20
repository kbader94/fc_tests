// SPDX-License-Identifier: GPL-2.0
/*
 * uart_probe.c - DebugFS interface for UART FIFO probing
 *
 * Copyright (C) 2025 Kyle L. Bader
 *
 * This module allows probing of UART FIFO sizes and levels
 * by utilizing internal loopback mode to physically test
 * the current configuration. It currently supports
 * serial devices compatible with the 8250 core and is intended for driver
 * development and diagnostics of fifo_control.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial_core.h>
#include <linux/serial_8250.h>
#include <linux/uaccess.h>
#include <linux/delay.h>

#define FIFO_SIZE_MAX 512

static struct dentry *dir_entry;
static struct dentry *dev_entry;
static struct dentry *tx_fifo_entry;
static struct dentry *tx_trig_entry;
static struct dentry *rx_fifo_entry;
static struct dentry *rx_trig_entry;
static char selected_dev[16] = "ttyS0";

/* uart_probe/select_dev
 * Select serial device for testing 
 * eg. ttyS1
 */
static ssize_t select_dev_write(struct file *file,
                                 const char __user *buf,
                                 size_t count, loff_t *ppos)
{
    if (!count || count >= sizeof(selected_dev))
        return -EINVAL;

    if (copy_from_user(selected_dev, buf, count))
        return -EFAULT;

    selected_dev[strcspn(selected_dev, "\n")] = 0;

    pr_info("uart_rx_trig_test: selected TTY device is now: %s\n", selected_dev);
    return count;
}

static ssize_t select_dev_read(struct file *file, char __user *buf,
                               size_t count, loff_t *ppos)
{
	char tmp[32];
	int len = snprintf(tmp, sizeof(tmp), "%s\n", selected_dev);
	return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static const struct file_operations select_dev_fops = {
    .write = select_dev_write,
	.read = select_dev_read,
};

/* uart_probe/rx_trig_test
 * Probe the serial devices RX FIFO trigger level
 * by setting internal loopback and sending data to itself,
 * one byte at a time,
 * until the rx interrupt is triggered 
 * @returns RX FIFO trigger level in number of bytes
 */
static ssize_t rx_trig_probe_read(struct file *file, char __user *buf,
                                  size_t count, loff_t *ppos)
{
	char tmp[128];
	struct tty_driver *driver;
	struct tty_port *tport;
	struct uart_port *port;
	struct uart_state *state;
	struct uart_8250_port *u8250p;
	int line = 0;
	int trig = 0;
	int ret = 0;
	unsigned char old_fcr, old_mcr, old_lcr, iir;
	u32 old_dl;

	pr_info("uart_probe: starting RX trigger probe\n");

	driver = tty_find_polling_driver(selected_dev, &line);
	if (!driver) {
		pr_err("uart_probe: tty_find_polling_driver failed\n");
		return -ENODEV;
	}

	tport = driver->ports[line];
	if (!tport) {
		pr_err("uart_probe: no tty_port found for line %d\n", line);
		return -ENODEV;
	}

	state = container_of(tport, struct uart_state, port);
	port = state->uart_port;

	if (!port || !port->serial_in || !port->serial_out) {
		pr_err("uart_probe: invalid port or missing ops\n");
		return -ENODEV;
	}

	u8250p = up_to_u8250p(port);
	if (!u8250p) {
		pr_err("uart_probe: Not an 8250-based UART\n");
		return -ENODEV;
	}

	if (tty_port_initialized(tport) && tty_port_users(tport) > 0) {
		pr_err("uart_probe: TTY device %s is busy or opened by userspace\n", selected_dev);
		return -EBUSY;
	}

	mutex_lock(&tport->mutex);

	/* Store current port config */
	old_lcr = port->serial_in(port, UART_LCR);
	old_fcr = u8250p->fcr;
	old_mcr = port->serial_in(port, UART_MCR);

	/* Enable and clear FIFO */
	port->serial_out(port, UART_FCR,
	                 old_fcr | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);

	/* Enable loopback */
	port->serial_out(port, UART_MCR, old_mcr | UART_MCR_LOOP);

	/* Set baud to 115200 */
	port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
	old_dl = port->serial_in(port, UART_DLL) |
	         (port->serial_in(port, UART_DLM) << 8);
	port->serial_out(port, UART_DLL, 1);
	port->serial_out(port, UART_DLM, 0);
	port->serial_out(port, UART_LCR, UART_LCR_WLEN8);

	/* Enable RX interrupts */
	port->serial_out(port, UART_IER, UART_IER_RDI);

	/* Probe for trigger threshold */
	for (trig = 1; trig < 256; trig++) {
		port->serial_out(port, UART_TX, 0x55);

		/* Wait for byte transmission */ 
		udelay(100); /* 1 byte @ 115200 bps = ~87us */
		iir = port->serial_in(port, UART_IIR);

		if (!(iir & 0x01) && ((iir & 0x0E) == UART_IIR_RDI)) {
			break;
		}
	}

	/* Disable interrupts */
	port->serial_out(port, UART_IER, 0x00);

	/* Drain RX FIFO */
	while (port->serial_in(port, UART_LSR) & UART_LSR_DR) 
		port->serial_in(port, UART_RX);

	/* Restore prior port config */
	port->serial_out(port, UART_FCR, old_fcr);
	port->serial_out(port, UART_MCR, old_mcr);
	port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
	port->serial_out(port, UART_DLL, old_dl & 0xff);
	port->serial_out(port, UART_DLM, old_dl >> 8);
	port->serial_out(port, UART_LCR, old_lcr);

	mutex_unlock(&tport->mutex);

	if (trig >= 256) {
		pr_err("uart_probe: RX trigger test failed — no interrupt detected\n");
		ret = snprintf(tmp, sizeof(tmp), "RX trigger test failed\n");
	} else {
		ret = snprintf(tmp, sizeof(tmp), "%d\n", trig);
	}

	return simple_read_from_buffer(buf, count, ppos, tmp, ret);
}

static const struct file_operations rx_trig_fops = {
	.read = rx_trig_probe_read,
	.llseek = default_llseek,
};

/* uart_probe/rx_fifo_size
 * Probe the RX FIFO size by setting internal loopback,
 * transmitting data to itself, one byte at a time
 * and detecting rx overrun
 * @returns the size of th RX FIFO in number of bytes
 */
static ssize_t rx_fifo_size_read(struct file *file, char __user *buf,
                                 size_t count, loff_t *ppos)
{
	char tmp[128];
	struct tty_driver *driver;
	struct tty_port *tport;
	struct uart_port *port;
	struct uart_state *state;
	struct uart_8250_port *u8250p;
	unsigned char old_fcr, old_mcr, old_lcr, lsr;
	u32 old_dl;
    int line, count_tx, rx_fifo_size = 0;

    pr_info("uart_probe: starting RX size probe\n");

	driver = tty_find_polling_driver(selected_dev, &line);
	if (!driver) {
		pr_err("uart_probe: tty_find_polling_driver failed\n");
		return -ENODEV;
	}

	tport = driver->ports[line];
	if (!tport) {
		pr_err("uart_probe: no tty_port found for line %d\n", line);
		return -ENODEV;
	}

	state = container_of(tport, struct uart_state, port);
	port = state->uart_port;

	if (!port || !port->serial_in || !port->serial_out) {
		pr_err("uart_probe: invalid port or missing ops\n");
		return -ENODEV;
	}

	u8250p = up_to_u8250p(port);
	if (!u8250p) {
		pr_err("uart_probe: Not an 8250-based UART\n");
		return -ENODEV;
	}

	if (tty_port_initialized(tport) && tty_port_users(tport) > 0) {
		pr_err("uart_probe: TTY device %s is busy or opened by userspace\n", selected_dev);
		return -EBUSY;
	}

    mutex_lock(&tport->mutex);

	/* Store current port config */
    old_lcr = port->serial_in(port, UART_LCR);
    old_fcr = u8250p->fcr;
    old_mcr = port->serial_in(port, UART_MCR);

	/* Enable FIFO and loopback */
    port->serial_out(port, UART_FCR, UART_FCR_ENABLE_FIFO |
                                    UART_FCR_CLEAR_RCVR |
                                    UART_FCR_CLEAR_XMIT);
    port->serial_out(port, UART_MCR, old_mcr | UART_MCR_LOOP);

	/* Set baud 115200 */
    port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
    old_dl = port->serial_in(port, UART_DLL) |
             (port->serial_in(port, UART_DLM) << 8);
    port->serial_out(port, UART_DLL, 1);
    port->serial_out(port, UART_DLM, 0);
    port->serial_out(port, UART_LCR, UART_LCR_WLEN8);

	/* Transmit one byte at a time and check for overrun */
    for (count_tx = 0; count_tx < FIFO_SIZE_MAX; count_tx++) {
        port->serial_out(port, UART_TX, 0xff);
        mdelay(1);

        lsr = port->serial_in(port, UART_LSR);
        if (lsr & UART_LSR_OE) {
            rx_fifo_size = count_tx;
            break;
        }
    }

	/* Restore initial port config */
    port->serial_out(port, UART_FCR, old_fcr);
    port->serial_out(port, UART_MCR, old_mcr);
    port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
    port->serial_out(port, UART_DLL, old_dl & 0xff);
    port->serial_out(port, UART_DLM, old_dl >> 8);
    port->serial_out(port, UART_LCR, old_lcr);
    mutex_unlock(&tport->mutex);

    int len = (rx_fifo_size == 0)
              ? snprintf(tmp, sizeof(tmp), "RX overflow not detected\n")
              : snprintf(tmp, sizeof(tmp), "%d\n", rx_fifo_size);
    return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static const struct file_operations rx_fifo_fops = {
    .read = rx_fifo_size_read,
    .llseek = default_llseek,
};

/* uart_probe/tx_fifo_size 
 * Probe the TX FIFO size by overrunning the THR
 * enabling loopback, and counting how many bytes
 * we received. This should match port->fifosize.
 * @returns TX FIFO size in number of bytes
 */
static ssize_t tx_fifo_size_read(struct file *file, char __user *buf,
                                 size_t count, loff_t *ppos)
{
    char tmp[128];
	struct tty_driver *driver;
	struct tty_port *tport;
	struct uart_port *port;
	struct uart_state *state;
	struct uart_8250_port *u8250p;
    unsigned long deadline;
    int line, i , rx_count, tx_count = 0;
	unsigned char old_fcr, old_mcr, old_lcr, lsr;
	u32 old_dl;

    pr_info("uart_probe: starting TX size probe\n");

	driver = tty_find_polling_driver(selected_dev, &line);
	if (!driver) {
		pr_err("uart_probe: tty_find_polling_driver failed\n");
		return -ENODEV;
	}

	tport = driver->ports[line];
	if (!tport) {
		pr_err("uart_probe: no tty_port found for line %d\n", line);
		return -ENODEV;
	}

	state = container_of(tport, struct uart_state, port);
	port = state->uart_port;

	if (!port || !port->serial_in || !port->serial_out) {
		pr_err("uart_probe: invalid port or missing ops\n");
		return -ENODEV;
	}

	u8250p = up_to_u8250p(port);
	if (!u8250p) {
		pr_err("uart_probe: Not an 8250-based UART\n");
		return -ENODEV;
	}

	if (tty_port_initialized(tport) && tty_port_users(tport) > 0) {
		pr_err("uart_probe: TTY device %s is busy or opened by userspace\n", selected_dev);
		return -EBUSY;
	}

    mutex_lock(&tport->mutex);

	/* Store initial port config */
    old_lcr = port->serial_in(port, UART_LCR);
    old_fcr = u8250p->fcr;
    old_mcr = port->serial_in(port, UART_MCR);

	/* Enable FIFO and Loopback */
    port->serial_out(port, UART_FCR, UART_FCR_ENABLE_FIFO |
                                    UART_FCR_CLEAR_RCVR |
                                    UART_FCR_CLEAR_XMIT);
    port->serial_out(port, UART_MCR, old_mcr | UART_MCR_LOOP);

	/* Set baud 115200 */
    port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
    old_dl = port->serial_in(port, UART_DLL) |
             (port->serial_in(port, UART_DLM) << 8);
    port->serial_out(port, UART_DLL, 1);
    port->serial_out(port, UART_DLM, 0);
    port->serial_out(port, UART_LCR, UART_LCR_WLEN8);
	
	/* Fill TX FIFO */
    for (i = 0; i < FIFO_SIZE_MAX; i++) {
        port->serial_out(port, UART_TX, 0xFF);
        tx_count++;
    }

	mdelay(50);

	/* Count how many bytes we received */
    deadline = jiffies + msecs_to_jiffies(500);
    while (time_before(jiffies, deadline) && rx_count < tx_count) {
        lsr = port->serial_in(port, UART_LSR);
        if (lsr & UART_LSR_DR) {
            unsigned char val = port->serial_in(port, UART_RX);
            if (val == 0xFF)
                rx_count++;
        } else {
            cpu_relax();
        }
    }

	/* Restore port config */
    port->serial_out(port, UART_FCR, old_fcr);
    port->serial_out(port, UART_MCR, old_mcr);
    port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
    port->serial_out(port, UART_DLL, old_dl & 0xff);
    port->serial_out(port, UART_DLM, old_dl >> 8);
    port->serial_out(port, UART_LCR, old_lcr);

    mutex_unlock(&tport->mutex);

	if (rx_count == 0)
		return simple_read_from_buffer(buf, count, ppos, 
					"TX loopback failed or no data received\n", 42);
	else {
		int strlen = scnprintf(tmp, sizeof(tmp), "%d\n", rx_count);
		return simple_read_from_buffer(buf, count, ppos, tmp, strlen);
	}
   
}

static const struct file_operations tx_fifo_fops = {
    .read = tx_fifo_size_read,
    .llseek = default_llseek,
};

/* uart_probe/tx_trig_level
 * Probe the trigger level of the TX FIFO
 * by enabling loopback, filling the TX FIFO, 
 * and counting how many bytes until THRI interrupt is set
 * NOTE: we explicitly cap TX size to port->fifosize
 * otherwise serial_out will drop & transmit
 * randomly while it's full.
 * @returns TX FIFO trigger level in number of bytes
 */
static ssize_t tx_trig_probe_read(struct file *file, char __user *buf,
                                  size_t count, loff_t *ppos)
{
    char tmp[128];
	struct tty_driver *driver;
	struct tty_port *tport;
	struct uart_port *port;
	struct uart_state *state;
	struct uart_8250_port *u8250p;
    unsigned long deadline;
    int line, trig, rx_count = 0;
	unsigned char old_fcr, old_mcr, old_lcr, old_ier, lsr, iir;
	u32 old_dl;

    pr_info("uart_probe: starting TX size probe\n");

	driver = tty_find_polling_driver(selected_dev, &line);
	if (!driver) {
		pr_err("uart_probe: tty_find_polling_driver failed\n");
		return -ENODEV;
	}

	tport = driver->ports[line];
	if (!tport) {
		pr_err("uart_probe: no tty_port found for line %d\n", line);
		return -ENODEV;
	}

	state = container_of(tport, struct uart_state, port);
	port = state->uart_port;

	if (!port || !port->serial_in || !port->serial_out) {
		pr_err("uart_probe: invalid port or missing ops\n");
		return -ENODEV;
	}

	u8250p = up_to_u8250p(port);
	if (!u8250p) {
		pr_err("uart_probe: Not an 8250-based UART\n");
		return -ENODEV;
	}

	if (tty_port_initialized(tport) && tty_port_users(tport) > 0) {
		pr_err("uart_probe: TTY device %s is busy or opened by userspace\n", selected_dev);
		return -EBUSY;
	}

	mutex_lock(&tport->mutex);

	/* Store initial port config */
	old_lcr = port->serial_in(port, UART_LCR);
	port->serial_out(port, UART_LCR, 0x00);
	old_fcr = port->serial_in(port, UART_FCR);
	old_mcr = port->serial_in(port, UART_MCR);
	old_ier = port->serial_in(port, UART_IER);

	/* Enable FIFO & loopback */
	port->serial_out(port, UART_FCR, UART_FCR_ENABLE_FIFO |
	                                UART_FCR_CLEAR_RCVR |
	                                UART_FCR_CLEAR_XMIT |
	                                UART_FCR_TRIGGER_1);
	port->serial_out(port, UART_MCR, old_mcr | UART_MCR_LOOP);

	/* Drain RX FIFO */
	while (port->serial_in(port, UART_LSR) & UART_LSR_DR)
		port->serial_in(port, UART_RX);

	/* Set baud rate 115200 */
	port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
	old_dl = port->serial_in(port, UART_DLL) |
	        	(port->serial_in(port, UART_DLM) << 8);
	port->serial_out(port, UART_DLL, 1);
	port->serial_out(port, UART_DLM, 0);
	port->serial_out(port, UART_LCR, UART_LCR_WLEN8);

	/* Enable Transmission Hold Register Empty Interrupt */
	port->serial_out(port, UART_IER, UART_IER_THRI);

	/* Fill THR, but don't overfill it!  */
	for (int i = 0; i <= port->fifosize; i++)
		port->serial_out(port, UART_TX, 0xFF);

	/* Count how many bytes we rx until THR is empty */
	deadline = jiffies + msecs_to_jiffies(1500);
	while (time_before(jiffies, deadline)) {
		lsr = port->serial_in(port, UART_LSR);
		if (lsr & UART_LSR_DR) {
			port->serial_in(port, UART_RX);
			rx_count++;
		}

		iir = port->serial_in(port, UART_IIR);
		if (!(iir & UART_IIR_NO_INT) && (iir & 0x0E) == UART_IIR_THRI) {
			trig = rx_count;
			break;
		}

		cpu_relax();
	}

	/* Restore IER */
	port->serial_out(port, UART_IER, old_ier);

	/* Drain RX FIFO just in case */
	while (port->serial_in(port, UART_LSR) & UART_LSR_DR)
		port->serial_in(port, UART_RX);

	/* Restore registers */
	port->serial_out(port, UART_FCR, old_fcr);
	port->serial_out(port, UART_MCR, old_mcr);
	port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
	port->serial_out(port, UART_DLL, old_dl & 0xff);
	port->serial_out(port, UART_DLM, old_dl >> 8);
	port->serial_out(port, UART_LCR, old_lcr);

	mutex_unlock(&tport->mutex);


	if (trig == 0)
		return simple_read_from_buffer(buf, count, ppos, 
					"TX loopback failed or no data received\n", 42);
	else {
		int len = scnprintf(tmp, sizeof(tmp), "%d\n", trig);
		return simple_read_from_buffer(buf, count, ppos, tmp, len);
	}

}

static const struct file_operations tx_trig_fops = {
	.read = tx_trig_probe_read,
	.llseek = default_llseek,
};

static int __init uart_probe_debugfs_init(void)
{
    dir_entry = debugfs_create_dir("uart_probe", NULL);
    if (!dir_entry)
        return -ENOMEM;

    dev_entry = debugfs_create_file("select_dev", 0666, dir_entry, NULL, &select_dev_fops);
    tx_fifo_entry = debugfs_create_file("tx_fifo_size", 0444, dir_entry, NULL, &tx_fifo_fops);
	tx_trig_entry = debugfs_create_file("tx_trig_level", 0444, dir_entry, NULL, &tx_trig_fops);
	rx_fifo_entry = debugfs_create_file("rx_fifo_size", 0444, dir_entry, NULL, &rx_fifo_fops);
    rx_trig_entry = debugfs_create_file("rx_trig_level", 0444, dir_entry, NULL, &rx_trig_fops);

    if (!dev_entry || !rx_fifo_entry || !rx_trig_entry) {
        debugfs_remove_recursive(dir_entry);
        return -ENOMEM;
    }

    pr_info("uart_probe: loaded\n");
    return 0;
}

static void __exit uart_probe_debugfs_exit(void)
{
	debugfs_remove_recursive(dir_entry);
	pr_info("uart_probe: unloaded\n");
}

module_init(uart_probe_debugfs_init);
module_exit(uart_probe_debugfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kyle L. Bader");
MODULE_DESCRIPTION("DebugFS interface for probing UART config");
