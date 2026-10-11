# Commandware — ESP8266 Hardware Shell

## 1. Goal

Commandware is generic firmware for the **ESP8266 ESP-12E** that turns the microcontroller into an interactive electronics tool.

Instead of writing and flashing firmware for every breadboard experiment, the user controls hardware from a terminal using Unix-like pipelines.

```text
source | transform | transform | sink
```

Examples:

```bash
read 5
write 5 1

clock 100 | write 5

adc A0 | map 0 1023 0 255 | print

adc A0 | map 0 1023 0 255 | shiftout 5 4 14 msb
```

The last example reads a potentiometer and displays its value using a 74HC595 and eight LEDs.

---

# 2. Core Philosophy

There are three classes of pipeline commands:

```text
READ / SOURCE
      ↓
  TRANSFORM
      ↓
 WRITE / SINK
```

Commands should know as little as possible about each other.

For example:

```bash
clock 100
```

produces:

```text
0
1
0
1
...
```

It doesn't know what consumes the values.

Therefore:

```bash
clock 100 | write 5
```

makes GPIO5 oscillate.

The same source could later be used as:

```bash
clock 100 | divide 10 | write 5
```

---

# 3. Values and Streams

Clock sources run indefinitely unless given `--dur MILLISECONDS`. Duration
belongs to the source; completion stops its pipeline and drives output pins low.
Scheduling controls only when commands start, relative to one shared timeline:

```text
schedule {
    1000 { clock 1000 --dur 500 | write 15 }
    1500 { clock 1400 --dur 1000 | write 15 }
}
```

Entries run independently, with up to 32 pending commands and eight concurrent
clocks. Future entries acquire execution slots only when they start.
Overlapping clock writers on one pin are rejected; adjacent
notes release the pin before the next note begins. Both serial and web input
accept multiline blocks, and queued entries can be cancelled with `kill ID`.

Internally, pipelines operate on events.

Conceptually:

```cpp
struct Event {
    Value value;
    uint32_t timestamp;
};
```

`Value` should initially support:

```text
integer
boolean
```

Later it can support strings, arrays, bytes, etc.

A command can emit one event or an indefinite stream of events.

---

# 4. GPIO Naming

Always use **ESP8266 GPIO numbers**, not ESP-12E physical pin numbers or NodeMCU `D0`, `D1`, etc.

Examples:

```bash
read 5
write 12 1
clock 100 | write 14
```

Optionally accept:

```bash
read GPIO5
```

as equivalent to:

```bash
read 5
```

---

# 5. Source Commands

### `read`

Read digital GPIO.

```bash
read 5
```

Returns once:

```text
1
```

Continuous mode:

```bash
read 5 --watch
```

Produces values whenever appropriate.

Prefer change-driven output rather than continuously flooding identical values.

---

### `adc`

Read analog input.

```bash
adc A0
```

Continuous:

```bash
adc A0 --watch
```

Example:

```bash
adc A0 --watch | map 0 1023 0 255 | print
```

---

### `clock`

Generate alternating 0/1 values.

```bash
clock 500
```

The argument is a frequency in hertz, not a toggle interval. `clock 500`
toggles every 1 ms, producing a nominal 500 Hz square wave:

```text
0
1
0
1
...
```

Example:

```bash
clock 500 | write 5
```

GPIO5 becomes a square wave.

Timing must be **non-blocking**. Do not implement it with long `delay()` calls.
The ESP8266 firmware uses microsecond scheduling and accepts frequencies from
0.01 to 1,000,000 Hz, with half-periods rounded to the nearest microsecond
(minimum 1 microsecond). This is the accepted command range, not a guarantee
that software-driven GPIO output can reach every requested frequency: loop and
`digitalWrite` overhead constrain actual output, particularly at high
frequencies. The previous 500 Hz ceiling was imposed by the firmware's
millisecond scheduler, not by the ESP8266 itself.

---

### `shiftin`

Read serial data, primarily for devices such as the 74HC165.

Initial syntax:

```bash
shiftin DATA CLOCK LOAD msb
```

or:

```bash
shiftin DATA CLOCK LOAD lsb
```

The result should be an integer.

---

# 6. Transform Commands

Initial transforms:

```text
map
not
and
or
xor
edge
rising
falling
debounce
toggle
divide
pulse
```

### `map`

```bash
map IN_MIN IN_MAX OUT_MIN OUT_MAX
```

Example:

```bash
adc A0 | map 0 1023 0 255 | print
```

