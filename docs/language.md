# Commandware scripting

The firmware and Python simulator support the first scripting milestone:
structured events, variables, `do`, `every`, functions, sequential `each`, and
cooperative `tone`/`sleep`, and OLED text/scrolling charts. General sensor
drivers, PWM, and hysteresis remain future hardware commands. The HD44780 `lcd`
also works inside scripts.

## Start here: blink while playing two notes

Paste this complete program into the web console or serial console. It is also
saved as [notes-and-blink.cw](examples/notes-and-blink.cw).

```text
func notes str {
    split " " $str |
        each --out note {
            split ":" $note --out pitch,dur |
                tone 15 --hz notehz($pitch) --dur int($dur)
        }
}

do {
    every 500 | toggle | write 12
    call notes "C4:200 E4:300"
    print "Melody finished; LED continues blinking"
}
```

GPIO12 is the LED output and GPIO15 is the tone output. Select suitable pins
for your circuit and preserve boot-pin levels as described in the tutorial.
The first note lasts 200 ms; the second begins after it finishes and lasts
300 ms. The LED has its own job. `jobs`, `status`, and `kill ID` remain usable
while the melody plays. No firmware upload is needed between command submissions.

In the simulator, `run_stream(program, hardware=hw, count=4)` takes four samples
from each unlimited source and runs finite work to completion on virtual time.
`hw.tone_history` contains `(start_ms, pin, frequency_hz, duration_ms)`;
`hw.script_writes` contains `(time_ms, pin, value)`. Simulation does not predict
physical waveform fidelity. For manual stepping, use `ScriptRuntime(hw)`,
`submit(program)`, then `tick(time_ms)` with increasing virtual timestamps.

## Statements, blocks, and pipes

```text
do {
    lcd init
    lcd print "Ready"

    every 100 |
        adc A0 |
        lcd print
}
```

Newlines and semicolons separate statements. A trailing `|` continues a
pipeline onto the next line. Braces delimit command bodies. Comments start
with `#` at a token boundary and end at the next newline. Single- and
double-quoted strings preserve spaces, pipes, and braces and support `\n`,
`\"`, `\'`, and `\\` escapes. Both quote styles interpolate `$name` in scripts.
The legacy direct LCD command retains its existing quoting behavior.

`do` executes statements in order. Finite operations finish before the next
statement starts. `every`, legacy `clock` (including `--dur`), ADC `--watch`, and
`schedule` start background work and let the block continue. The enclosing
script can itself remain a background job while waiting for finite operations.

An error stops the affected script job and releases its pins. Already-started
independent jobs continue. There is no rollback of earlier hardware operations.
The complete program is parsed and command names and basic argument shapes are
checked before execution, including expression syntax. Expression values, name collisions, and hardware
availability are checked when the relevant command runs. A function definition
can therefore contain a bad value that is reported only when the function runs.

Asynchronous firmware output and errors go to serial after the submitting web
request has returned. The last eight script failures are also retained as
`FAILED` entries in `jobs` and the web jobs list until `reset` clears them.

## Values, fields, and variables

```text
var time 0
set time $time + int($dur)
```

`var NAME EXPR` creates a variable or replaces its value if the name already
exists in the current scope. `set NAME EXPR` requires an existing variable.
For example, `var count 10` followed by `var count 0` resets `count` to 0.
There is no `=` token. A name in a declaration or output binding introduces
that name; `$name` reads its value. The remainder of a `var`/`set` command is
one expression. Other command arguments use compact expressions such as
`int($dur)` or `($time+100)` without spaces.

Expressions support numbers, references, parentheses, unary `+`/`-`, and
`+ - * / %`, with normal precedence. Unary `!` returns 1 for zero and 0 for
nonzero values, and binds more tightly than arithmetic. Read its operand with
`$name`, for example `!$bit`. Division produces a fractional result;
`int()` truncates toward zero and can convert numeric text. `notehz()` accepts
notes such as `C4`, `F#4`, and `Bb3` (octaves 0 through 8). Unknown names,
nonfinite results, division by zero, and unsupported expression operations are
errors. This is a small expression parser, not a general-purpose evaluator.

Each pipeline event has a primary value, named fields, and a timestamp. `$value`
is reserved for the primary value. `every` creates a fresh event for each tick;
it emits its first trigger as soon as the new job is serviced. Later triggers
are separated by the requested interval. A busy pipeline handles one event at
a time, skips overdue ticks, and never queues an unlimited catch-up burst.

```text
do {
    var count 0
    every 100 |
        adc A0 --out raw |
        map 0 1023 0 100 --value $raw --out percent |
        set count $count + 1 |
        print "Sample $count: $raw -> $percent"
}
```

