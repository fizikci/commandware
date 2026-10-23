# Commandware tutorial

Commandware lets you experiment with hardware by submitting small programs over
serial or the web console. Start with the [README](../README.md) for firmware
build/upload instructions. The [language guide](language.md) explains the model;
the [reference](language-reference-draft.md) defines every command contract.

## Connect and inspect

Use serial at 115200 baud or open the board's reported Wi-Fi address. Paste
multiline programs into the editor and click Run or press Ctrl/Cmd+Enter. Saved
scripts live in that browser's local storage. A timed-out web request may already
have executed; inspect state before retrying an action.

```text
help
pins
status
```

Use `pins` to inspect availability. GPIO1/3 are reserved for UART and flash pins
are unavailable. GPIO0/2/15 affect boot mode; preserve their required boot levels.
Digital commands use numeric GPIOs, not `D1` labels. Use suitable resistors,
drivers, and voltage levels for the actual circuit.

## An LED and an input

With an LED and resistor on GPIO12:

```text
write 12 1
sleep 100
write 12 0
```

Statements execute in order, including the cooperative wait. There is no timed
`write` shorthand. No automatic cleanup write happens when a job ends: include
the final level explicitly. To read a separate input circuit on GPIO14:

```text
read 14 | print
```

`read` configures its pin as input. A pipe passes an event, and `print` explicitly
observes its primary value. A read by itself does not print the reading.

```text
clock 2 --dur 1000 | write 12
```

This generates a nominal 2 Hz square wave for one second. First high is due after
250 ms; later transitions are 250 ms apart. The exact end deadline starts no new
event. Last output is preserved; write low afterward when desired. Frequencies
are best-effort because software, Wi-Fi, and device transfers delay servicing.

## Analog readings and decisions

Keep A0 within the voltage range of your board and circuit.

```text
adc A0 | map 0 1023 0 100 | print
```

`map` scales the reading and truncates its final result toward zero. It does not
clamp values. Fields let you retain both units:

```text
adc A0 --out raw |
    map 0 1023 0 100 --out percent |
    write 12 $raw > 700 |
    print "$raw counts, $percent percent"
```

`write` uses the comparison but preserves the event containing the percentage.
To sample in the background:

```text
every 100 | adc A0 | print
```

Use `jobs` and `kill ID` with the actual reported ID. Each source owns an
independent job. `adc A0 --watch | print` instead samples every 100 ms and emits
the first reading and changes only. Stop prior jobs before starting another
writer on the same pin.

Hysteresis keeps a shared state between two thresholds:

```text
set enabled 0
every 100 | adc A0 --out raw | do {
    if $raw > 700 { set enabled 1 }
    if $raw < 600 { set enabled 0 }
    write 12 $enabled
}
```

Each body statement receives the same raw event. Global updates are visible to
later statements, so the final write uses the current `enabled` value.

## Parse data and reuse commands

```text
func notes text {
    split " " $text | each --out note {
        split ":" $note --out pitch,dur |
            tone 15 --hz notehz $pitch --dur int $dur
    }
}
notes "C4:200 E4:300 G4:400"
```

Connect a suitable buzzer circuit on GPIO15. Each note finishes before the next
starts. `notehz` and `int` consume their arguments directly; parentheses are
optional grouping. Function parameters are read-only, while `set` updates shared
globals. Pure functions also process text/lists:

```text
max 5 min 10 3 | print
split ":" "C4:E4:G4" | join $value ", " | print
slice "é雪😀" 1 3 | print
```

The outputs are `5`, `C4, E4, G4`, and `雪😀`. See the complete
[notes-and-blink program](examples/notes-and-blink.cw) for independent LED work
while a melody plays.

## Shape signals

```text
clock 4 --dur 1000 | divide 2 | write 12
```

`divide` emits initial low, then toggles after every second input transition.
`edge`, `rising`, and `falling` first establish a baseline and filter subsequent
changes. `debounce 20` accepts a Boolean level after 20 stable milliseconds.
`pulse 100` emits high immediately on any event, then low; retriggering extends
the deadline. Pipe these transforms to explicit actions:

