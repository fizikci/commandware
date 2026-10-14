# Commandware language reference — draft

This document defines Commandware syntax and execution semantics. It is a draft
specification; the firmware and simulator may not yet implement all described
behavior.

The current implementation guide is [language.md](language.md).

## 1. Purpose

Commandware is an interactive hardware language built around events and pipes:

```text
source | transform | action
```

Use commands to read hardware, transform values, filter events, and perform
actions. Use blocks for decisions and ordered sequences that do not fit naturally
into one pipeline. Functions package reusable command sequences.

The core does not include classes, modules, general-purpose collection APIs,
exceptions, or unrestricted loops. Add features when a concrete hardware task
needs them. Hardware capabilities belong in commands rather than special syntax.

## 2. Programs and syntax

A submission contains statements. Newlines and semicolons separate statements;
`|` connects pipeline stages. A trailing pipe continues on the next line.

```text
adc A0 |
    map 0 1023 0 100 |
    print
```

Braces enclose command bodies. A comment starts with `#` at a token boundary and
continues to the newline. Command names are case-insensitive; variable, parameter,
field, and function names are case-sensitive.

Names use letters, digits, and underscores, start with a letter or underscore,
and cannot be `value`. A declaration or binding uses the bare name; reading it
requires `$name`.

Single- and double-quoted strings have identical behavior: they preserve spaces
and syntax characters and interpolate references. Supported escapes are `\n`,
`\"`, `\'`, `\\`, and `\$`. Unknown escapes are errors. Interpolation occurs
once; inserted text is never reparsed as code or interpolated again.

```text
print "Sample $count: $raw"
print "Literal reference: \$raw"
```

Commands define their expected arguments: names, hardware identifiers, option
names, and value expressions. Value arguments use expression-aware parsing,
not whitespace-only splitting. Whitespace is allowed inside expressions.

The parser consumes the complete expression, including any following binary
operators and their operands. It does not stop at the first valid value. A new
value that cannot continue the expression begins the next argument. Recognized
command options, a body-opening `{`, a pipe, or a statement boundary end the
argument where permitted by the command's syntax. An incomplete expression at
such a boundary is an error. Quoted strings protect their contents from these
syntax rules.

```text
every 100 |
    adc A0 |
    write 12 $value > 700 |
    oled chart
```

Here `write` receives two arguments: pin `12` and the result of the comparison.
It writes `1` when the reading exceeds 700, otherwise `0`, while preserving the
ADC event for `oled chart`. The comparison is one value argument despite its
spaces. The same expression rules apply to all commands:

```text
if $value > 700 { write 12 1 }
where $value > 700
set count $count + 1
write 12 $value > 700
```

Parentheses control expression grouping and can make argument boundaries
explicit; they are not required merely to include spaces. Nested parentheses
are supported, and unmatched parentheses are syntax errors.

### Signed arguments and subtraction

After an expression, `+` or `-` continues that expression as a binary operator,
regardless of surrounding spaces. For example, `$a - 1` and `$a -1` both mean
subtraction, not two arguments. To supply a separate signed argument, enclose
it in parentheses: `$a (-1)` is two arguments. At the start of an expression,
`-1` is an ordinary negative value.

This rule also applies to consecutive numeric arguments:

```text
map 0 1023 (-100) 100
```

The four arguments are `0`, `1023`, `-100`, and `100`. Without parentheses,
`1023 -100` would be one subtraction expression. The parser does not reinterpret
an expression to satisfy an expected argument count; missing or extra arguments
produce an error.

The parser recognizes `||` as a logical operator rather than two pipeline
separators. A single `|` always separates stages.

There is one grammar for interactive commands, function bodies, timer pipelines,
and scheduled bodies. A command may restrict its placement for a stated semantic
reason, but no execution context selects an older command language.

## 3. Values and expressions

There are three value kinds, with no type declarations:

| Kind | Examples | Main use |
| --- | --- | --- |
| Number | `0`, `-12`, `3.5` | Readings, arithmetic, durations, conditions |
| String | `"Ready"`, `"C4"` | Text and parsed input |
| List | Result of `split` | Sequential iteration and unpacking |

