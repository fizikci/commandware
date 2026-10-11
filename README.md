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
- ohm
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

## Calibrated resistance measurement

Run `ohm` or `ohm A0` over serial or the web console. `adc A0 | ohm` also
works, with `ohm` as the terminal measurement stage. Numeric output is rounded
to whole ohms. In the Python simulator, `ohm` converts incoming raw ADC values
or reads the virtual A0 when no values are supplied.

This calibration is specific to the measured circuit: 4.954 V through 20,160 Ω
to A0, with 3,357 Ω and the unknown resistor in parallel from A0 to ground.
ADC counts are interpolated linearly to measured node voltage. Between adjacent
measured voltage/resistance pairs, resistance follows a piecewise exponential
function, with voltage in millivolts and resistance in ohms:

```text
R(v) = Ri * exp(((v - vi) / (vi+1 - vi)) * log(Ri+1 / Ri))
```

| Known resistor (Ω) | Raw A0 | Node voltage (mV) |
| --- | --- | --- |
| 270 | 50 | 60 |
| 995 | 179 | 181 |
| 1992 | 291 | 288 |
| 5152 | 459 | 449 |
| 9959 | 557 | 543 |
| 20020 | 628 | 612 |
| 108800 | 703 | 682 |
| 688900 | 720 | 697 |

The function passes through all eight measurements and follows the connecting
lines on the logarithmic resistance chart. Numeric output is accepted for A0
counts 30–720, including both endpoints (approximately 41.2403–697 mV).
Counts 30–49 extrapolate the first measured segment, giving approximately
221–267 Ω; this extension is uncalibrated. Counts below 30 report `<270`;
counts above 720 report `>700k`. The upper label is approximate: the last
measured resistance is 688,900 Ω, and no extrapolation to 700,000 Ω is performed.
Counts outside the valid ADC range 0–1023 report `OUT_OF_RANGE`.

Python also provides `resistance_from_voltage(v_mv)` for measured millivolts,
with the same bounds and whole-ohm output. Interpolation does not establish
accuracy between measurements, particularly near the high-resistance plateau.
Changing the supply, divider, or A0 input circuitry requires new calibration.

Web requests are serialized and time out after ten seconds. A timed-out command
may already have executed; it is not automatically retried. A failed command
does not prevent later requests. This improves recovery but does not establish
the cause of a device lockup. If it recurs, try `status` over serial at 115200
baud: firmware status includes uptime, free heap, Wi-Fi status and IP address;
startup also prints the reset reason. Compare web and serial responsiveness
before restarting the board.