```text
pulse 100 | write 12
```

Pulse retains the job until the pending low continuation finishes. Canceling that
job may leave high on the pin, so use an explicit write after cancellation when
needed. Stateful stages inside a function initialize afresh on every call.

## Use an LCD

The supported LCD is a 16×2 HD44780 with a PCF8574 I2C backpack using
P0=RS, P1=RW, P2=Enable, P3=backlight, P4–P7=data. Connect SDA to GPIO4 and SCL
to GPIO5, sharing ground. If a backpack pulls the bus to 5 V, use a bidirectional
level shifter; do not connect a 5 V I2C bus directly to ESP8266 GPIO. Adjust
contrast if the backlight works but characters are invisible.

```text
lcd init 0x27
lcd print "Commandware\nReady"
```

The default address is `0x27`; use the module's actual seven-bit address. Text
wraps at 16 characters and clips at 32; shorter text pads old cells. Character
ROM determines available glyphs. To show live readings:

```text
every 200 | adc A0 | lcd print
```

GPIO4/5 become reserved for I2C. Kill conflicting jobs before initialization.
Display actions preserve the event, and `kill`/`reset` preserves the last screen.
Use `lcd clear` to erase it. I2C operations can delay other work.

## Draw on an OLED

Use an SSD1306 or SH1106 128×64/128×32 I2C module, with 3.3 V bus pull-ups,
SDA=GPIO4 and SCL=GPIO5. Select the actual controller and screen size:

```text
oled init ssd1306 --height 64
oled print "A0 counts"
every 200 | adc A0 | oled chart --min 0 --max 1023 --y 8 --height 56
```

Try the module's documented address, commonly `0x3c` or `0x3d`. An acknowledgment
does not identify its controller. Initialization/clear erases contents; printing
changes only character cells. The bundled bitmap font is ASCII. Stop a previous
screen-update job before starting another layout. The
[two-chart example](examples/oled-charts.cw) keeps raw and percentage fields from
one reading. Simulation models display pixels/history, not physical I2C timing.

Use filled rectangles for simple graphics. Drawing and erasing affect only the
specified pixels, and coordinates/dimensions must be integers within the screen:

```text
oled init ssd1306
oled rect 1 24 3 14
oled rect 63 32 2 2
oled rect 1 24 3 14 --value 0
oled rect 1 22 3 14
```

The [two-player Pong example](examples/pong.cw) uses GPIO12 for the left button
and GPIO14 for the right. Connect each button between its GPIO and GND, with a
separate 10 kΩ external pull-up from that GPIO to 3.3 V. `read` uses plain input
mode, so pull-ups are required. Hold a button to move its paddle up; release to
move down. The program assumes a 128×64 OLED; change the initialization controller
or address to match your module. Stop old jobs before running the full file in
the web editor. Use `jobs` and `kill ID` to stop the game.

Pong samples both buttons and updates the ball every 50 ms, with a short serve
pause after each point while paddles remain movable. Scores wrap after 99. OLED
transfers and script scheduling are best effort; simulator/native checks do not
establish animation smoothness on the physical board.

## Measure resistance

```text
adc A0 | ohm | print
```

This calibration is specific to a measured circuit: 4.954 V through 20,160 Ω to
A0, with 3,357 Ω and the unknown resistor in parallel from A0 to ground. Changing
the supply, divider, or A0 circuitry requires new calibration. `ohm` transforms
an integer count; it does not read A0 or print implicitly.

| Resistor (Ω) | Raw A0 | Node voltage (mV) |
| --- | --- | --- |
| 270 | 50 | 60 |
| 995 | 179 | 181 |
| 1992 | 291 | 288 |
| 5152 | 459 | 449 |
| 9959 | 557 | 543 |
| 20020 | 628 | 612 |
| 108800 | 703 | 682 |
| 688900 | 720 | 697 |

