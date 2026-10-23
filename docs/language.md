# Commandware language guide

Commandware uses one event-and-pipe language in the console, functions, periodic
jobs, and scheduled bodies. The [language reference](language-reference-draft.md)
is the authoritative contract; the [tutorial](tutorial.md) explains practical
circuits and programs. Both Python and portable firmware have shared conformance
tests. Physical timing, display transfers, and ESP8266 peak memory remain board
verification tasks.

## A complete program

```text
func notes text {
    split " " $text |
        each --out note {
            split ":" $note --out pitch,dur |
                tone 15 --hz notehz $pitch --dur int $dur
        }
}
do {
    every 500 | toggle | write 12
    notes "C4:200 E4:300"
    print "Melody finished; LED continues blinking"
}
```

GPIO12 is the LED output; GPIO15 is the buzzer output. Use suitable circuits and
preserve boot-pin levels. The LED runs independently while the two notes execute
in order. Use `jobs` to find its job ID and `kill ID` to stop it. Stopping a job
releases ownership and preserves the last pin level; explicitly write low if
that is the required final state. See [notes-and-blink.cw](examples/notes-and-blink.cw).

## Syntax and expressions

Newlines and semicolons separate statements; `|` connects stages. Put a trailing
pipe before a newline to continue a pipeline. Braces delimit bodies; `#` starts a
comment at a token boundary. Single and double quotes both interpolate `$name`.
Supported escapes are `\n`, `\"`, `\'`, `\\`, and `\$`. Interpolation happens once.

Variables and fields always use `$` when read. `set` creates or updates a global;
there is no `var`, assignment `=`, or `call` prefix. Built-in names ignore case;
user function names, variables, parameters, and fields preserve case.

```text
set threshold 700
max 5 min 10 3 | print
max --b min --a 10 --b 3 --a 5 | print
adc A0 | clamp $value 0 $threshold --out limited | print $limited
```

Functions consume fixed parameter counts, so nested calls need no parentheses.
Parentheses still control grouping. Binary operators continue the current
argument; `max 5 -1` is missing an argument because `5 - 1` is one expression.
Use `max 5 (-1)` for two arguments. Named and positional function arguments
cannot be mixed. Nested calls cannot use `--out`.

Expressions support unary `+ - !`, arithmetic `+ - * / %`, comparisons
`< <= > >= == !=`, and short-circuit `&& ||`. Numbers are finite binary64;
strings convert strictly for arithmetic/conditions. Invalid text is an error,
not truthy data. Two strings compare textually with `==`; mixed number/string
equality converts numerically. Lists cannot be compared. `+` does not concatenate
strings; use interpolation or `join`.

Pure functions include `number`, `int`, `string`, `abs`, `min`, `max`, `clamp`,
`round`, `floor`, `ceil`, `len`, `contains`, `trim`, `lower`, `upper`, `slice`,
`replace`, `at`, `join`, and `notehz`. They work in expressions and as stages.
All parameters are explicit, including `$value` for piped input. User-defined
functions are command stages and cannot appear inside expressions.

```text
round (-2.5) | print
len "é雪😀" | print
split ":" "C4:E4:G4" | join $value ", " | print
```

Strings use UTF-8 scalar indexing; case conversion and trimming are ASCII-only.
Round ties away from zero. Indexes are nonnegative numeric integers; invalid
ranges error. Lists are immutable values produced by `split`; no list literal
or mutation syntax is provided.

## Events and bodies

An event has an optional primary value, named fields, and a timestamp. `$value`
reads its primary value; reading a missing value errors. `--out NAME` adds or
replaces a field without changing the primary. Multiple names unpack a list.
Fields cannot collide with globals or current parameters. Assignments cannot
target fields/parameters either; parameters are read-only.

Transforms replace primary values; actions such as `write`, `print`, and display
commands preserve the event. Explicit action data takes precedence over piped
data and never replaces it. Commands do not print implicitly.

```text
adc A0 --out raw |
    map 0 1023 0 100 --out percent |
    write 12 $raw > 700 |
    print "$raw -> $percent"
```

Each body statement independently receives the original input event. Globals
are shared immediately, but values/fields from one statement do not become the
next statement's input. The final statement supplies the block result. Empty
bodies forward their input; a filtered final statement emits nothing. A false
`if` without `else` forwards its input.

```text
number 7 | do {
    number 3
    print
} | print
```