A missing primary value is an internal event state, not a fourth value kind.
It differs from `0`, `""`, and an empty list. Reading a missing `$value` is an
error. There is no `null` literal.

Numbers use IEEE 754 binary64 (64-bit floating point), with integers represented
exactly through magnitude `2^53`. Numeric literals accept decimal integers,
fractions, and exponents, such as `20`, `0.5`, and `1e3`, and hexadecimal integers,
such as `0x3c`. Unary `+` and `-` apply to these values.

Numeric-looking quoted text remains a string until used in a numeric operation.
Arithmetic, including unary signs, and ordering comparisons convert all string
operands to numbers using the conversion rules below, including when both
operands are strings. `+` is numeric addition, not string concatenation. Numbers
must be finite; division by zero and nonfinite results are errors. Division can
produce a fractional number.

When formatting a number as text, omit unnecessary `.0` and use enough significant
digits to recover the stored binary64 value when parsed again. Exponent notation
may be used for very large or small values. Firmware and simulator use the same
formatting rule.

Operators, from highest to lowest precedence:

| Operators | Meaning |
| --- | --- |
| Function calls, parentheses | Conversion and grouping |
| `!`, unary `+`, unary `-` | Negation and sign |
| `*`, `/`, `%` | Multiplication, division, remainder |
| `+`, `-` | Addition and subtraction |
| `<`, `<=`, `>`, `>=` | Numeric comparison |
| `==`, `!=` | Equality and inequality |
| `&&` | Logical and |
| `||` | Logical or |

Binary arithmetic associates left to right. Remainder uses a quotient truncated
toward zero. Comparison chaining is rejected: write `$a < $b && $b < $c`.
Equality (`==` and `!=`) compares two numbers numerically and two strings by
their text content. When one operand is a number and the other is a string, the
string is converted and the comparison is numeric. Ordering comparisons always
convert string operands, including when both are strings. List comparisons are errors.

```text
"20" + 1             # 21
5 == "5"             # 1
"20" > 5             # 1
"hello" > 5          # 0: numeric comparison of 1 > 5
"hello" == "world"   # 0: textual comparison
"20" + "5"          # 25
"20" > "5"          # 1: numeric ordering
"05" == "5"         # 0: different text
number("05") == number("5")  # 1: equal numbers
```

Conditions and logical operators convert strings to numbers using the same
conversion rules: zero is false and any other number is true. Thus `""`, `"   "`,
and `"0"` are false, while `"hello"` is true. Lists are invalid conditions.
Comparisons and logical operators produce `0` or `1`. Logical operators
short-circuit, so `$divisor != 0 && ($total / $divisor) > 10` is valid even when
the divisor is zero.

Built-in expression functions:

| Function | Contract |
| --- | --- |
| `int(x)` | Convert a string using the rules below, then truncate toward zero; truncate numeric input toward zero |
| `number(x)` | Convert a string using the rules below; accept a number unchanged |
| `notehz(x)` | Convert a note string such as `C4`, `F#4`, or `Bb3` to Hz |

String-to-number conversion follows these rules:

- Numeric text converts to its numeric value.
- An empty or whitespace-only string converts to `0`.
- Other nonnumeric text converts to `1`, rather than raising a conversion error.

Numeric text accepts surrounding whitespace, an optional sign, and the same
decimal or hexadecimal forms as numeric literals. The entire trimmed string must
match to be treated as numeric text; conversion does not accept just a numeric
prefix. Nonfinite numeric results remain errors.

```text
int("200.9")   # 200
int("hello")   # 1
int("")        # 0
int("   ")     # 0
int("20O")     # 1: nonnumeric text, not 20
```

Implicit conversion uses the numeric value without truncation; `int()` explicitly
truncates toward zero. Hardware commands validate their own ranges and integer
requirements rather than silently truncating arguments.

There are no list literals, indexing operators, implicit string concatenation,
or user-defined expression functions in this core. Interpolation composes text;
`call` invokes user-defined command functions.

## 4. Events and pipes

