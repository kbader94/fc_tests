# `uart_probe.sh` — User Guide

This script builds/installs the `` kernel module and runs a set of **FIFO/trigger-level probes** against 8250-style UARTs exposed as `/dev/ttyS*`. It can also run a tiny user-space **round‑trip time (RTT)** test.

---

## What it does

1. Ensures kernel headers exist and **mounts debugfs** (`/sys/kernel/debug`) if needed.
2. `make clean && make && sudo make install` to build/install the `uart_probe` module.
3. Uses **/proc** to find **real, initialized** UARTs (skips ghost nodes with no hardware).
4. For each selected TTY:
   - Writes the device name into `debugfs:/sys/kernel/debug/uart_probe/select_dev`.
   - Reads **RX/TX trigger levels** and **FIFO sizes** from the module’s debugfs nodes:
     - `rx_trig_level`, `rx_fifo_size`, `tx_trig_level`, `tx_fifo_size`
   - Optionally runs the **userspace RTT** test (`./rtt_test`) if requested.
   - Optionally **sweeps** RX/TX trigger settings you provide and reports results.

> The debugfs files are created by the `uart_probe` kernel module you’re developing.

---

## Requirements

- **Kernel headers** for the running kernel: `/lib/modules/$(uname -r)/build`
- `sudo` privileges (to mount debugfs, install the module, and read/write sysfs/debugfs)
- A working `Makefile` for the `uart_probe` module (`make install` should load it)
- Optional: `./rtt_test` binary in the same directory if you use `-u/--rtt`
- Permission to open serial devices (be in the **dialout** group on many distros, or run with `sudo`)

---

## Usage

```bash
./uart_probe.sh [-d /dev/ttySX] [-x] [-u] [-r 1,4,8,14] [-t 1,4,8,14]
```

### Options

- `-d, --device <DEVICE>`\
  Probe only the specified device. Examples: `-d /dev/ttyS0` or `-d ttyS0`.

- `-x, --disable-fifo`\
  Intent: test with FIFO disabled (16450 mode). Whether this has an effect depends on your driver/sysfs support. (If not wired up yet, this is a no-op.)

- `-u, --rtt`\
  Run the **userspace RTT** test (`./rtt_test`) after probing.

- `-r, --rx-trigger <LIST>`\
  Comma-separated RX trigger levels to set and test (e.g. `1,4,8,14`). When provided, the script will write each level (via your driver’s sysfs attribute, if available) and then run **RX‑relevant probes**.

- `-t, --tx-trigger <LIST>`\
  Comma-separated TX trigger levels to set and test (e.g. `1,4,8,14`). When provided, the script will write each level and then run **TX‑relevant probes**.

---

## Device selection & filtering

The script builds a device list:

- If `-d` is given: just that device.
- Else: all `/dev/ttyS*`.

Before probing each `ttyS<N>`, it checks **/proc/tty/driver/serial** (via `sudo`) for an initialized row:

- Accepts entries with `uart != unknown`, `irq > 0`, and a real I/O resource (`port != 00000000` or `mmio` present).
- Anything else is reported as **“skip (not initialized / no probed hardware)”**.

Busy devices are skipped as **“skip (busy)”**.

> This avoids wasting time on static device nodes that aren’t backed by hardware.

---

## Probes performed

For each selected device, the script talks to the **debugfs** nodes created by your module:

- `rx_trig_level` — **RX trigger** as measured by the kernel probe
- `rx_fifo_size` — **RX FIFO size** (measured)
- `tx_trig_level` — **TX trigger** (measured)
- `tx_fifo_size` — **TX FIFO size** (measured)

If you provided `-r` or `-t` lists, the script will **set** the corresponding sysfs attributes on `/sys/class/tty/<dev>/…` (your driver must expose these) before reading back the debugfs probe result.

If `-u/--rtt` is used, the script also runs `./rtt_test <device>` and prints:

- `RTT: <microseconds>` on success
- Or an error such as `Timeout waiting for response.`, `open: Permission denied`, etc.

