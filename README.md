# Commandware

Commandware is a lightweight simulator for the ESP8266 hardware shell described in the PRD.

## Features

- GPIO-based command pipeline inspired by Unix-style filtering
- Source, transform, and sink commands
- Virtual hardware state for reads and writes
- Example commands for clocks, ADCs, shifts, and pulse logic

## Quick start

```bash
PYTHONPATH=src python -m commandware clock 10
PYTHONPATH=src python -m commandware map 0 10 0 100 <<< "5"
PYTHONPATH=src python -m commandware not <<< "1"
```

## Example pipeline

```bash
echo 0 | PYTHONPATH=src python -m commandware not
```

## ESP-12E firmware workflow

Build and upload the sketch in one step:

```bash
arduino-cli compile --fqbn esp8266:esp8266:generic firmware/esp12e_commandware && arduino-cli upload -p COM3 --fqbn esp8266:esp8266:generic firmware/esp12e_commandware
```

If you want to compile only:

```bash
arduino-cli compile --fqbn esp8266:esp8266:generic firmware/esp12e_commandware
```

If you want to upload only:

```bash
arduino-cli upload -p COM3 --fqbn esp8266:esp8266:generic firmware/esp12e_commandware
```

Monitor the serial console:

```bash
arduino-cli monitor -p COM3 --config baudrate=115200
```

## Supported commands

- read
- adc
- clock
- shiftin
- map
- not
- edge
- rising
- falling
- debounce
- toggle
- divide
- pulse
- write
- print
- shiftout