An event contains a primary value, named fields, and a timestamp. `$value` reads
the primary value. `$name` reads a field, variable, or function parameter.
Ambiguous names are errors rather than having a hidden lookup priority.

A pipe passes events, not printed text. Each downstream stage runs once for each
event it receives. Event emission and command completion are separate: a source
can emit many events before completing; a waiting operation can emit its event
only after it completes.

Ordinary stages preserve the incoming timestamp. Sources stamp events with their
actual emission time in milliseconds since device startup, or virtual startup
in the simulator. Timestamp metadata has no expression accessor in this draft.

| Command behavior | Primary value | Existing fields |
| --- | --- | --- |
| Producer or transform: `adc`, `read`, `map`, `split`, `toggle` | Replace with result | Preserve |
| Assignment: `set` | Replace with assigned value | Preserve |
| Observation/action: `print`, `write`, LCD/OLED drawing | Preserve | Preserve |
| Waiting action: `sleep`, `tone` | Preserve after completion | Preserve |
| Filter: `where` | Preserve or emit nothing | Preserve if emitted |
| Sequential block: `do` | Result of body | Result of body |
| Branch: `if` | Result of selected branch | Result of selected branch |
| Function: `call` | Result of body | Result of body |

The action rule means an explicit argument selects what is acted upon without
changing the event flowing onward:

```text
adc A0 | print "Reading: $value" | oled chart
```

An explicit value overrides piped input. `0` and empty text count as supplied
values. Where a command accepts both a positional value and `--value`, supplying
both is an error. If a required value is neither explicit nor piped, execution
fails.

At the start of a top-level statement, a command without a pipe receives an empty
event. Inside a block, every statement receives the block's incoming event,
independently of preceding statements' results. A producer or explicit
argument can supply a missing value; `print` without an argument requires one.

### Output bindings

`--out NAME` captures the command result in an event field without changing the
primary result. Several comma-separated names unpack a list of exactly that
length. Binding names must be unique and must not collide with a global variable
or a parameter of the current function call. Rebinding an existing field replaces that field for the outgoing
event; unrelated fields remain.

```text
adc A0 --out raw |
    map 0 1023 0 100 --out percent |
    print "$raw -> $percent"

split ":" "C4:200" --out pitch,duration
```

`--out` is the final option, before any body. Result-producing commands and
`call` support it; actions and control commands do not. `each` uses `--out` to
bind the current item.

## 5. Global variables and local parameters

```text
set count 0
set count $count + 1
```

`set NAME EXPR` creates a global variable if it does not exist, or replaces its
value if it does. It evaluates the expression before creating or updating the
variable and emits the assigned value as the new primary value. Fields and
timestamps survive assignment. There is no separate declaration command.

Reading an unknown variable is still an error: `set count $count + 1` requires
`count` to have a value already. A failed expression does not create a variable
or change its previous value.

```text
do {
    set bit 0
    every 500 | set bit !$bit | write 12
}
```

All variables persist between submissions until `reset`, regardless of where
they were created. `do`, `if`, `each`, functions, and scheduled bodies all access
the same global variable table. Blocks never copy that table or create local
variables. Repeated execution of `set count 0` resets the same global variable;
it is not an initialize-only operation.

Function parameters are local to each call. They do not create global variables
or require a copy of the global table. Variables set inside a function are
still global. A parameter name must not collide with a global variable or an
incoming event field; ambiguous references are errors, not implicit shadowing.
Event fields belong to events, not the global variable table.

All jobs share global variables and run cooperatively. One expression evaluation and
assignment completes before another job advances; a sequence spanning a wait
is not atomic. Job ordering is not a synchronization mechanism.

## 6. Blocks and sequential execution

A block executes statements sequentially. A statement is one command or a
pipeline; newlines and semicolons separate statements. Finite operations complete
before the next statement begins.

Every statement starts with the same event supplied to that block invocation.
Only an explicit `|` passes one command's output into another command. A statement's
primary value and field changes do not become input to the next statement.
Global variable updates and hardware actions do persist. Blocks do not introduce
variable scopes.

