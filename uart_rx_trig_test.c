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

static struct dentry *dir_entry;
static struct dentry *trig_entry;
static struct dentry *dev_entry;
static char selected_tty[16] = "ttyS0";

/* uart_probe/select_dev
 * Select serial device for testing 
 * eg. ttyS1
 */
static ssize_t select_dev_write(struct file *file,
                                 const char __user *buf,
                                 size_t count, loff_t *ppos)
{
    if (!count || count >= sizeof(selected_tty))
        return -EINVAL;

    if (copy_from_user(selected_tty, buf, count))
        return -EFAULT;

    selected_tty[strcspn(selected_tty, "\n")] = 0;

    pr_info("uart_rx_trig_test: selected TTY device is now: %s\n", selected_tty);
    return count;
}

static ssize_t select_dev_read(struct file *file, char __user *buf,
                               size_t count, loff_t *ppos)
{
	char tmp[32];
	int len = snprintf(tmp, sizeof(tmp), "%s\n", selected_tty);
	return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static const struct file_operations select_dev_fops = {
    .write = select_dev_write,
	.read = select_dev_read,
};

/* uart_probe/rx_trig_test
 * Probe the serial devices RX FIFO trigger level
 * by setting internal loopback and sending data to itself
 * until the rx interrupt is triggered 
 * returns RX FIFO trigger level in number of bytes
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

	driver = tty_find_polling_driver(selected_tty, &line);
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

	mutex_lock(&tport->mutex);
	pr_info("uart_probe: locked tty_port mutex\n");

	/* Store current port config */
	old_lcr = port->serial_in(port, UART_LCR);
	old_fcr = u8250p->fcr;
	old_mcr = port->serial_in(port, UART_MCR);
	pr_info("uart_probe: old LCR=0x%02x FCR=0x%02x MCR=0x%02x\n", old_lcr, old_fcr, old_mcr);

	/* Enable and clear FIFO */
	port->serial_out(port, UART_FCR,
	                 old_fcr | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
	pr_info("uart_probe: enabled FIFO and cleared RX/TX\n");

	/* Enable loopback */
	port->serial_out(port, UART_MCR, old_mcr | UART_MCR_LOOP);
	pr_info("uart_probe: loopback mode enabled\n");

	/* Set baud to 115200 */
	port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
	old_dl = port->serial_in(port, UART_DLL) |
	         (port->serial_in(port, UART_DLM) << 8);
	port->serial_out(port, UART_DLL, 1);
	port->serial_out(port, UART_DLM, 0);
	port->serial_out(port, UART_LCR, UART_LCR_WLEN8);
	pr_info("uart_probe: baud set to 115200 (DL=1), old DL=0x%04x\n", old_dl);

	/* Enable RX interrupts */
	port->serial_out(port, UART_IER, UART_IER_RDI);
	pr_info("uart_probe: RX interrupt enabled\n");

	/* Probe for trigger threshold */
	for (trig = 1; trig < 256; trig++) {
		port->serial_out(port, UART_TX, 0x55);
		udelay(100);

		iir = port->serial_in(port, UART_IIR);
		pr_info("uart_probe: byte %d sent, IIR=0x%02x\n", trig, iir);

		if (!(iir & 0x01) && ((iir & 0x0E) == UART_IIR_RDI)) {
			pr_info("uart_probe: RX trigger interrupt detected at %d bytes\n", trig);
			break;
		}
	}

	/* Disable interrupts */
	port->serial_out(port, UART_IER, 0x00);
	pr_info("uart_probe: disabled IER\n");

	/* Drain RX FIFO */
	while (port->serial_in(port, UART_LSR) & UART_LSR_DR) {
		unsigned char val = port->serial_in(port, UART_RX);
		pr_info("uart_probe: drained RX byte: 0x%02x\n", val);
	}

	/* Restore prior port config */
	port->serial_out(port, UART_FCR, old_fcr);
	port->serial_out(port, UART_MCR, old_mcr);
	port->serial_out(port, UART_LCR, UART_LCR_CONF_MODE_A);
	port->serial_out(port, UART_DLL, old_dl & 0xff);
	port->serial_out(port, UART_DLM, old_dl >> 8);
	port->serial_out(port, UART_LCR, old_lcr);
	pr_info("uart_probe: restored original UART registers\n");

	mutex_unlock(&tport->mutex);
	pr_info("uart_probe: unlocked tty_port mutex\n");

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

static int __init rx_trig_debugfs_init(void)
{
	dir_entry = debugfs_create_dir("uart_probe", NULL);
	if (!dir_entry)
		return -ENOMEM;

	dev_entry = debugfs_create_file("select_dev", 0200, dir_entry,
									NULL, &select_dev_fops);

	trig_entry = debugfs_create_file("rx_trig_test", 0444, dir_entry,
	                                 NULL, &rx_trig_fops);

	if (!trig_entry || !dev_entry) {
		debugfs_remove_recursive(dir_entry);
		return -ENOMEM;
	}

	pr_info("uart_probe: loaded\n");
	return 0;
}

static void __exit rx_trig_debugfs_exit(void)
{
	debugfs_remove_recursive(dir_entry);
	pr_info("uart_probe: unloaded\n");
}

module_init(rx_trig_debugfs_init);
module_exit(rx_trig_debugfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kyle L. Bader");
MODULE_DESCRIPTION("DebugFS interface for probing UART RX trigger level");
