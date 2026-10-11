# Commandware

Commandware is a command shell for ESP8266 / ESP-12E boards. Read sensors,
control GPIO, build signal pipelines, and schedule commands over serial or a
local web interface. A Python simulator is included for experimenting without
hardware.

## Getting started

Install Arduino CLI and the ESP8266 board core. For Wi-Fi, copy
[`wifi_config.example.h`](firmware/esp12e_commandware/wifi_config.example.h) to
`wifi_config.h` in the same directory and fill in your credentials. Git ignores
the local configuration; without it, the serial console still works.

Build, upload, and monitor (replace `COM3` with your board's port):

```bash
arduino-cli compile --fqbn esp8266:esp8266:generic firmware/esp12e_commandware
arduino-cli upload -p COM3 --fqbn esp8266:esp8266:generic firmware/esp12e_commandware
arduino-cli monitor -p COM3 --config baudrate=115200
```

## Documentation

- [Tutorial](docs/tutorial.md): setup, explained commands, pipelines, scheduling,
  and a complete Happy Birthday melody.
- [Product requirements](docs/PRD.md): project goals, command model, and design.