---

### `not`

```bash
read 5 --watch | not | write 4
```

```text
0 → 1
1 → 0
```

---

### `edge`

Only emit when value changes.

```bash
read 5 --watch | edge | print
```

---

### `rising`

Only emit a rising edge.

```bash
read 5 --watch | rising | print
```

---

### `falling`

Equivalent for `1 → 0`.

---

### `debounce`

```bash
read 5 --watch | debounce 20 | print
```

20 ms debounce.

Useful for buttons.

---

### `toggle`

Every incoming event toggles:

```text
0
1
0
1
...
```

Example:

```bash
read 5 --watch | rising | toggle | write 4
```

A button can therefore toggle an LED.

---

### `divide`

Divide an event/clock stream.

```bash
clock 100 | divide 10 | write 5
```

---

### `pulse`

Generate a pulse of specified duration.

```bash
read 5 --watch | rising | pulse 100 | write 4
```

---

# 7. Sink Commands

### `write`

```bash
write PIN VALUE
```

Example:

```bash
write 5 1
write 5 0
```

When used in a pipeline:

```bash
clock 500 | write 5
```

the value comes from upstream.

---

### `print`

Print incoming values to terminal.

```bash
adc A0 --watch | print
```

---

### `shiftout`

Serialize an integer to GPIO pins.

Syntax:

```bash
shiftout DATA CLOCK LATCH [msb|lsb]
```

Example:

```bash
adc A0 | map 0 1023 0 255 | shiftout 5 4 14 msb
```

For an input value:

```text
134
```

binary representation is:

```text
10000110
```

`msb` sends:

```text
1 0 0 0 0 1 1 0
```

`lsb` sends:

```text
0 1 1 0 0 0 0 1
```

For every bit:

```text
DATA = bit
CLOCK HIGH
CLOCK LOW
```

After all bits:

```text
LATCH HIGH
LATCH LOW
```

### Important design decision

There is **no `bits=8` parameter**.

`shiftout` sends the natural binary representation of the integer.

```text
5   → 101
134 → 10000110
537 → 1000011001
```

Commandware should not artificially impose the target hardware's register width.

---

# 8. Background Jobs

Long-running commands cannot occupy the terminal.

For:

```bash
clock 100 | write 5
```

Commandware responds:

```text
Started job 1
>
```

Terminal remains usable.

Example:

```bash
clock 500 | write 12
```

```text
Started job 2
```

Then:

```bash
jobs
```

could show:

```text
1 RUNNING  clock 100 | write 5
2 RUNNING  clock 500 | write 12
```

Stop one:

```bash
kill 1
```

A job owns the hardware resources it uses.

---

# 9. Pin Ownership

Commandware must prevent conflicting jobs from silently controlling the same GPIO.

For example:

```bash
clock 100 | write 5
```

starts job 1.

Then:

```bash
write 5 1
```

should return something like:

```text
ERROR: GPIO5 is owned by job 1
```

The user can:

```bash
kill 1
```

then use GPIO5 again.

---

# 10. System Commands

Minimum shell commands:

```text
help
pins
jobs
kill
status
reset
run
```

### `pins`

Especially important.

```bash
pins
```

Example output:

```text
GPIO0   GPIO       available
GPIO1   UART-TX    terminal
GPIO2   GPIO       available
GPIO3   UART-RX    terminal
GPIO4   GPIO       available
GPIO5   GPIO       available
GPIO12  GPIO/MISO  available
GPIO13  GPIO/MOSI  available
GPIO14  GPIO/SCLK  available
GPIO15  GPIO/CS    available
GPIO16  GPIO       available
A0      ADC        available
```

Commandware must understand that some pins have alternate peripheral functions.

---

# 11. UART Terminal

Initial Commandware interface should be the serial terminal.

For example:

```text
115200 baud
8-N-1
```

GPIO1/GPIO3 are therefore normally reserved for UART TX/RX.

Commandware should not casually allow a running command to steal the terminal pins.

Later, Wi-Fi/Telnet/WebSocket/SSH-like interfaces can remove that restriction.

---

# 12. Parser Grammar

Keep parsing deliberately simple.

Conceptually:

```text
LINE       := PIPELINE
PIPELINE   := COMMAND ("|" COMMAND)*
COMMAND    := NAME ARG*
ARG        := NUMBER | IDENTIFIER | OPTION
```

Example:

```bash
adc A0 | map 0 1023 0 255 | shiftout 5 4 14 msb
```

