#!/usr/bin/env bash
set -euo pipefail

arduino-cli compile --fqbn esp8266:esp8266:generic firmware/esp12e_commandware
arduino-cli upload -p COM3 --fqbn esp8266:esp8266:generic firmware/esp12e_commandware
