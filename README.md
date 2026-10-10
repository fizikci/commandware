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

## ESP8266 web interface

The firmware can join an existing Wi-Fi network and expose the command shell to
devices on that same network. Before compiling, set `WIFI_SSID` and
`WIFI_PASSWORD` near the top of
[the firmware sketch](firmware/esp12e_commandware/esp12e_commandware.ino).
Upload the sketch and check the serial monitor for the board's IP address, then
open `http://<board-ip>/` on your phone or browser.

The page accepts the same commands as the serial shell. Commands can also be
submitted directly with an HTTP POST:

```bash
curl --data-urlencode "command=read 4" http://<board-ip>/api/command
```

The endpoint accepts a form field named `command` and returns command output as
plain text. The web interface has no authentication; use it only on a trusted
Wi-Fi network and do not expose it to the public internet. Do not commit real
Wi-Fi credentials to a shared repository.

The page has one editable command field with suggestions for common GPIO
commands. Successfully run commands are saved in a newest-first history on that
browser (up to 12 commands), so they can be selected and run again. The
running-jobs list refreshes every three seconds and provides a Kill button for
each job.

Command names are case-insensitive, including pipeline sources and stages. For
example, `READ 4`, `NOT 0`, and `CLOCK 100 | WRITE 5` work the same as their
lowercase forms.

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

`clock HZ [PIN]` generates an alternating 0/1 signal at the requested
frequency. For example, `clock 500 | write 5` toggles GPIO5 every millisecond,
producing a nominal 500 Hz square wave. The firmware accepts values from
0.01 to 1,000,000 Hz and schedules toggles with microsecond timing. This is an
input range, not a guarantee that the software-driven GPIO output can reach
every requested frequency: loop and `digitalWrite` overhead limit the actual
output rate, especially at the high end. The ESP8266 itself does not impose the
old 500 Hz limit; that came from the firmware's former millisecond scheduler.