becomes:

```text
Pipeline
 ├─ adc(A0)
 ├─ map(0,1023,0,255)
 └─ shiftout(5,4,14,msb)
```

Do **not** implement a full Bash-style shell.

No quoting rules, redirection, environment variables, subshells, etc. in v1.

---

# 13. Scripts

A critical principle:

> Anything valid interactively should also be valid inside a Commandware program.

For example:

```text
# demo.cmd

clock 500 | write 5

adc A0 --watch |
    map 0 1023 0 255 |
    shiftout 12 13 14 msb
```

Then:

```bash
run demo.cmd
```

Each persistent pipeline becomes a job.

Eventually:

```bash
autorun demo.cmd
```

could cause the program to start at boot.

This allows Commandware to become actual firmware for projects rather than merely a debugging shell.

---

# 14. Architecture

Codex should separate the implementation approximately like:

```text
src/
  main.cpp

  shell/
    Terminal
    Parser
    CommandRegistry

  runtime/
    Pipeline
    Job
    Scheduler
    Event
    Value

  commands/
    sources/
      Read
      ADC
      Clock
      ShiftIn

    transforms/
      Map
      Not
      Edge
      Rising
      Falling
      Debounce
      Toggle
      Divide
      Pulse

    sinks/
      Write
      Print
      ShiftOut

  hardware/
    GPIO
    PinManager
    Timer

  system/
    Help
    Pins
    Jobs
    Kill
    Status
```

Don't create one enormous `main.cpp`.

Each command should implement a common interface so new commands are easy to add.

---

# 15. Runtime

The ESP8266 must remain responsive while many pipelines operate.

Therefore avoid architecture like:

```cpp
while (true) {
    // command owns CPU forever
}
```

Instead use cooperative/non-blocking execution.

Conceptually:

```text
loop()

  terminal.poll()
  scheduler.tick()
      job1.tick()
      job2.tick()
      job3.tick()
```

No pipeline should monopolize `loop()`.

This is particularly important because ESP8266 Wi-Fi and watchdog handling also need CPU time.

---

# 16. Errors

Errors should be short and terminal-friendly.

```text
ERROR: unknown command 'foo'

ERROR: GPIO5 is owned by job 3

ERROR: GPIO99 does not exist

ERROR: map requires 4 arguments

ERROR: expected source at pipeline start

ERROR: write requires numeric input
```

Never silently ignore malformed commands.

---

# 17. ESP8266 Safety

Commandware must know hardware restrictions.

Most importantly:

**ESP8266 GPIO operates at 3.3 V and is not 5-V tolerant.**

That matters considerably when experimenting with your 5 V 74-series parts.

Also account for boot-strapping pins and pins used by UART/flash.

`pins` should eventually expose warnings such as:

```text
GPIO0   BOOT-SENSITIVE
GPIO2   BOOT-SENSITIVE
GPIO15  BOOT-SENSITIVE
```

Commandware should avoid pretending every GPIO is completely interchangeable.

---

# 18. MVP

Don't have Codex build the whole language initially.

### Phase 1

Implement:

```text
Terminal
Parser
Pipeline
Job scheduler
Pin manager

read
write
adc
clock

map
not
edge

print
shiftout

help
pins
jobs
kill
status
```

This should already allow:

```bash
write 5 1
```

```bash
read 4
```

```bash
clock 100 | write 5
```

```bash
read 4 --watch | not | write 5
```

and our canonical demo:

```bash
adc A0 | map 0 1023 0 255 | shiftout 5 4 14 msb
```

Only after this works cleanly should we add `shiftin`, debounce, edge variants, scripts, autorun, SPI/I²C abstractions, networking, etc.

---

# 19. MVP Acceptance Test

I'd make this the milestone for **Commandware 0.1**:

Wire:

```text
Potentiometer
      │
      A0
      │
   ESP8266
      │
 DATA CLOCK LATCH
      │
   74HC595
      │
 LED LED LED LED LED LED LED LED
```

Enter:

```bash
adc A0 --watch | map 0 1023 0 255 | shiftout 5 4 14 msb
```

Turn the potentiometer.

**The eight LEDs should display the mapped ADC value in binary, while the terminal remains responsive and allows `jobs`, `pins`, and `kill`.**

If that works, the fundamental Commandware architecture works.

And from there we can get to the really interesting goal: using the same primitives to express much of **Checkmate-64's hardware behavior without writing project-specific ESP8266 C++ firmware.**