`adc` inside a pipeline takes a new hardware reading for each trigger. `map`
normally consumes the primary value; `--value` selects another value. `--out`
must be the final option and adds named fields while preserving existing ones.
The primary value remains the command's result. `set` forwards the event
unchanged. Numeric script `map` results truncate toward zero.

To toggle a variable and write its updated value, select it explicitly:

```text
var bit 0
every 500 | set bit !$bit | write 5 $bit
```

`!bit` is an invalid expression because reading a variable requires `$bit`.
`write 5` without a value would consume the original timer event, since `set`
preserves that event rather than replacing its primary value.

One output name binds the entire result. Several comma-separated names unpack
a list whose length must match exactly:

```text
split ":" --value "C4:200" --out pitch,dur |
    print "Pitch $pitch, duration $dur"
```

Duplicate output names and collisions between variables and fields are errors.
Names are case-sensitive; command names are case-insensitive. A `do` block gets
a local copy of its enclosing variables. Its jobs share that local scope and
retain it after the submitting block completes. Separate block executions have
separate scopes. Top-level variables persist between submissions until `reset`.
Functions have their own parameter/local-variable scope; they receive event
fields but do not implicitly inherit the caller's variables. Pass configuration
as parameters. Concurrent jobs sharing a variable execute cooperatively in loop
order; use independent scopes when their counters should be independent.

## Functions and sequential iteration

```text
func pair str {
    split ":" $str
}

call pair "C4:200" --out pitch,dur |
    print "$pitch $dur"
```

Functions have named positional parameters and command bodies. Definitions are
registered before the program starts and persist between submissions; redefining
a function affects subsequent calls. `call` works directly or as a pipeline
stage. Function statements share the current event. The final primary value and
all resulting fields continue downstream. No separate `return` is needed for
this milestone. Function-local variables disappear when no job retains them.

`split DELIM [VALUE | --value VALUE]` returns one list. Supply its value as the
second positional argument, with `--value`, or through the pipe. An explicit
value takes precedence over the pipe; zero and an empty string are valid inputs.
It preserves empty parts and rejects an empty delimiter. `each` consumes that
list sequentially:

```text
split " " "A B C" |
    each --out letter |
    print $letter |
    sleep 100
```

It prints A, waits 100 ms, prints B, waits, then prints C. Downstream processing
must complete before the next element advances. Each item receives a fresh
copy of the upstream fields and its own primary value. Items are expanded
lazily in firmware to avoid allocating a whole pipeline per list element.

The block form runs its body once per element:

```text
split " " --value "A B C" |
    each --out letter {
        print $letter
        sleep 100
    }
```

If a pipeline follows the closing brace, it runs once after all iterations with
the original list event. Fields created within an iteration do not escape into
other iterations or that final event. Variables updated in the shared scope do
persist. There is no parallel iteration option in this milestone.

## Pending operations and jobs

```text
tone 15 --hz 440 --dur 200
sleep 100
```

Inside one `do` block or function these commands produce a 200 ms tone followed
by 100 ms of silence. Separate interactive submissions are independent jobs.
Both operations return control to the device loop while pending. Completion
resumes the waiting command sequence; no blocking `delay()` is used.

Emission and completion are separate concepts: a source may emit many events
before completing, whereas a pending sink may have no output at all. The
runtime represents waiting through a deadline and suspended continuation.
Each script job receives at most 32 interpreter steps per firmware loop. Tone
edges are serviced cooperatively with microsecond deadlines, with the same
software timing limitations as existing clocks.

GPIO resources belong to the job through completion, cancellation, or error.
Script reads/writes/tones reject UART, flash, LCD-reserved, and conflicting job
pins. Output pins return low when their owner ends; input pins are released
without being driven. The shared LCD reservation retains its existing lifetime.
`kill ID` stops one job; `reset` stops all work and clears variables while
preserving function definitions. `pins` identifies script-owned GPIOs.

Existing scalar `clock` and ADC `--watch` pipelines can start inside `do`, but
their downstream stages still use the legacy pipeline grammar. Use `every`
when you need named fields, function calls, or other scripting stages.

## OLED text and charts

```text
oled init ssd1306
oled print "hello"
oled print 2 2 "hello"
oled print "hello" --x 2 --y 2
adc A0 | oled print 0 16
oled clear
```

Coordinates are pixels and default to `--x 0 --y 0`. Positional `X Y` and named
coordinates are alternatives; do not mix them. A text/value argument or
`--value VALUE` overrides the incoming primary value. Without one, `print` and
`chart` require a piped value; zero and empty text are valid. Both commands
preserve the pipeline event and all its fields for downstream commands.