Counts map linearly to measured voltage, then exponentially between adjacent
resistance measurements. Counts 30–720 return whole ohms. Counts 30–49 extrapolate
the first segment and are uncalibrated. Lower counts return `<270`; higher counts
return `>700k`; counts outside 0–1023 return `OUT_OF_RANGE`. The upper label is
approximate; the final measured resistance is 688,900 Ω. Interpolation does not
establish accuracy between measurements.

## Shift one hardware frame

```text
shiftout 12 13 14 msb --value 170 --bits 8
shiftin 12 13 14 msb --bits 8 | print
```

Run these separately with suitable output/input-register circuits. Pins are data,
clock, and latch/load. Width defaults to 8; `--bits` permits 1–53 exactly
representable bits. `shiftout` preserves the event; `shiftin` replaces its value.
Input samples the first bit before the first clock pulse. For output without a
latch, use `shiftout 12 13 msb --value 170 --bits 8`. Bit order can be `lsb`.
Verify register load/latch polarity and clock timing on the actual circuit.

## Start work on a timeline

```text
schedule {
    0 { tone 15 --hz notehz "C4" --dur 200 }
    250 { tone 15 --hz notehz "C4" --dur 200 }
    500 { tone 15 --hz notehz "D4" --dur 450 }
    1000 { tone 15 --hz notehz "C4" --dur 450 }
    1500 { tone 15 --hz notehz "F4" --dur 450 }
    2000 { tone 15 --hz notehz "E4" --dur 900 }
}
```

This plays the first Happy Birthday phrase. Offsets are milliseconds from one
registration timeline, evaluated once. Each entry gets its own job and reads
live globals. There are 32 pending entries and eight active execution slots.
An entry can fail at execution for unavailable resources; independent entries
continue. Registration does not wait or emit an event. `reset` cancels queued and
active work, clears globals/failures, and preserves pin/display state and function
definitions. No command automatically drives a pin low at completion.

## Happy Birthday

This complete 25-note melody preserves the shared timeline's note durations and
short gaps, finishing in about 12.4 seconds. A reusable command function makes
those gaps explicit instead of relying on cleanup writes or implicit pauses.
Each token is `NOTE:DURATION_MS:GAP_MS`. With a suitable buzzer circuit on GPIO15,
paste the whole program into the console:

```text
func birthday score {
    split " " $score | each --out entry {
        split ":" $entry --out pitch,duration,gap | do {
            tone 15 --hz notehz $pitch --dur int $duration
            write 15 0
            sleep int $gap
        }
    }
}
birthday "C4:200:50 C4:200:50 D4:450:50 C4:450:50 F4:450:50 E4:900:100 C4:200:50 C4:200:50 D4:450:50 C4:450:50 G4:450:50 F4:900:100 C4:200:50 C4:200:50 C5:450:50 A4:450:50 F4:450:50 E4:450:50 D4:900:100 Bb4:200:50 Bb4:200:50 A4:450:50 F4:450:50 G4:450:50 F4:900:0"
```

Replace both occurrences of GPIO15 when using another pin. The explicit low
write makes each inter-note gap silent and leaves the buzzer low after completion.
Finite statements execute in order, so the melody needs one active job rather
than 25 queued entries. Timing remains best-effort on the physical board.

## Try programs without a board

From the repository root:

```bash
PYTHONPATH=src python -m commandware 'max 5 min 10 3 | print'
PYTHONPATH=src python -m commandware 'adc A0 | map 0 1023 0 100 | print'
```

The simulator advances virtual time and bounds unlimited source samples. For
manual stepping use `ScriptRuntime.submit` and increasing `tick` timestamps.
Check `HardwareState.script_writes`, `tone_history`, and display history to inspect
simulated behavior. Simulation cannot validate wiring or physical waveform fidelity.

Run `python -m pytest`, `python tests/run_firmware_tests.py`, and
`node tests/web_request.test.cjs` for software checks. Board checks still include
GPIO ownership, pulse/tone timing under Wi-Fi load, I2C recovery, and peak heap.
