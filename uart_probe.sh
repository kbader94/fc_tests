#!/bin/bash
set -euo pipefail

DEBUGFS_BASE="/sys/kernel/debug/uart_probe"
DEVICE_ARG=""
DISABLE_FIFO_ARG=false
TEST_RTT_ARG=false
RX_TRIGGER=""
TX_TRIGGER=""
RX_LIST=()
TX_LIST=()

usage() {
    echo "Usage: $0 [-d /dev/ttySX]"
    echo "  -d, --device <DEVICE>   Probe only the specified UART device"
    echo "              e.g.: $0 -d /dev/ttyS1"
    echo "  -x, --disable-fifo   Disable FIFO, 1 byte FIFO depth, 16450 mode"
    echo "  -r, --rx-trigger <LEVEL> Comma separated list of RX trigger levels to test (1, 4, 8, 14)"
    echo "  -t, --tx-trigger <LEVEL>  Comma separated list of TX trigger levels to test (1, 4, 8, 14)"

    exit 1
}

# Parse args
OPTS=$(getopt -o hd:xr:t: --long help,device:,disable-fifo,rx-trigger:,tx-trigger: -n "$0" -- "$@")
eval set -- "$OPTS"

while true; do
    case "$1" in
        -d|--device) DEVICE_ARG="$2"; shift 2 ;;
        -x|--disable-fifo) DISABLE_FIFO_ARG=true; shift ;;
        -u|--rtt) TEST_RTT_ARG=true; shift ;;
        -r|--rx-trigger) RX_TRIGGER="$2"; shift 2 ;;
        -t|--tx-trigger) TX_TRIGGER="$2"; shift 2 ;;
        -h|--help) usage ;;
        --) shift; break ;;
    esac
done

