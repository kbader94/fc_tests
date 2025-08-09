#!/bin/bash
set -euo pipefail

DEBUGFS_BASE="/sys/kernel/debug/uart_probe"

# Ensure headers (optional for runtime but keeps your build step helpful)
if [ ! -d "/lib/modules/$(uname -r)/build" ]; then
  echo "Kernel headers not found for $(uname -r)."
  exit 1
fi

# Ensure debugfs is mounted
mountpoint -q /sys/kernel/debug || sudo mount -t debugfs none /sys/kernel/debug

# Build & install (assumes your Makefile's install does insmod)
make -s clean
make -s
sudo make -s install

# Sanity check
if ! sudo test -d "$DEBUGFS_BASE"; then
  echo "DebugFS path '$DEBUGFS_BASE' not found (module not loaded?)."
  exit 1
fi

echo "[+] Probing UARTs..."

# Gather candidate TTYs (add more patterns if needed)
cands=(/dev/ttyS*)
if [ ${#cands[@]} -eq 0 ]; then
  echo "  (no ttyS* devices found on this system)"
fi

for dev_path in "${cands[@]}"; do
    dev=$(basename "$dev_path")
    # Skip busy devices
    if fuser -s "$dev_path"; then
        echo "  - $dev: skip (busy)"
        continue
    fi

    # Select the device in debugfs
    echo -n "$dev" | sudo tee "$DEBUGFS_BASE/select_dev" >/dev/null

    echo "  * $dev"
    for test in rx_fifo_size tx_fifo_size rx_trig_level tx_trig_level; do
        tp="$DEBUGFS_BASE/$test"
        if out=$(sudo cat "$tp" 2>&1); then
            echo "     - $test: $out"
        else
            if [[ "$test" == "tx_trig_level" ]]; then
            	echo "     - N/A"
            fi
           #  echo "    - $test: [error] $out"
        fi
    done
done

echo "[+] Done."