The outward result of a block is separate from statement-to-statement input.
For `do`, a selected `if` branch, and functions, the final statement's output is
the block's output. Earlier statement outputs do not feed later statements or
escape the block. An empty block forwards its incoming event. If the final
statement filters out its event, the block emits no event downstream.

```text
clock 10 | do {
    print "Taking a reading"
    adc A0
} | oled chart
```

Both statements receive the clock event, but the final statement produces an
ADC reading. That reading is the block's output and is passed to `oled chart`.
Placing a different statement last controls the output without changing the
input received by any statement.

| Construct | When the body runs | Incoming event |
| --- | --- | --- |
| `do` | Once per incoming event | That event |
| `if` | When its branch is selected | That event |
| `each` | Once per list item | Item value and upstream fields |

`do { STATEMENTS }` groups sequential commands and can be a pipeline stage:

```text
clock 10 | do {
    oled print $value
} | write 12
```

Each clock event runs the block once. In this single-statement example, OLED
printing preserves the clock event for downstream `write`. The next event waits until the
current block and its downstream stages finish. Standalone `do` runs once with
an empty event, or with its enclosing block's incoming event when nested.

```text
clock 10 | do {
    adc A0
    where $value > 700
    oled print "High"
}
```

Here `$value` in the `where` statement is the clock value, not the ADC reading.
The ADC result is not piped anywhere. A false `where` ends only its own pipeline;
`oled print "High"` still runs. To filter the ADC reading before printing, connect
those commands explicitly:

```text
clock 10 | do {
    adc A0 | where $value > 700 | oled print "High"
}
```

```text
do {
    tone 15 --hz 440 --dur 200
    sleep 100
    print "Finished"
}
```

Starting a background source or a schedule registers independent work and then
continues the block. Future child events are not inserted into later statements.

A filter suppresses downstream stages in its own pipeline only. It does not skip
later statements in a `do`, branch, function, or `each` body. A standalone `where`
is valid even though it has no downstream command to filter. A false condition
is not an error, a block exit, or cancellation of the source.

## 7. Decisions

### Filtering with `where`

```text
every 100 |
    adc A0 |
    where $value > 700 |
    print "High reading: $value"
```

`where EXPR` evaluates its condition once for each incoming event. True forwards
that event unchanged. False emits nothing; the downstream stages do not run for
that event. Later statements in the enclosing block still run, each with the
block's incoming event. A periodic source continues with its next trigger.

### Branching with `if`

```text
every 100 |
    adc A0 |
    if $value > 700 {
        write 12 1
    } else {
        write 12 0
    } |
    oled chart
```

`if EXPR { STATEMENTS } [else { STATEMENTS }]` evaluates the condition once and
executes exactly one branch. The selected branch receives the incoming event;
each statement independently receives that event. The final statement's output
continues downstream. A false condition without `else` forwards
the incoming event unchanged. An empty branch also forwards it unchanged.

`else` belongs to the preceding `if` and may begin on the next line. Nest `if`
inside `else` for additional cases; there is no separate `elif` keyword.

A branch must finish before downstream processing begins. Its assignments persist
in the global table, and its field changes survive in its output event.

Hysteresis can be expressed using state and two conditions, without a dedicated
language construct:

```text
do {
    set enabled 0
    every 100 |
        adc A0 --out raw |
        if $raw > 700 {
            set enabled 1
        } |
        if $raw < 600 {
            set enabled 0
        } |
        write 12 $enabled
}
```

This example operates on ADC counts; it does not imply a temperature driver.

## 8. Functions

```text
func pair text {
    split ":" $text
}

call pair "C4:200" --out pitch,duration |
    tone 15 --hz notehz($pitch) --dur int($duration)
```

Functions have named positional parameters and a sequential body. Arguments are
evaluated once at call entry and their count must match the parameter count.
Calls receive the incoming event and create local parameter bindings only.
They read and update the shared global variable table; no globals are copied.

The final statement's output is the call result. Earlier statement outputs are
not fed into later statements. There
is no `return` statement. An empty body forwards the incoming event. Filtering
inside a statement does not skip later statements. Parameter bindings disappear
when the invocation is no longer needed.
Global variable updates persist, and output fields continue to the caller.

