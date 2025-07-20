#!/bin/bash

set -euo pipefail

MODULE_NAME="uart_rx_trig_test"
DEBUGFS_BASE="/sys/kernel/debug/uart_probe"

# Ensure kernel headers are present
if [ ! -d /lib/modules/$(uname -r)/build ]; then
    echo "Kernel headers not found for kernel $(uname -r)"
    echo "Please install the correct kernel headers. "
    echo "Hint:"
    echo "Debian/Ubuntu: sudo apt install linux-headers-$(uname -r)"
    echo "Redhat/Fedora: sudo dnf install kernel-devel-$(uname -r)"
    echo "Arch/BTW: sudo pacman -S linux-headers"
    exit 1
fi

# Ensure debugfs is mounted
if ! mountpoint -q /sys/kernel/debug; then
    echo "[*] Mounting debugfs..."
    sudo mount -t debugfs none /sys/kernel/debug
else
    echo "[+] debugfs already mounted."
fi

# Build and install the module
echo "[+] Building and installing $MODULE_NAME..."
make clean
make && sudo make install

if ! sudo test -d "$DEBUGFS_BASE"; then
    echo "[-] Error: DebugFS path '$DEBUGFS_BASE' not found or inaccessible"
    exit 1
fi

# Run tests on serial devices
echo "[+] Probing UARTs..."

for dev_path in /sys/class/tty/ttyS*; do
    dev=$(basename "$dev_path")

    # Select device
    echo "$dev" | sudo tee "$DEBUGFS_BASE/select_dev" > /dev/null

    # Test tx_fifo_size; skip if ENODEV or any read error
    if ! output=$(cat "$DEBUGFS_BASE/tx_fifo_size" 2>/dev/null); then
        # silently skip device if tx_fifo_size returns error (e.g. ENODEV)
        continue
    fi

    echo "[*] Probing device: $dev"

    # Perform UART_probe tests
    for test in rx_fifo_size tx_fifo_size rx_trig_level tx_trig_level; do
        test_path="$DEBUGFS_BASE/$test"
        if [[ -f "$test_path" ]]; then
            result=$(cat "$test_path" 2>/dev/null)
            echo "  - $test: ${result:-[No data]}"
        else
            echo "  - $test: [Not available]"
        fi
    done

    echo
done

echo "[+] Done."
