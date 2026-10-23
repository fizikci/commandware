#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 [--build-only] [--fqbn FQBN] [PORT]"
  echo "Default FQBN matches Commandware's upload scripts: esp8266:esp8266:generic"
  echo "Use the SAME board/flash-layout options as your Commandware build."
}

build_only=false
fqbn="esp8266:esp8266:generic"
port=""
while (( $# )); do
  case "$1" in
    --build-only) build_only=true; shift ;;
    --fqbn)
      if (( $# < 2 )); then usage >&2; exit 1; fi
      fqbn="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    -*) usage >&2; exit 1 ;;
    *)
      if [[ -n "$port" ]]; then usage >&2; exit 1; fi
      port="$1"; shift ;;
  esac
done
if [[ ! "$fqbn" =~ ^esp8266:esp8266:[A-Za-z0-9_]+(:[^[:space:]]+)?$ ]]; then
  echo "This utility supports ESP8266 Arduino boards only." >&2; exit 1
fi
wipe_selected=false
IFS=',' read -r -a board_options <<< "${fqbn##*:}"
for option in "${board_options[@]}"; do
  case "$option" in
    wipe=none)
      if "$wipe_selected"; then echo "Duplicate wipe option." >&2; exit 1; fi
      wipe_selected=true ;;
    wipe=*)
      echo "Use wipe=none; this utility must not erase the whole flash or Wi-Fi settings." >&2; exit 1 ;;
  esac
done
if ! "$wipe_selected"; then
  if [[ "$fqbn" == *:*:*:* ]]; then fqbn+=",wipe=none"
  else fqbn+=":wipe=none"
  fi
fi
if ! "$build_only" && [[ -z "$port" ]]; then usage >&2; exit 1; fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
formatter="$script_dir/tools/provision_littlefs"
application="$script_dir/firmware/esp12e_commandware"
build_dir="$script_dir/.build/littlefs-provision"
echo "Target FQBN: $fqbn"
arduino-cli compile --fqbn "$fqbn" --build-path "$build_dir/formatter" "$formatter"
# Verify the application can be restored before offering to upload maintenance firmware.
arduino-cli compile --fqbn "$fqbn" --build-path "$build_dir/application" "$application"
if "$build_only"; then echo "Builds verified; nothing uploaded or formatted."; exit 0; fi

echo "Port: $port"
echo "This temporarily replaces Commandware with the separate provisioning sketch."
echo "Formatting requires FORMAT LITTLEFS over serial and erases ALL filesystem files."
read -r -p 'Type UPLOAD FORMATTER to upload maintenance firmware, or anything else to cancel: ' answer
if [[ "$answer" != "UPLOAD FORMATTER" ]]; then echo "Cancelled; nothing uploaded or formatted."; exit 0; fi
arduino-cli upload --port "$port" --fqbn "$fqbn" --input-dir "$build_dir/formatter" "$formatter"
echo "Open the serial monitor, check the displayed flash layout, then send FORMAT LITTLEFS:"
printf '  arduino-cli monitor --port %q --config baudrate=115200\n' "$port"
echo "After PROVISIONED, close the monitor and restore Commandware with this exact command:"
printf '  arduino-cli upload --port %q --fqbn %q --input-dir %q %q\n' "$port" "$fqbn" "$build_dir/application" "$application"
echo "Keep this FQBN/layout for future firmware updates. Copy any unsaved editor text before reloading the browser."