Definitions are registered before a submission executes and persist between
submissions. Redefinition applies to calls that begin afterward. A suspended call
continues the definition it began with. Retaining active definitions must be
bounded by an implementation limit; replacement must fail clearly if it would
exceed that limit.

Sources started inside a function retain access to that invocation's parameter
bindings for as long as needed, without copying global variables. The call
finishes after its finite statements; it does not wait for independent child
sources to end.

## 9. Lists and sequential iteration

`split DELIMITER [VALUE | --value VALUE]` produces a list of strings, preserving
empty parts. An empty delimiter is an error. Its input must be a string; numeric
input is not silently formatted into text.

`each [--out NAME] { STATEMENTS }` requires a block and executes it sequentially
for each item of the incoming list. `each` without a block is a syntax error.

```text
split " " "A B C" |
    each --out letter {
        print $letter
    } |
    print "Finished all letters"
```

This prints A, B, C, and then "Finished all letters" once. Each item becomes the
body's primary value, receives the upstream fields, and retains the upstream
timestamp. `--out letter` additionally binds that item as an event field. The
entire body finishes before the next item begins. There is no parallel iteration.

After all iterations, `each` forwards the original list event exactly once,
including for an empty list. Per-item field changes do not escape. Variable
updates persist globally. Each statement in the body starts with the same item
event. Filtering within one statement suppresses only that statement's downstream
stages; later statements still execute. It does not suppress the final list event.

Per-item commands belong inside the block. Commands piped after its closing
brace run once after iteration finishes, not once per item.

## 10. Sources, timing, and scheduling

`every INTERVAL_MS` starts a background source. The interval must be a positive
integer. Its first event is emitted when first serviced; each event has primary
value `0` and fresh fields. It accesses global variables at execution time,
not a snapshot. It does not inherit fields from the registration event.

Each source processes one event at a time, including downstream waits. Overdue
ticks are skipped instead of queued without bound. Timer intervals are best-effort
service intervals, not guarantees about physical edge timing or sample delivery.

`clock` and `adc --watch` use the same events, expressions, and downstream grammar
as `every`. Their waveform or sampling parameters remain command-specific.
Starting a source is permitted as a statement in any body or as a stage after a
pipe. When a background source receives an upstream event, it starts a new
independent job for that event. The new job generates its own events for the
remaining downstream stages; it does not block the upstream source until the
new job ends.

```text
every 100 | clock 10 | write 12
```

Each timer event attempts to start another clock job whose output feeds `write`.
The usual job limits and pin-conflict checks still apply; a warning does not
reserve resources or suppress runtime errors.

During submission validation, obvious cases where a repeating source feeds
another background source produce a warning at the downstream source's line:

```text
Warning at line 1: each incoming event starts a new clock job.
```

Emit each such warning once per source location per submission, not on every
event. Warnings do not prevent execution. Validation need not predict all
indirect spawning through functions or data-dependent control flow.

`sleep DURATION_MS` and `tone PIN --hz HZ --dur DURATION_MS` wait cooperatively,
then forward their event. Durations are nonnegative integer milliseconds; a
zero-duration wait completes immediately. Tone frequency must be positive and
within the hardware command's supported range.

`schedule` registers bodies at offsets on one shared timeline:

```text
do {
    set offset 100
    schedule {
        $offset { tone 15 --hz 440 --dur 100 }
        300 {
            call pair "C4:200" --out pitch,duration |
                tone 15 --hz notehz($pitch) --dur int($duration)
        }
    }
    print "Registered"
}
```

Offsets are nonnegative integer milliseconds, evaluated once when the schedule
is registered. Entries access global variables at execution time, without copying
or snapshotting them. Each entry receives the registration event, with its
timestamp replaced by the actual start time; storage of event data is a separate
implementation concern from global variables. Bodies use the
ordinary grammar, including functions, branches, and iteration. Registering a
schedule does not wait for its entries to finish.