# Helpers
is_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }
trim() { local s="$1"; s="${s#"${s%%[![:space:]]*}"}"; echo "${s%"${s##*[![:space:]]}"}"; }

parse_csv_to_array() {
  local csv="$1" ; local -n outarr=$2
  outarr=()
  IFS=',' read -ra parts <<< "$csv"
  for p in "${parts[@]}"; do
    p="$(trim "$p")"
    [[ -z "$p" ]] && continue
    if ! is_uint "$p"; then
      echo "Invalid numeric value in list: '$p'" >&2
      exit 2
    fi
    outarr+=("$p")
  done
}

# Parse RX and TX trigger levels
[[ -n "$RX_TRIGGER" ]] && parse_csv_to_array "$RX_TRIGGER" RX_LIST
[[ -n "$TX_TRIGGER" ]] && parse_csv_to_array "$TX_TRIGGER" TX_LIST

# Normalize device argument
if [[ -n "$DEVICE_ARG" && "$DEVICE_ARG" != /dev/* ]]; then
  DEVICE_ARG="/dev/$DEVICE_ARG"
fi

# Ensure headers
if [ ! -d "/lib/modules/$(uname -r)/build" ]; then
    echo "Kernel headers not found for $(uname -r)."
    exit 1
fi

# Ensure debugfs is mounted
mountpoint -q /sys/kernel/debug || sudo mount -t debugfs none /sys/kernel/debug

# Build & install
make -s clean
make -s
sudo make -s install

# Sanity check
if ! sudo test -d "$DEBUGFS_BASE"; then
    echo "DebugFS path '$DEBUGFS_BASE' not found (module not loaded?)."
    exit 1
fi

echo "[+] Probing UARTs..."

# Build device list
if [[ -n "$DEVICE_ARG" ]]; then
    if [[ ! -e "$DEVICE_ARG" ]]; then
        echo "Device $DEVICE_ARG not found"
        exit 1
    fi
    cands=("$DEVICE_ARG")
else
    shopt -s nullglob
    cands=(/dev/ttyS*)
    shopt -u nullglob
    if [ ${#cands[@]} -eq 0 ]; then
        echo "  (no ttyS* devices found on this system)"
        exit 0
    fi
fi

for dev_path in "${cands[@]}"; do
  dev=$(basename "$dev_path")
  if fuser -s "$dev_path"; then
    echo "  - $dev: skip (busy)"
    continue
  fi

  printf '%s\n' "$dev" | sudo tee "$DEBUGFS_BASE/select_dev" >/dev/null
  echo "  * $dev"

  fifo_base="/sys/class/tty/$dev"
  have_fifo=false
  sudo test -d "$fifo_base" && have_fifo=true

  # --- RX trigger sweep (if provided) ---
  if (( ${#RX_LIST[@]} )); then
    if ! $have_fifo || ! sudo test -w "$fifo_base/rx_trig_bytes"; then
      echo "     - rx_sweep: [fifo_control not available]"
    else
      for rx in "${RX_LIST[@]}"; do
        echo "$rx" | sudo tee "$fifo_base/rx_trig_bytes" >/dev/null
        # run only RX-relevant probes
        echo "  * $dev rx_trig_level set to $rx"
        if out=$(sudo cat "$DEBUGFS_BASE/rx_trig_level" 2>&1); then
          echo "     - rx_trig_level: $out"
        else
          echo "     - rx_trig_level (set=$rx): [error] $out"
        fi
        if out=$(./rtt_test "$dev_path" 2>&1); then
          rtt="$out"
        else
          rtt="[error] $out"
        fi
        echo "     - $rtt"
      done
      if out=$(sudo cat "$DEBUGFS_BASE/rx_fifo_size" 2>&1); then
        echo "     - rx_fifo_size: $out"
      else
        echo "     - rx_fifo_size: [error] $out"
      fi
    fi
  else
    # No RX list provided: run once with current setting
    if out=$(sudo cat "$DEBUGFS_BASE/rx_trig_level" 2>&1); then
      echo "     - rx_trig_level: $out"
    else
      echo "     - rx_trig_level: [error] $out"
    fi
    if out=$(sudo cat "$DEBUGFS_BASE/rx_fifo_size" 2>&1); then
      echo "     - rx_fifo_size:  $out"
    else
      echo "     - rx_fifo_size:  [error] $out"
    fi
    if out=$(./rtt_test "$dev_path" 2>&1); then
      rtt="$out"
    else
      rtt="[error] $out"
    fi
  fi

  # --- TX trigger sweep (if provided) ---
  if (( ${#TX_LIST[@]} )); then
    if ! $have_fifo || ! sudo test -w "$fifo_base/tx_trig_bytes"; then
      echo "     - tx_sweep: [tx_trig_bytes not available]"
    else
      for tx in "${TX_LIST[@]}"; do
        echo "$tx" | sudo tee "$fifo_base/tx_trig_bytes" >/dev/null
        # run only TX-relevant probes
        if out=$(sudo cat "$DEBUGFS_BASE/tx_trig_level" 2>&1); then
          echo "     - tx_trig_level (set=$tx): $out"
        else
          echo "     - tx_trig_level (set=$tx): [error] $out"
        fi
        rtt=$(./rtt_test "$dev_path" 2>&1)
        echo "     - $rtt"
      done
      if out=$(sudo cat "$DEBUGFS_BASE/tx_fifo_size" 2>&1); then
        echo "     - tx_fifo_size: $out"
      else
        echo "     - tx_fifo_size: [error] $out"
      fi
    fi
  else
    # No TX list provided: run once with current setting
    if out=$(sudo cat "$DEBUGFS_BASE/tx_trig_level" 2>&1); then
      echo "     - tx_trig_level: $out"
    else
      echo "     - tx_trig_level: N/A"  # keep your earlier behavior if desired
    fi
    if out=$(sudo cat "$DEBUGFS_BASE/tx_fifo_size" 2>&1); then
      echo "     - tx_fifo_size:  $out"
    else
      echo "     - tx_fifo_size:  [error] $out"
    fi
  fi
done

echo "[+] Done."
