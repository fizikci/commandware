# Commandware tutorial

This guide uses the current ESP8266 firmware. Commands go into the serial console
at 115200 baud or the web console. Use ESP8266 GPIO numbers, not a board's `D`
labels. Run each example separately unless it is a single schedule block.

## Connect to the board

Follow the [README build and upload steps](../README.md#getting-started).


The firmware can join an existing Wi-Fi network and expose the command shell to
devices on that same network. Before compiling, copy
[`wifi_config.example.h`](../firmware/esp12e_commandware/wifi_config.example.h) to
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

## Web request troubleshooting

Web requests are serialized and time out after ten seconds. A timed-out command
may already have executed; it is not automatically retried. A failed command
does not prevent later requests. This improves recovery but does not establish
the cause of a device lockup. If it recurs, try `status` over serial at 115200
baud: firmware status includes uptime, free heap, Wi-Fi status and IP address;
startup also prints the reset reason. Compare web and serial responsiveness
before restarting the board.

## Inspect the board

```text
help
pins
status
```

`help` lists command syntax. `pins` lists GPIO names and identifies the UART
pins. `status` reports uptime, free heap, Wi-Fi status, and the board's IP
address. GPIO1 and GPIO3 are used by the serial console; GPIO0, GPIO2, and GPIO15
affect boot mode, so preserve their required boot levels when wiring devices.

## Read and write GPIO

```text
write 4 1
```

Set GPIO4 high. With an appropriately connected LED and resistor, this can turn
the LED on. Use `write 4 0` to set the pin low.

```text
write 4 1 100
```

Set GPIO4 high for 100 milliseconds, then return it low without blocking the
console.

```text
read 5
```

Configure GPIO5 as an input and print its current digital level, `0` or `1`.

## Connect commands with pipes

A pipeline sends values from a source, through optional transforms, to a sink:

```text
read 4 | not | print
```

Read GPIO4 once, invert its value (`0` becomes `1`, and `1` becomes `0`), and
print the result. This sets GPIO4 to input mode, so use it with an input circuit
rather than expecting it to retain a previous output setting.

```text
clock 2 --dur 3000 | write 5
```

Generate a nominal 2 Hz square wave on GPIO5 for three seconds. A clock emits
two alternating values per cycle, so the pin changes level about every 250 ms.
It returns low when the pipeline finishes.

```text
clock 4 --dur 3000 | divide 2 | toggle | write 5
```

Keep every second clock event, toggle the output for each retained event, and
write that value to GPIO5. This produces a nominal 2 Hz output from a 4 Hz
source. Stateful transforms retain their state while the job runs.

Other signal transforms include `edge`, `rising`, `falling`, `debounce`, and
`pulse`. See the [PRD](PRD.md) for their command model; `help` gives the syntax
implemented by the firmware.

## Read an analog value

```text
adc A0
```

Read the ADC once and print its raw value from 0 to 1023. Keep the input voltage
within the limits of your particular board and circuit.

```text
adc A0 | map 0 1023 0 100 | print
```

Scale the ADC reading to an integer from 0 to 100 and print it. This takes one
reading per submission; it does not start a continuous ADC job.

```text
adc A0 --watch | print
```

Start a background ADC job that samples every 100 ms, emits the first reading,
then emits only changes. Use `jobs`, `kill ID`, or `reset` to manage it. ADC
watch jobs share the eight execution slots with clocks and can also start in a
schedule block. Sampling uses the board's loop; it does not block the console.

## Display text on an LCD

The LCD command supports a **16×2 HD44780 character LCD with a PCF8574 I²C
backpack**, using the common mapping P0=RS, P1=RW, P2=Enable, P3=backlight,
P4–P7=D4–D7. It uses Arduino's bundled `Wire` library; no extra library is needed.
Other backpack mappings and graphical LCD controllers are not supported.

Connect SDA to **GPIO4**, SCL to **GPIO5**, and share ground. Power the module
according to its specification. If its backpack pulls SDA/SCL up to 5 V, use a
bidirectional I²C level shifter between it and the ESP8266's 3.3 V GPIO; do not
connect a 5 V bus directly to the ESP8266. Adjust the backpack contrast
potentiometer if the backlight lights but characters are invisible.

```text
lcd print "Hello World!"
adc A0 --watch | lcd print
```

The first LCD command initializes address `0x27` automatically. If your backpack
has another address, initialize it explicitly first (addresses are seven-bit):

```text
lcd init 0x3f
lcd print "Hello World!"
```

Each print replaces the screen starting at its top-left corner. Text wraps after
16 characters and is clipped after 32; unused cells are padded with spaces so a
shorter number removes old digits. Double-quoted text preserves spaces, `|`,
and braces. The escapes `\n`, `\"`, and `\\` represent a row break, a quote,
and a backslash. LCD character ROM determines which non-ASCII characters display.

```text
lcd print "Temperature\n25 C"
adc A0 | map 0 1023 0 100 | lcd print
lcd clear
schedule { 1000 { lcd print "Ready!" } }
```

`lcd print` consumes numeric pipeline values as decimal text. Its pipeline form
accepts no literal text argument. Direct `lcd print "TEXT"`, `lcd init`, and
`lcd clear` return `OK`; communication failures report an error. A failing LCD
sink stops its background job. The last screen stays visible after `kill` or
`reset`; use `lcd clear` to blank it.

Once initialized, GPIO4/5 are reserved for LCD I²C. LCD initialization rejects
running or queued clock/ADC GPIO writers on those pins. Kill conflicting jobs
before initializing the display. LCD initialization takes about 60 ms once;
subsequent screen writes use short synchronous I²C transfers, so they can delay
clock edges. Avoid LCD updates in high-frequency clock pipelines.

The Python simulator exposes `HardwareState.lcd_lines` (two padded rows) and
`lcd_history` for inspection. A simulated ADC watch processes supplied readings,
suppresses consecutive duplicates, and returns; without supplied readings it
takes one virtual sample.

The driver timing follows the [HD44780 datasheet](https://www.sparkfun.com/datasheets/LCD/HD44780.pdf)
and uses the [PCF8574's 100 kHz I²C bus](https://www.nxp.com/docs/en/data-sheet/PCF8574_PCF8574A.pdf).

## Measure resistance


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

## Shift data

```text
shiftout 5 4 14 msb 170
```

Send the eight-bit value 170 (`10101010`) to a shift register using GPIO5 for
data, GPIO4 for clock, and GPIO14 for latch, most significant bit first.

```text
shiftin 12 4 14 msb 8
```

Read eight bits from a compatible input shift register, most significant bit
first, using GPIO12 for data, GPIO4 for clock, and GPIO14 for load. Print the
resulting integer. These examples assume the corresponding register is wired
to the named pins.

## Manage background jobs

```text
clock 2 | write 5
```

Start an unlimited clock job. The console returns a job ID and remains
responsive while GPIO5 toggles.

```text
jobs
```

List running clocks and queued schedule entries. Queued entries are marked
`[queued]` and retain their IDs when they start.

```text
kill 1
```

Cancel job 1; replace `1` with the ID reported by your board. Killing a running
clock returns its output pins low. Killing a queued entry prevents it starting.
Use `reset` to stop all clock jobs and clear the schedule queue.

## Timed clocks and scheduling

`clock HZ [PIN] [--dur MILLISECONDS]` accepts frequencies from 0.01 to
1,000,000 Hz. This is an accepted input range, not a guarantee of achievable
GPIO frequency: loop, Wi-Fi, and software GPIO overhead limit actual timing.


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

## Try the Python simulator

The simulator provides virtual GPIO and evaluates streams without a board.
From the repository root in Bash:

```bash
PYTHONPATH=src python -m commandware clock 10
echo 5 | PYTHONPATH=src python -m commandware map 0 10 0 100
echo 0 | PYTHONPATH=src python -m commandware not
```

The first command returns a bounded clock sample rather than running forever.
The second maps `5` to `50`; the third inverts `0` to `1`. Simulator timing is
virtual and does not predict the board's electrical behavior.

## Happy Birthday

With a buzzer connected to GPIO15, paste this entire block into the web command
field and click **Run**, or send it over serial followed by a newline. It plays
all four phrases automatically in about 12.5 seconds, using 25 queued entries.
To use another GPIO, replace every `write 15` with your buzzer's pin number.

Frequencies are in hertz; start offsets and durations are in milliseconds.
The short gaps make repeated notes distinct. Each note returns the output low
when it finishes, and the next note starts from the same shared timeline.

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