Invalid offsets and insufficient entry capacity reject the entire registration.
Hardware conflicts discoverable only at entry execution fail that entry's job;
other entries continue. Static resource preflight may catch definite conflicts
earlier, but must not claim to predict all data-dependent use.

Programmatically constructing schedule entries is outside this draft. A normal
command body may register a schedule using the grammar above.

## 11. Jobs, resources, and failures

Independent background work has a job ID. `jobs` lists work and retained failures;
`kill ID` cancels the selected job; `reset` stops all jobs and clears variables
while preserving function definitions. An active parent occupies a job slot.
Spawned independent jobs outlive normal completion or failure of their parent.
`reset` stops all pending schedule entries and running scheduled work. No separate
schedule-group cancellation mechanism is required by this reference.

GPIO ownership belongs to the executing job, not an individual event or branch.
A periodic job keeps its pins between ticks. Pin state persists until a command
explicitly changes it. Completion, cancellation (`kill`), failure, and software
`reset` do not write cleanup values to pins. For example:

```text
write 12 1
```

GPIO12 remains high after the command finishes, including after a later `kill`
or software `reset`. Ending a job releases its ownership without changing the
pin's configuration or last output value. A later command may acquire the pin
and change it, subject to device reservations and conflicts with active jobs.

Stopping a tone or clock stops future waveform transitions and leaves the pin
at its last driven level; there is no automatic transition to low. Software
`reset` stops work and clears variables, but does not restore hardware pin
states. A physical device reset follows the hardware and firmware startup behavior.

Reserved pins and conflicts fail explicitly. LCD/OLED bus reservations and
display contents retain their existing device lifetime, including across `reset`.
Device commands document those resource rules separately from syntax.

The complete submission is parsed and checked for known command names, expression
syntax, and argument structure before execution. Runtime values, name collisions,
and hardware availability are checked when reached. Unselected branches are not
evaluated, but their syntax must be valid.

Errors stop the affected job and release its resources without changing pin
states. Earlier hardware actions
are not rolled back; independent jobs continue. Errors identify the command's
source line, including delayed operations. Function errors also identify the
function; preserving a call-site location is desirable where memory permits.

Firmware retains the last eight failures in `jobs` until `reset`. Asynchronous
failures remain observable after an interactive request has returned.

## 12. Command reference

Commands are part of the language contract and follow the composition rules
above. Entries describe intended behavior, including behavior not implemented
yet. Firmware/simulator differences are tracked separately in the
[implementation checklist](language-implementation-checklist.md).

This first catalog installment defines `adc`, `clock`, `map`, `print`, `read`,
and `write`. Entries for other commands remain to be drafted. Brackets denote
optional syntax, uppercase words denote arguments, and alternatives separated
by `|` inside a syntax description are choices, not literal pipeline operators.

### Command inventory

| Group | Commands |
| --- | --- |
| Hardware input | `read`, `adc`, `shiftin` |
| Hardware output | `write`, `shiftout`, `tone` |
| Display | `lcd init`, `lcd print`, `lcd clear`; `oled init`, `oled print`, `oled chart`, `oled clear` |
| Transformation | `map`, `not`, `toggle`, `split`, `ohm` |
| Event filtering | `where`, `edge`, `rising`, `falling`, `debounce`, `divide` |
| Signal shaping | `pulse` |
| Timing | `clock`, `every`, `sleep`, `schedule` |
| Structure and state | `do`, `if`/`else`, `func`, `call`, `each`, `set` |
| Console and management | `print`, `help`, `pins`, `jobs`, `kill`, `status`, `reset` |
| File execution | `run` (currently simulator-only; device support remains future work) |

`else` is part of `if` syntax, not an independently invocable command. `var` is
excluded from the language. Expression functions `int()`, `number()`, and
`notehz()` are defined in section 3 rather than as pipeline commands.

### Alphabetical index of drafted entries