This prints `7` twice: `print` observes and preserves its own input. Delayed
outputs from a final statement also flow through enclosing block/function
continuations. Filtering never skips later sibling statements.

```text
func choose reading {
    if $reading > 700 { number 1 } else { number 0 }
}
adc A0 | choose $value | write 12
split ":" "C4:E4" | each { notehz $value | print }
```

`each` requires a body and runs it sequentially for each item, then forwards the
original list once. A finite statement/item completes its pending timer work
before the next sibling statement/item starts. Direct periodic pipelines can
still retrigger pending signal timers. Functions register before execution, including definitions
later in a submission. A failed validation changes no definitions. A redefinition
affects new calls; suspended calls finish their original body.

## Sources, signals, and schedules

```text
every 100 | adc A0 | print
clock 2 --dur 1000 | write 12
adc A0 --watch --out reading | print $reading
```

Sources register independent jobs and emit no immediate event into their caller.
Their future events have fresh fields and actual service timestamps. `every`
emits `0` at first service; `clock` first emits high after one half-period; ADC
watch samples every 100 ms and emits first/change readings only. Each supports
`--out`. Fixed timelines skip missed deadlines and never build catch-up bursts.
At clock duration expiry an in-progress downstream event finishes; `kill`/`reset`
cancel it immediately.

`toggle` flips on every event. `edge`, `rising`, and `falling` first establish a
Boolean baseline, then filter transitions. `divide N` emits baseline `0`, then
toggles after each N input transitions. `debounce MS` accepts a Boolean level
after it remains stable. `pulse MS` emits high on any event and later low;
retriggering extends the low deadline without another high. State persists per
job/stage; every function invocation gets fresh body-stage state.

```text
schedule {
    0 { write 12 1 }
    100 { write 12 0 }
    200 { print "Done" }
}
```

Offsets are expressions evaluated once at registration. Entries capture the
registration event/parameters but read live globals. Their timestamps become
the actual start time. Scheduled bodies support normal commands and nested
bodies. Hardware conflicts fail the affected entry; independent jobs continue.

## Saved scripts

The device is the source of truth for the editor's Save and Open actions. The
same LittleFS files are available through pipes:

```text
string 'print "Hello"' | script save demo
script list | print
script load demo | print
script load demo | script save backup
script delete backup
```

`save` consumes a string and preserves the event; `load` emits source without
executing it. `list` emits sorted names; neither prints implicitly. `delete`
preserves the event. Names allow 1–28 ASCII letters, digits, spaces, `_`, and `-`,
without surrounding spaces. Quote names with spaces or only digits. Up to 32
scripts of 4,096 UTF-8 bytes each fit, subject to available flash. A failed save
preserves the previous file. Saving an empty string is allowed.

LittleFS requires explicit initial provisioning; mount failures never trigger
formatting. Saved scripts survive reboot and firmware updates using the same
filesystem layout, but erasing flash or uploading a filesystem image can remove
them. Simulator storage lasts only for its runtime instance, including `reset`.
See the [reference](language-reference-draft.md#script) for errors and web transport.

## Limits and verification

Submissions allow 4,096 UTF-8 bytes; at most 128 command AST nodes remain
retained across definitions/tasks/timers. Blocks/calls nest to 8 and
expressions to 16. There are 16 functions, 32 globals, 16 fields per event,
64 list items, 4,096 bytes per string, eight active jobs, and 32 queued entries.
There are 128 pending continuation frames total across jobs/timers.
Initial retained budgets are 8,192 value-payload bytes and 16,384 source bytes;
see the reference for accounting and physical-memory limitations.

`help`, `pins`, `status`, and `jobs` print diagnostics and preserve events.
`reset` cancels all work and clears globals/failures while keeping definitions and
hardware state. Errors include source lines and stop only the affected job.
`run "PATH"` is simulator-only.

OLED graphics use `oled rect X Y WIDTH HEIGHT [--value 0|1]`: fill a rectangle
(default on) or erase it (`--value 0`), preserving the incoming event. The
rectangle must fit the display. See [two-player Pong](examples/pong.cw) for a
complete game using GPIO12/14 buttons with external pull-ups.

The simulator uses virtual time; `run_stream(program, hardware=hw, count=4)` bounds
unlimited sources to four emissions. For manual stepping use `ScriptRuntime(hw)`,
`submit(program)`, then increasing `tick(time_ms)` timestamps. Hardware histories
record simulated writes/tones; they do not establish electrical timing accuracy.
Run Python tests, the native firmware harness, and web tests before flashing.