`oled init [ssd1306|sh1106] [--address 0x3c|0x3d] [--height 32|64] [--width 128]`
selects the I2C controller and clears the display/chart state. Defaults are
SSD1306, 128x64, and address `0x3c`. The first drawing command initializes those
defaults automatically. The controller is selected explicitly, not detected.

Text uses a 6x8 pixel cell (5x7 ASCII glyph with spacing), wraps to the starting
X coordinate, and clips at the bottom. `\n` advances one text row. Printing
overwrites its character cells while retaining other content; use `oled clear`
to remove old text when replacing it with a shorter string. Unsupported UTF-8
bytes render as `?`. `oled clear` also removes chart histories.

```text
do {
    oled init sh1106
    every 200 | adc A0 | oled chart --min 0 --max 1023
}
```

`oled chart [X Y] [VALUE] [--min 0 --max 1023] [--x 0 --y 0]
[--width WIDTH --height HEIGHT]` draws a scrolling trace. The default rectangle
extends from `(x,y)` to the display's bottom-right corner. Each sample shifts
that rectangle left one pixel and draws a step connecting the previous and new
value at the right edge. Values outside the range are clipped; `min` must be
less than `max`. The horizontal axis counts samples, so `every 200` corresponds
to 200 ms per pixel when processing keeps up.

Up to four non-overlapping rectangles can retain independent traces. A
rectangle must fit the display and be at least 2x2 pixels. Its `(x,y,width,height)`
identifies its history; changing its range clears that trace. Use `oled clear`
before changing to overlapping chart regions. Labels can occupy other areas.
See [two chart areas](examples/oled-charts.cw) for a complete ADC example that
shows raw counts and percent from the same reading. Separate sensor readings
can use the same arrangement when their drivers are available.

OLED commands also work in legacy clock/ADC-watch pipelines and scheduled
entries. The firmware updates a shared 1,024-byte framebuffer immediately and
transfers it in short I2C packets from the device loop. Screen refresh and the
initial power-up wait complete asynchronously; fast producers can replace an
intermediate frame before it reaches the physical display. The console remains
serviceable between transfers, though software tone/clock timing can still
vary with I2C activity. The Python simulator renders immediately into
`HardwareState.oled.buffer` and exposes `text_history` and `chart_history`.

SDA is GPIO4 and SCL is GPIO5. Once initialized, those pins remain reserved,
including after `kill` or `reset`; display contents also remain visible. An LCD
and OLED can share those pins if their addresses differ. OLED initialization
rejects existing GPIO owners, output pulses, and queued jobs using the bus pins.
Communication failures include the drawing command's line. A transfer failure
pauses refresh until `oled init` succeeds; subsequent drawing commands fail,
stopping their affected background jobs.

## Scheduling

```text
do {
    var offset 100
    schedule {
        $offset { clock 1000 --dur 200 | write 15 }
        300 { clock 1200 --dur 200 | write 15 }
    }
    print "Notes scheduled"
}
```

One `schedule` block uses one shared timeline, starting when that block is
executed. References are resolved before the entries are queued. Existing
capacity, overlap, and cancellation rules apply. Scheduled entries retain the
legacy command grammar: script bodies and iteration inside a schedule are not
supported. Generating a dynamic schedule from a loop is a future extension;
the `notes` function uses sequential completion instead.

## Error locations

Errors include `at line N`, with lines numbered from 1 in each submitted program
or loaded script. Comments and blank lines count. Delayed errors retain the
failing command's line, and functions retain lines from their definition even
when called in a later submission. Legacy background jobs report their source
command's line; legacy pipeline stages share that command location.

For example, a `split " "` on line 5 without an explicit or piped value reports:

```text
split: value is required through pipe or explicit parameter --value at line 5
```

Firmware stores 16-bit line numbers on parsed commands and active jobs, rather
than retaining source text for error reporting. Token locations fit existing
alignment padding without increasing token size.

## Limits and simulator boundaries

Firmware limits: 4,096 source bytes per submission, 128 parsed commands,
8 nested blocks, 8 nested function calls, 16 function definitions, 32 variables
per scope, 16 named fields per event, 64 list elements, 128 pending continuation
frames, and 8 script jobs. The existing 8 clock/ADC slots and 32 pending schedule
entries remain separate. The parent script occupies a slot while active.

The Python simulator uses virtual time and bounds unlimited sources to `count`
samples (one by default). It has a 10,000-operation guard for script runs and
does not reproduce firmware instruction budgets, microsecond edge jitter, or
schedule preflight/resource planning against concurrent script jobs. Firmware
native tests exercise those real scheduling/resource checks. For multi-line
Python CLI input, pass the whole program as one argument, or use
`python -m commandware run docs/examples/notes-and-blink.cw` with `src` on
`PYTHONPATH`. Firmware file loading with `run FILE` and boot autorun are still
future work.