[adc](#adc) · [clock](#clock) · [map](#map) · [print](#print) ·
[read](#read) · [write](#write)

### adc

```text
adc A0 [--out NAME]
adc A0 --watch
```

**Input and arguments.** `A0` identifies the ESP8266 analog input. One-shot `adc`
ignores the incoming primary value and takes a new hardware reading each time
it executes. It does not reinterpret the incoming value as an ADC sample.

**Output.** One-shot `adc` replaces the primary value with an integer reading
from `0` through `1023`, preserving incoming fields and timestamp. `--out` also
binds that result as an event field.

**Execution.** Without `--watch`, one reading completes before downstream work
begins. `--watch` starts an independent background source that samples every
100 ms. It emits the first sample and then only samples whose raw reading differs
from the preceding sample. Its emitted events carry the reading, fresh fields,
and the actual emission timestamp. It accepts the same downstream grammar as
other sources. Use `--out` on one-shot `adc` to name readings; named bindings on
source forms remain to be specified with the other source command entries.

**Hardware and errors.** This command reads A0 without claiming a digital GPIO.
Other analog identifiers, unsupported options, and unavailable background job
capacity are errors. Device-specific ADC voltage limits belong in the wiring
guide; the returned count is not a voltage measurement in physical units.

```text
adc A0 | print
every 100 | adc A0 --out raw | print "Raw: $raw"
adc A0 --watch | oled chart
```

### clock

```text
clock HZ [--dur MILLISECONDS]
```

**Input and arguments.** `HZ` is a numeric expression in the range `0.01` through
`1000000`, inclusive, representing complete cycles per second. `--dur` is an
optional integer duration from `1` through `2147483647` milliseconds. Without
`--dur`, the source continues until stopped. The existing optional direct-pin
shorthand is not specified in this entry pending review; use an explicit `write`.

**Output.** The source alternates primary values `1`, `0`, `1`, `0`, with one
event per half-cycle. Each emitted event has fresh fields and its actual emission
timestamp. At 10 Hz, the nominal interval between events is 50 ms. The source
does not forward an upstream primary value as its waveform.

**Execution.** Registration starts an independent background job and does not
wait for its duration to expire. Placing `clock` after a pipe starts a new job
for each upstream event. Downstream work runs sequentially per emitted event;
busy work skips overdue transitions rather than queuing an unlimited burst.
Accepted frequency ranges describe validation, not a physical timing guarantee.
Exact first-event phase remains to be specified consistently across runtimes.

**Hardware and errors.** `clock` itself generates events; downstream actions
claim hardware pins. Ending generation leaves those pins at their last driven
levels. Invalid frequency/duration, duplicate options, unavailable job capacity,
and downstream pin conflicts are errors. Obvious repeated source spawning
produces the submission warning defined in section 10.

```text
clock 10 | write 12
clock 440 --dur 200 | write 15
clock 10 | do {
    oled print $value
}
```

### map

```text
map IN_MIN IN_MAX OUT_MIN OUT_MAX [VALUE | --value VALUE] [--out NAME]
```

**Input and arguments.** The four range endpoints and the selected value are
numeric expressions, with string operands converted using section 3. An explicit
positional value or `--value` overrides the incoming primary value; supplying
both is an error. Without an explicit value, an incoming primary value is required.

**Output.** Replace the primary value with:

```text
int(OUT_MIN + (VALUE - IN_MIN) * (OUT_MAX - OUT_MIN) / (IN_MAX - IN_MIN))
```

The final result truncates toward zero; intermediate calculations retain their
numeric precision. Values outside the input range are extrapolated, not clamped.
Reversed ranges are valid. Preserve incoming fields and timestamp. `--out` also
binds the result.

**Execution, hardware, and errors.** One-shot transformation with no hardware
ownership. Equal input endpoints, missing input, unsupported list operands,
nonfinite results, and invalid argument structure are errors.

```text
adc A0 | map 0 1023 0 100 | print
map 0 1023 0 100 512 | print
adc A0 --out raw |
    map 0 1023 0 100 --value $raw --out percent |
    print "$raw -> $percent"
```

### print

```text
print [VALUE | --value VALUE]
```

**Input and arguments.** Print an explicit value if supplied; otherwise print
the incoming primary value. Supplying both positional and named values is an
error. Numeric and string values are supported; numeric formatting follows
section 3. List formatting is not yet specified. Empty text and zero are valid.

**Output.** Write the selected value followed by a newline to the command output
channel. Forward the incoming event unchanged, even when explicit text or a
different value was printed. Printing does not produce the printed text as a
replacement primary value.

**Execution, hardware, and errors.** One-shot observation with no GPIO ownership.
The submitting console or asynchronous serial output receives text according to
the transport. Missing both explicit and incoming values, unsupported value
kinds, and invalid argument structure are errors. `--out` is not supported.

```text
print "Ready"
adc A0 | print "Raw: $value" | oled chart
```

For a standalone `print "Ready"`, the console receives text but the forwarded
event still has no primary value.

### read

```text
read PIN [--out NAME]
```

**Input and arguments.** `PIN` is an integer numeric expression identifying a
digital GPIO. This entry uses numeric GPIO identifiers, not board labels such
as `D1`. Ignore the incoming primary value and take a fresh reading when executed.
There is no `--watch` form in this entry; use `every` for repeated readings.

**Output.** Replace the primary value with `0` for low or `1` for high. Preserve
incoming fields and timestamp. `--out` also binds the result.

**Execution and hardware.** One-shot read; configure the pin as input and claim
it for the executing job. Input pull-up configuration is not part of this syntax.
Ownership lasts until that job ends; releasing ownership does not drive the pin
or change its configuration.

**Errors.** Fractional or invalid pin identifiers, reserved pins, conflicting
active owners, and invalid arguments fail explicitly. On the ESP8266 target,
script-accessible digital pins are GPIO0, 2, 4, 5, 12, 13, 14, 15, and 16,
subject to device reservations. UART and flash pins are unavailable.

```text
read 12 | print
every 100 | read 12 --out button | print "Button: $button"
```

### write

```text
write PIN [VALUE | --value VALUE]
```

**Input and arguments.** `PIN` uses the same identifier and reservation rules as
`read`. Select the explicit value if supplied, otherwise the incoming primary
value. Convert strings using section 3. Zero drives low; any other finite numeric
value drives high. Supplying both positional and named values is an error.

**Output.** Preserve the incoming event's primary value, fields, and timestamp.
The physical output level does not replace the event value. An explicit value
does not interpret incoming data as a duration.

**Execution and hardware.** One-shot action; claim the GPIO for the executing job,
configure it as output, and set its level. The pin retains its last level after
normal completion, cancellation, failure, or software `reset`. Job ownership
ends when the job ends. A periodic job retains ownership between events.

**Errors.** Missing both explicit and incoming values, unsupported list values,
invalid/reserved pins, conflicting owners, and invalid arguments are errors.
`--out` is not supported. Timed-write syntax is not defined in this entry;
the existing duration forms require separate review.

```text
write 12 1
write 12 0
every 100 | adc A0 | write 12 $value > 700 | oled chart
```

In the final example, the comparison selects the GPIO level while `oled chart`
still receives the raw ADC reading.

## 13. Bounds and simulator agreement

Existing firmware bounds are the initial implementation targets, not expanded
budgets authorized by this draft:

| Resource | Current bound |
| --- | --- |
| Submission | 4,096 source bytes |
| Parsed commands | 128 |
| Nested blocks | 8 |
| Nested function calls | 8 |
| Function definitions | 16 |
| Global variables | Proposed: 32 total, replacing the current per-scope bound |
| Named fields per event | 16 |
| List elements | 64 |
| Pending continuation frames | 128 |
| Script jobs | 8 |
| Clock/ADC slots | 8, currently separate |
| Pending schedule entries | 32 |

Exceeding a bound produces a clear error. No silent truncation or dropped work is
allowed except the documented skipped periodic ticks. Source- and event-specific
resource counts must be documented if separate pools remain internally.

The simulator must agree with firmware on value kinds, event propagation, scope,
branch selection, ordering, and error behavior. Virtual time may bound unlimited
sources with a sample count. It does not establish physical timing fidelity.
Differences in resource planning or scheduling must be listed explicitly, not
treated as alternative language semantics.

