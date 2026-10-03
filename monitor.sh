#!/usr/bin/env bash
set -euo pipefail

arduino-cli monitor -p COM3 --config baudrate=115200
