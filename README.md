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
devices on that same network. Before compiling, copy
[`wifi_config.example.h`](firmware/esp12e_commandware/wifi_config.example.h) to
`wifi_config.h` in the same directory:

```powershell
Copy-Item firmware/esp12e_commandware/wifi_config.example.h firmware/esp12e_commandware/wifi_config.h
```

Set `COMMANDWARE_WIFI_SSID` and `COMMANDWARE_WIFI_PASSWORD` in your local
`wifi_config.h`. Git ignores that file; commit only the empty example header.
The firmware uses the local header when present. Without it, Wi-Fi is disabled
and the serial console still works. Arduino uses this header at compile time;
it does not load a `.env` file at runtime.

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
Ignoring a file does not remove credentials from earlier commits. If credentials
have already been committed or shared, change the Wi-Fi password; removing them
from Git history is a separate operation. Credentials are also embedded in the
compiled firmware, so keep builds containing them private.

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

`clock HZ [PIN] [--dur MILLISECONDS]` generates an alternating 0/1 signal at the requested
frequency. For example, `clock 500 | write 5` toggles GPIO5 every millisecond,
producing a nominal 500 Hz square wave. The firmware accepts values from
0.01 to 1,000,000 Hz and schedules toggles with microsecond timing. This is an
input range, not a guarantee that the software-driven GPIO output can reach
every requested frequency: loop and `digitalWrite` overhead limit the actual
output rate, especially at the high end. The ESP8266 itself does not impose the
old 500 Hz limit; that came from the firmware's former millisecond scheduler.

## Timed clocks and scheduling

Clocks run indefinitely by default. Add `--dur` to stop a clock after a given
number of milliseconds, including any downstream pipeline stages:

```text
clock 1000 --dur 500 | write 15
```

This plays a nominal 1000 Hz tone for 500 ms. Completion, `kill ID`, and `reset`
drive the clock's output pins low. The existing shorthand also accepts a
duration: `clock 1000 15 --dur 500`. Without a write or print sink, a clock prints
its values to serial.

Use a schedule block to start commands on one shared timeline:

```text
schedule {
    1000 { clock 1000 --dur 500 | write 15 }
    1500 { clock 1400 --dur 1000 | write 15 }
}
```

The timeline starts after the complete block is accepted. Each number is a
start offset in milliseconds. The first note starts at 1000 ms and finishes at
1500 ms; the second starts at 1500 ms and finishes at 2500 ms. Entries may be
written in any order. Different pins can run concurrently; overlapping clock
writes to the same pin are rejected before any entries are queued. At a shared
boundary, expired clocks stop before the next clocks start.

Paste the block into the multiline web command field, or send it over serial;
serial waits for the outer closing brace and a newline before submitting it.
Single-line blocks work too. Immediate commands such as `1000 { write 4 1 }`
run once at their scheduled time. Administrative commands (`kill`, `reset`,
`jobs`) and nested schedules are not allowed inside a block.

The firmware queues up to 32 future entries separately from its eight running
clock slots. A future note only takes an execution slot when it starts, so a
whole melody can fit in one block. `jobs` lists both queued and running entries
with stable job IDs, and `kill ID` cancels either. Blocks that overflow the
queue or would require more than eight concurrent clocks are rejected as a
whole. New clocks are also checked against already queued notes.
Offsets are integers from 0 to 2,147,483,647 ms; durations are integers from 1
to 2,147,483,647 ms, and offset plus duration must fit that same maximum.
Timing is serviced by the device loop rather than by delays on the computer;
Wi-Fi and loop overhead can still delay signal edges.

The Python simulator evaluates schedules on a virtual timeline without waiting
in real time. Finite clocks produce samples until their duration ends and then
release their output pins. Unlimited clocks retain bounded simulation sampling
(one sample by default, or `run_stream(..., count=N)`), while firmware clocks
remain active indefinitely. The simulator does not model GPIO or network timing
accuracy.

Verify changes with `python -m pytest` and `node tests/web_request.test.cjs`.
On Windows with Visual Studio Community C++ tools installed,
`python tests/run_firmware_tests.py` runs the actual firmware parser, scheduler,
and serial loop against a simulated clock and GPIO without a connected board.

## Music example: Happy Birthday

With a buzzer connected to GPIO15, paste this entire block into the web command
field and click **Run**, or send it over serial followed by a newline. It plays
all four phrases automatically in about 12.5 seconds. Frequencies are in hertz;
start offsets and durations are in milliseconds. The gaps between notes make
repeated notes distinct, and the output returns low when each note finishes.

The same command is available in [examples/happy-birthday.cw](examples/happy-birthday.cw).
To use another GPIO, replace every `write 15` with your buzzer's pin number.

```text
schedule {
    0     { clock 262 --dur 200 | write 15 }
    250   { clock 262 --dur 200 | write 15 }
    500   { clock 294 --dur 450 | write 15 }
    1000  { clock 262 --dur 450 | write 15 }
    1500  { clock 349 --dur 450 | write 15 }
    2000  { clock 330 --dur 900 | write 15 }
    3000  { clock 262 --dur 200 | write 15 }
    3250  { clock 262 --dur 200 | write 15 }
    3500  { clock 294 --dur 450 | write 15 }
    4000  { clock 262 --dur 450 | write 15 }
    4500  { clock 392 --dur 450 | write 15 }
    5000  { clock 349 --dur 900 | write 15 }
    6000  { clock 262 --dur 200 | write 15 }
    6250  { clock 262 --dur 200 | write 15 }
    6500  { clock 523 --dur 450 | write 15 }
    7000  { clock 440 --dur 450 | write 15 }
    7500  { clock 349 --dur 450 | write 15 }
    8000  { clock 330 --dur 450 | write 15 }
    8500  { clock 294 --dur 900 | write 15 }
    9500  { clock 466 --dur 200 | write 15 }
    9750  { clock 466 --dur 200 | write 15 }
    10000 { clock 440 --dur 450 | write 15 }
    10500 { clock 349 --dur 450 | write 15 }
    11000 { clock 392 --dur 450 | write 15 }
    11500 { clock 349 --dur 900 | write 15 }
}
```

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