---

## Important behavior & caveats

### Opening the device resets FIFO/trigger configuration

On most 8250-style drivers:

- **Open (**``**)** → TTY core calls `uart_startup()` → driver reprograms UART (**EFR[ECB] cleared**, triggers to defaults, etc.).
- `` also causes the driver’s `->set_termios()` to reapply defaults.

**Implications:**

- Running `` (or any userspace app) **after** you set triggers may revert them.
- If you want a specific trigger/FIFO mode for the RTT, **reapply** your sysfs settings **after the device is opened** and **before** timing (the sweep options handle this; document for manual runs).

### Permissions

- Debugfs and driver sysfs attributes usually require **root**.
- Opening `/dev/ttyS*` requires membership in the appropriate group (e.g., **dialout**) or using `sudo`.

### Module presence

If `/sys/kernel/debug/uart_probe` isn’t present after build/install, the module didn’t load. Check `dmesg` for reasons.

---

## Sample output (trimmed)

```
uart_probe installed
[+] Probing UARTs...
  - ttyS1: skip (not initialized / no probed hardware)
  * ttyS4
     - rx_trig_level: 32
     - rx_fifo_size:  253
     - RTT: 505.00 microseconds
     - tx_trig_level: 1
     - tx_fifo_size:  253
[+] Done.
```

Common error lines and meaning:

- `cat: …/rx_trig_level: No such device`\
  The kernel module rejected the selected device (e.g., not 8250‑based or not selected). Ensure you wrote the device name to `select_dev` and that the device is supported.

- `open: Permission denied` (from `rtt_test`)\
  Your user can’t open the TTY. Add your user to **dialout**, or run with `sudo`.

- `Timeout waiting for response.` (from `rtt_test`)\
  Loopback not enabled or no physical loopback path; check your probe’s loopback enable, wiring, or simulation.

---

## Examples

Probe a single device and run RTT:

```bash
./uart_probe.sh -d /dev/ttyS0 -u
```

Sweep RX triggers on ttyS4 only:

```bash
./uart_probe.sh -d ttyS4 -r 1,4,8,14
```

Sweep both RX and TX triggers on all initialized UARTs:

```bash
./uart_probe.sh -r 1,4,8,14 -t 1,4,8,14
```

Disable FIFO (if supported by your driver) and run RTT:

```bash
./uart_probe.sh -x -u
```

---

## Troubleshooting

- **“DebugFS path '/sys/kernel/debug/uart\_probe' not found”**\
  The module isn’t loaded. Rebuild; check `dmesg` for load errors.

- **All devices skipped**\
  `/proc/tty/driver/serial` may need root to read. The script uses `sudo` for that; ensure you can `sudo` without a TTY prompt (or run the whole script with `sudo`).

- **RTT seems to revert triggers**\
  Expected: opening the port resets configuration. Reapply your sysfs trigger settings **after open** (use the sweep options or a gated RTT flow).

---

## Notes for future you

- Long‑term robustness: Make your driver remember the configured trigger policy and reapply it in `->startup()` and `->set_termios()`.
- For pure hardware/driver timing (not user‑visible read latency), consider an **in‑kernel RTT** debugfs node to avoid userspace `open()` side effects.

---

## Appendix: Files and paths used

- **DebugFS (from **``** module)**\
  `/sys/kernel/debug/uart_probe/`\
  ├── `select_dev` (write: `ttyS<N>`)\
  ├── `rx_trig_level` (read measured level)\
  ├── `rx_fifo_size` (read measured size)\
  ├── `tx_trig_level` (read measured level)\
  └── `tx_fifo_size` (read measured size)

- **Driver sysfs (if your fifo\_control exposes them):**\
  `/sys/class/tty/ttyS<N>/rx_trig_bytes`\
  `/sys/class/tty/ttyS<N>/tx_trig_bytes`

- **Probe filtering:**\
  `/proc/tty/driver/serial` (read via `sudo`) to decide whether `ttyS<N>` is backed by initialized hardware.

