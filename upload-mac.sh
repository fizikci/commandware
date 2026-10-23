#!/usr/bin/env bash
set -euo pipefail

if (( $# > 1 )); then
  echo "Usage: $0 [PORT]" >&2
  exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sketch="$script_dir/firmware/esp12e_commandware"
fqbn="esp8266:esp8266:generic:wipe=none"

if (( $# == 1 )); then
  port="$1"
else
  shopt -s nullglob
  ports=(/dev/cu.usbserial* /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART*)
  if (( ${#ports[@]} != 1 )); then
    echo "Expected one USB serial port; found ${#ports[@]}." >&2
    echo "Connect the board or specify its port: $0 /dev/cu.usbserial-0001" >&2
    if (( ${#ports[@]} > 0 )); then
      printf '  %s\n' "${ports[@]}" >&2
    fi
    exit 1
  fi
  port="${ports[0]}"
fi

if [[ ! -e "$port" ]]; then
  echo "Serial port does not exist: $port" >&2
  exit 1
fi

echo "Uploading to $port"
echo "Target FQBN: $fqbn (firmware only; preserves LittleFS)"
arduino-cli compile --fqbn "$fqbn" "$sketch"
arduino-cli upload --port "$port" --fqbn "$fqbn" "$sketch"
