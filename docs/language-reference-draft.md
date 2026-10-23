# Commandware language reference

This document defines the shared Commandware syntax and execution contracts.
The simulator and portable firmware runtime are checked against shared conformance
cases. Physical timing, device transfers, and peak ESP8266 heap use still require
board validation. The historical filename is retained for existing links.

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
continues to the newline. Built-in command and value-function names are
case-insensitive; variable, parameter, field, and user-defined function names
are case-sensitive.

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
"hello" > 5          # Error: invalid numeric text
"hello" == "world"   # 0: textual comparison
"20" + "5"          # 25
"20" > "5"          # 1: numeric ordering
"05" == "5"         # 0: different text
(number "05") == (number "5")  # 1: equal numbers
```

Conditions and logical operators convert strings to numbers using the same
conversion rules: zero is false and any other number is true. Thus `"0"` is
false and `"2"` is true; `""`, `"   "`, and `"hello"` are errors. There is no
separate permissive text-truthiness rule. Lists are invalid conditions.
Comparisons and logical operators produce `0` or `1`. Logical operators
short-circuit, so `$divisor != 0 && ($total / $divisor) > 10` is valid even when
the divisor is zero.

### Function invocation syntax

Functions use space-separated arguments, whether built-in or user-defined.
Arguments are either all positional or all named; mixing the two forms is an
error. Named arguments use `--PARAMETER VALUE`, match the parameter names exactly,
and may appear in any order. Unknown, duplicate, missing, or extra arguments are
errors. `--out` is an output binding, not a function parameter.

```text
max 5 10
max --a 5 --b 10
max 5 (min 10 20)
max --a 5 --b (min --a 10 --b 20)
set limit (max 5 10)
write 12 $value > (max $low $high)
max ($reading + 5) ($threshold * 2)
```

Nested value-function invocations need no parentheses. Each function has a fixed,
known parameter signature and consumes exactly its own arguments recursively.
A bare function name starts an invocation; variables always require `$name`.
Parentheses can delimit a complete invocation or group ordinary expressions.
There is no comma-separated `max(5, 10)` syntax or `call` prefix.

```text
max 5 min 10 3             # max receives 5 and the result of min 10 3.
max --a 5 --b min --a 10 --b 3
set limit max 5 min 10 3
max 5 min 10 3 + 2         # min receives 10 and (3 + 2).
max 5 ((min 10 3) + 2)     # max receives 5 and ((min 10 3) + 2).
```

Binary operators continue the current argument as described in section 2;
the parser never reinterprets an expression to satisfy an argument count.
Thus `max 5 -1` is missing an argument, while `max 5 (-1)` is valid.
Named arguments belong to the innermost unfinished invocation. Each invocation
must consume its full parameter set before parsing resumes in its caller.
Function signatures are collected before executable statements are parsed and
validated, including signatures of definitions later in the same submission.

Only pure built-in value functions may appear inside expressions. They do not
modify globals, events, hardware, or job state and cannot wait or spawn jobs.
As stages, they replace the primary value with their result, preserve incoming
fields and timestamp, and support a final `--out` binding. All listed parameters
are required explicitly; functions do not implicitly take an argument from the
incoming event. Use `$value` to select it. Nested calls cannot use `--out`.

```text
int "200.9" | print
adc A0 | clamp $value 0 700 --out limited | print
```

### Built-in value-function catalog

Lowercase words in the signatures below are parameter names, including their
named-argument spelling. Both runtimes implement these functions. Numeric parameters use strict numeric conversion. Text
and list parameters require their stated kinds; lists never convert implicitly
to text or numbers. All numeric results must be finite.

| Function signature | Contract |
| --- | --- |
| `number x` | Convert numeric text using the rules below; accept a number unchanged |
| `int x` | Convert to a number, then truncate toward zero |
| `string x` | Preserve a string or format a number using the round-trip formatting rule; reject lists |
| `abs x` | Absolute value |
| `min a b` | Smaller numeric value |
| `max a b` | Larger numeric value |
| `clamp x low high` | Restrict a number to the inclusive range; reject `low > high` |
| `round x` | Nearest integer; exact ties round away from zero |
| `floor x` | Round toward negative infinity |
| `ceil x` | Round toward positive infinity |
| `len x` | Unicode scalar count for strings, item count for lists |
| `contains x item` | Substring test for strings, membership test for lists; return `0` or `1` |
| `trim text` | Remove surrounding ASCII space, tab, CR, LF, vertical tab, and form feed |
| `lower text` | Convert ASCII A–Z to lowercase; preserve other characters |
| `upper text` | Convert ASCII a–z to uppercase; preserve other characters |
| `slice x start end` | Return a string or list range; zero-based start, exclusive end |
| `replace text old new` | Replace all non-overlapping occurrences of `old` with `new` |
| `at items index` | Return a list item using a zero-based integer index |
| `join items separator` | Join a list of strings into text; reject non-string items |
| `notehz text` | Convert a note string such as `C4`, `F#4`, or `Bb3` to Hz |

Indexes and range endpoints must be integers. Negative indexes, out-of-range
indexes, and invalid ranges are errors. A slice requires
`0 <= start <= end <= (len x)`; equal endpoints produce an empty result.
List operations return new values and never mutate their inputs. `split` remains
a pipeline command that produces lists; this catalog adds no list literals,
mutation, sorting, or general-purpose collection framework.

Strings use valid UTF-8; invalid encoding is an error. `len` and string `slice`
count Unicode scalar values, not bytes or grapheme clusters: an emoji may occupy
one scalar, while an accented letter plus a combining mark occupies two. Case
conversion and trimming are ASCII-only. Index arguments must be numeric integers;
numeric text does not substitute for an index.

List membership uses the scalar equality rules above: `2` matches `"2"`, but
`"02"` and `"2"` differ. Comparing a list as a member is an error. Empty string
searches match in `contains`, including in empty strings; an empty `old` argument
to `replace` is an error. Replacement scans non-overlapping matches from left to
right and never rescans inserted text. `round 2.5` is `3`, and `round (-2.5)` is
`-3`. String/list operations enforce section 13's limits before returning growth.

String-to-number conversion follows these rules:

- Numeric text converts to its numeric value.
- Empty, whitespace-only, and other nonnumeric text raise a conversion error.

Numeric text accepts surrounding whitespace, an optional sign, and the same
decimal or hexadecimal forms as numeric literals. The entire trimmed string must
match to be treated as numeric text; conversion does not accept just a numeric
prefix. Nonfinite numeric results remain errors.

```text
(int "200.9")   # 200
(int "hello")   # Error: invalid numeric text
(int "")        # Error: invalid numeric text
(int "   ")     # Error: invalid numeric text
(int "20O")     # Error: invalid numeric text, not 20 or 1
```

Implicit conversion uses the numeric value without truncation; `int` explicitly
truncates toward zero. Hardware commands validate their own ranges and integer
requirements rather than silently truncating arguments.

There are no list literals, indexing operators, implicit string concatenation,
or user-defined expression functions in this core. Interpolation composes text;
User-defined command functions are invoked directly by name.

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
| User-defined function invocation | Result of body | Result of body |

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
user-defined function invocations support it; actions and control commands do not. `each` uses `--out` to
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

Collision checks apply in both directions. `set` rejects a target name that is
an incoming event field or a parameter of the current invocation, before changing
the global table. Parameters are read-only bindings; `set` never assigns to them.
For example, `adc A0 --out raw | set raw 10` fails without creating a global.
Checks use the current execution context, not a registry of all live fields and
parameters. Another job can therefore create a global that collides with a
suspended invocation's parameter or event field. A later reference to that name
fails as ambiguous; it never silently changes lookup priority.

All jobs share global variables and run cooperatively. One expression evaluation and
assignment completes before another job advances; a sequence spanning a wait
is not atomic. Job ordering is not a synchronization mechanism.

## 6. Blocks and sequential execution

A block executes statements sequentially. A statement is one command or a
pipeline; newlines and semicolons separate statements. Finite operations complete
before the next statement begins, including pending delayed outputs and their
downstream continuations. Likewise, an `each` item finishes its finite body and
pending timers before the next item begins. This does not make a periodic source
wait for a pulse/debounce deadline before accepting another source event: direct
source pipelines can retrigger those stages while a timer is pending.

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
Successful registration emits no immediate event. If registration is the final
statement, the block has no immediate output, even though its work completes.

```text
do {
    every 100 | adc A0
} | print "Registered"  # Does not run: registration emits no immediate event.

do {
    every 100 | adc A0
    print "Registered"  # Runs as the next statement after registration.
}
```

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

pair "C4:200" --out pitch,duration |
    tone 15 --hz (notehz $pitch) --dur (int $duration)
```

Functions have declared parameters and a sequential body. Invoke a function
directly by its case-sensitive name, using positional or named arguments under
section 3's invocation rules. The `call` keyword is removed.

```text
pair --text "C4:200" --out pitch,duration
```

Arguments are evaluated once at call entry and must match the declared parameters.
User-defined functions are command functions and may be pipeline stages, but
cannot appear inside expressions: their bodies may wait or act on hardware.
Calls receive the incoming event and create local parameter bindings only.
They read and update the shared global variable table; no globals are copied.

The final statement's output is the call result. Earlier statement outputs are
not fed into later statements. There
is no `return` statement. An empty body forwards the incoming event. Filtering
inside a statement does not skip later statements. Parameter bindings disappear
when the invocation is no longer needed.
Global variable updates persist, and output fields continue to the caller.

Function definitions are allowed only as standalone top-level statements, never
inside a body or a pipeline. Duplicate function names within one submission are
errors. User-defined function names must not collide with built-in commands or
value functions, ignoring case. Definitions are committed together only after the entire submission
passes validation, before any executable statements run, and persist between
submissions. Failed validation leaves existing definitions unchanged. A later
runtime failure does not undo committed definitions.
Redefinition applies to calls that begin afterward. A suspended call
continues the definition it began with. Retaining active definitions must be
bounded by an implementation limit; replacement must fail clearly if it would
exceed that limit.

Sources started inside a function retain access to that invocation's parameter
bindings for as long as needed, without copying global variables. The call
finishes after its finite statements; it does not wait for independent child
sources to end.

### Stateful stage lifetime

Each stateful stage occurrence, such as `toggle`, `edge`, `divide`, `debounce`,
or `pulse`, has independent state. Direct stages in a job retain their state
between events until that job ends. Separate jobs never share stage state merely
because they execute the same source text.

Each function invocation creates independent state for its body stages. Repeated
calls initialize new state; nested or concurrent calls do not share it. A source
created inside a function owns its downstream stage state for the child job's
lifetime, even after the invocation that registered it has finished. Commands
must document their initial state and transitions separately.

```text
func flip { toggle }
every 100 | flip | print  # Each call starts a fresh toggle state.
every 100 | toggle | print     # This toggle retains state between timer events.
```

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

Each source processes one event at a time, including downstream waits. Periodic
deadlines follow a fixed timeline anchored at source registration, rather than
restarting the interval after downstream processing. When ready for another event,
a source discards overdue ticks and waits for the next future deadline; a tick
due exactly when the source becomes ready may run. There is no catch-up burst.
The first-service event of `every` is the explicit exception to overdue skipping;
later deadlines remain anchored at registration.
Timer intervals are best-effort
service intervals, not guarantees about physical edge timing or sample delivery.

`clock` and `adc --watch` use the same events, expressions, and downstream grammar
as `every`. Their waveform or sampling parameters remain command-specific.
Starting a source is permitted as a statement in any body or as a stage after a
pipe. When a background source receives an upstream event, it starts a new
independent job for that event. The new job generates its own events for the
remaining downstream stages; it does not block the upstream source until the
new job ends.
Registration emits no immediate event into the upstream job. Only the new job's
future events execute the captured downstream stages. Source arguments are
evaluated once at registration; downstream expressions read globals when reached.

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
            pair "C4:200" --out pitch,duration |
                tone 15 --hz (notehz $pitch) --dur (int $duration)
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
schedule does not wait for its entries to finish and emits no immediate event.
Each entry retains the current invocation's parameter bindings, if any, until
its work no longer needs them. It captures event data and parameters, not globals;
parameter bindings are not dynamically inherited from later callers.

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
above. Entries define the shared software contracts. Verification status and
physical-board work are tracked separately in the
[implementation checklist](language-implementation-checklist.md).

The catalog covers the built-in command inventory. Brackets denote
optional syntax, uppercase words denote arguments, and alternatives separated
by `|` inside a syntax description are choices, not literal pipeline operators.

### Command inventory

| Group | Commands |
| --- | --- |
| Hardware input | `read`, `adc`, `shiftin` |
| Hardware output | `write`, `shiftout`, `tone` |
| Display | `lcd init`, `lcd print`, `lcd clear`; `oled init`, `oled print`, `oled chart`, `oled rect`, `oled clear` |
| Transformation | `map`, `not`, `toggle`, `split`, `ohm` |
| Pure value functions | `number`, `int`, `string`, `abs`, `min`, `max`, `clamp`, `round`, `floor`, `ceil`, `len`, `contains`, `trim`, `lower`, `upper`, `slice`, `replace`, `at`, `join`, `notehz` (section 3) |
| Event filtering | `where`, `edge`, `rising`, `falling`, `debounce`, `divide` |
| Signal shaping | `pulse` |
| Timing | `clock`, `every`, `sleep`, `schedule` |
| Structure and state | `do`, `if`/`else`, `func`, `each`, `set` |
| Console and management | `print`, `help`, `pins`, `jobs`, `kill`, `status`, `reset` |
| File execution | `run` (currently simulator-only; device support remains future work) |

`else` is part of `if` syntax, not an independently invocable command. `var` is
excluded from the language, as is the old `call` keyword. Built-in value
functions are defined in section 3 and can also be used as pipeline stages.

### Alphabetical command index

[adc](#adc) · [clock](#clock) · [debounce](#debounce) · [divide](#divide) ·
[do](#do-each-func-if-and-set) · [each](#do-each-func-if-and-set) ·
[edge](#edge-rising-and-falling) · [falling](#edge-rising-and-falling) ·
[func](#do-each-func-if-and-set) · [help](#help-pins-jobs-and-status) ·
[if](#do-each-func-if-and-set) · [jobs](#help-pins-jobs-and-status) ·
[kill](#kill-and-reset) · [lcd](#lcd) · [map](#map) · [not](#not) ·
[ohm](#ohm) · [oled](#oled) · [print](#print) · [pulse](#pulse) ·
[read](#read) · [reset](#kill-and-reset) · [rising](#edge-rising-and-falling) ·
[run](#run) · [schedule](#sleep-tone-every-and-schedule) ·
[set](#do-each-func-if-and-set) · [shiftin](#shiftin-and-shiftout) ·
[shiftout](#shiftin-and-shiftout) · [sleep](#sleep-tone-every-and-schedule) ·
[split](#split) · [status](#help-pins-jobs-and-status) · [toggle](#toggle) ·
[tone](#sleep-tone-every-and-schedule) · [where](#where) · [write](#write).
Pure value functions are indexed in section 3's table.

### adc

```text
adc A0 [--out NAME]
adc A0 --watch [--out NAME]
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
other sources. A final `--out NAME` binds each emitted reading in its fresh event.

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
clock HZ [--dur MILLISECONDS] [--out NAME]
```

**Input and arguments.** `HZ` is a numeric expression in the range `0.01` through
`1000000`, inclusive, representing complete cycles per second. `--dur` is an
optional integer duration from `1` through `2147483647` milliseconds. Without
`--dur`, the source continues until stopped. Direct-pin shorthand is removed;
use `clock HZ | write PIN`. A final `--out NAME` binds each waveform value.

**Output.** The nominal waveform alternates primary values `1`, `0`, `1`, `0`,
with one event per half-cycle when downstream work keeps up. Skipped transitions
follow the phase rule below. Each emitted event has fresh fields and its actual emission
timestamp. At 10 Hz, the nominal interval between events is 50 ms. The source
does not forward an upstream primary value as its waveform.

**Execution.** Registration starts an independent background job and does not
wait for its duration to expire. Placing `clock` after a pipe starts a new job
for each upstream event. Downstream work runs sequentially per emitted event;
busy work skips overdue transitions rather than queuing an unlimited burst.
Waveform phase follows nominal transition deadlines: skipped transitions still
advance phase, so consecutive delivered events can have the same value. Frequency
does not become a count of delivered events. The initial nominal level is low;
the first event is high after one half-period. Round half-periods to the nearest
whole microsecond, with ties upward and a minimum of one microsecond.
When `--dur` expires, no new events start, including at the exact end deadline.
An event already in progress finishes its downstream work before the job releases
its resources; expiration does not cancel a downstream wait. Explicit `kill` and
`reset` still cancel the job immediately.
Accepted frequency ranges describe validation, not a physical timing guarantee.

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
(int (OUT_MIN + (VALUE - IN_MIN) * (OUT_MAX - OUT_MIN) / (IN_MAX - IN_MIN)))
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
section 3. Lists use bracketed, comma-and-space-separated values, with strings
quoted and escaped as JSON text: `["C4", "E4", 2]`. Numbers retain the binary64
formatting rule. Console/display actions use this same list formatting. Lists
cannot be interpolated into strings; use `join` for lists of strings. Empty text
and zero are valid.

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
`--out` is not supported. Timed-write shorthand is removed. Use a sequential
body such as `do { write 12 1; sleep 100; write 12 0 }` when an explicit
return to low is required.

```text
write 12 1
write 12 0
every 100 | adc A0 | write 12 $value > 700 | oled chart
```

In the final example, the comparison selects the GPIO level while `oled chart`
still receives the raw ADC reading.

### debounce

```text
debounce MILLISECONDS
```

Require a primary value that converts numerically to `0` or `1`, including numeric
text under section 3. Preserve its original kind when forwarding it. Retain the
first level or a changed
level as a candidate, then emit it after the specified positive integer duration
from `1` through `2147483647` milliseconds without another change. Repeated identical input does not restart
the deadline. A changed candidate replaces the pending candidate. Do not emit a
level equal to the last accepted output. The initial level must also settle before
its first emission. Preserve the candidate's fields and triggering timestamp.

### divide

```text
divide N [--out NAME]
```

Require input that converts numerically to `0` or `1` and a positive integer
divisor from `1` through `2147483647`. Emit initial output `0`
on the first input. Count input transitions, ignoring duplicate levels. Every
Nth transition toggles the output and emits the new level. Intermediate counts
emit nothing. State starts independently for each stage occurrence and invocation.
For inputs `0,0,1,1,0,0,1,0`, `divide 2` emits `0,1,0`. A final `--out`
binds each produced level.

### do, each, func, if, and set

```text
do { BODY }
each [--out NAME] { BODY }
func NAME [PARAMETER...] { BODY }
NAME ARGUMENTS [--out NAME[,NAME...]]
if EXPR { BODY } [else { BODY }]
set NAME EXPR
```

Braces are required even for an empty body. Sections 5–9 define statement
isolation, output binding, conditions, globals, function calls, and iteration.
`set` emits its assigned value; actions inside a body preserve its input event.
`each` executes its body for each list item, then forwards the original list once.
Function calls and selected branches forward their final statement's results.

Delayed outputs from a final statement, such as the low event of `pulse`, also
continue through the enclosing block/function's downstream pipeline. This applies
recursively to nested final statements. Outputs from earlier statements stay
inside those statements; delayed item-body outputs do not become `each` outputs.
The job retains pending continuations until completion or explicit cancellation.

### edge, rising, and falling

```text
edge
rising
falling
```

Require primary values that convert numerically to `0` or `1`. The first value establishes a baseline and emits
nothing. `edge` forwards both subsequent level changes, `rising` only `0` to `1`,
and `falling` only `1` to `0`. Duplicate levels emit nothing. Accepted events keep
their original primary kind/value, fields, and timestamp; these filters do not
generate a new timestamp. `edge`, `rising`, `falling`, and `debounce` do not
support `--out`; bind data upstream. Producers `divide` and `pulse` do.

### help, pins, jobs, and status

```text
help [COMMAND]
pins
jobs
status
```

Write diagnostic text to the command output and forward the incoming event
unchanged, including its missing-value state. `help` describes the common command
catalog; an optional command name is accepted but does not currently restrict its
output.
`pins` describes GPIO availability and reservations; `jobs` lists running/queued
work and retained failures; `status` reports runtime/device state. Text and device
fields may differ between simulator and hardware. These observations are allowed
inside ordinary command bodies and do not create replacement values.

### kill and reset

```text
kill ID
reset
```

`kill` requires an integer existing job ID and cancels that job, including its
pending callbacks/scheduled work. It forwards the caller's event unless the caller
kills itself, in which case no remaining statements execute. Unknown IDs error.
`reset` cancels all jobs, clears global variables and retained failures, and emits
no event. Function definitions remain registered. Both release GPIO ownership
without changing last levels or clearing display contents.

### lcd

```text
lcd init [ADDRESS]
lcd print [VALUE | --value VALUE]
lcd clear
```

Support a 16×2 HD44780 LCD with the documented PCF8574 backpack mapping. The default
seven-bit I2C address is `0x27`; valid addresses are `0x08`–`0x77`. `print` selects
explicit data or the primary value and uses the console's scalar/list formatting.
It replaces the screen from its top-left corner, wraps at 16 characters, clips
at 32, and pads unused cells with spaces. `\n` starts the second row. `clear`
blanks the screen. Commands preserve the incoming event. Missing print input,
invalid options/addresses, pin conflicts, and failed transfers error.

Initialization reserves GPIO4 (SDA) and GPIO5 (SCL) for the display bus; reject
conflicting active owners. First use initializes automatically. Character ROM
controls available glyphs; UTF-8 language support does not guarantee matching LCD
glyphs. See the tutorial for electrical requirements.

### not

```text
not [VALUE | --value VALUE] [--out NAME]
```

Select an explicit value or require the incoming primary value. Apply strict
numeric conversion, replace zero with `1`, and any nonzero value with `0`.
Preserve fields/timestamp; a final `--out` also binds the result.

### ohm

```text
ohm [VALUE | --value VALUE] [--out NAME]
```

Select an explicit integer ADC count or the primary value. Convert it through the
existing measured resistor-divider calibration, replacing the primary value with
whole ohms or a range string. Counts 30–720 produce numeric results; counts below
30 return `"<270"`, above 720 return `">700k"`, and outside 0–1023 return
`"OUT_OF_RANGE"`. Fractional/nonnumeric input is an error. Preserve fields and
timestamp and optionally bind the result. This is a transformation, with no
implicit hardware read or print: use `adc A0 | ohm | print`. Circuit calibration
and the uncalibrated 30–49 extrapolation are described in the tutorial.

### oled

```text
oled init [ssd1306|sh1106] [--address ADDRESS] [--width 128] [--height 32|64]
oled print [X Y] [VALUE | --value VALUE] [--x X --y Y]
oled chart [X Y] [VALUE | --value VALUE] [--x X --y Y]
    [--width WIDTH --height HEIGHT --min MIN --max MAX]
oled clear
```

The two-line `chart` signature above is notation for one command. Supply all
options on its command line. Default controller is SSD1306, address `0x3c`, and
screen size 128×64. SH1106 uses its controller-specific column mapping. Select the
actual controller explicitly; an I2C acknowledgment does not identify it.

Printing uses explicit data or the primary value, formatted like console output.
Coordinates default to `(0,0)`; positional coordinates and named coordinates
cannot be combined. Printing changes its character cells rather than clearing the
whole screen. The bundled bitmap font covers ASCII; unsupported glyphs follow
the driver's fallback. `chart` requires a finite numeric reading, appends one
sample in its rectangle, and defaults to the screen rectangle and range 0–1023.
Screen geometry and range bounds are validated. `clear` removes text and chart
history; initialization also clears previous contents. Commands preserve events.

GPIO4/5 are reserved for I2C. Rendering is serviced cooperatively in short device
transfers; invalid options, duplicate options, conflicting owners, invalid
geometry/ranges, and transfer failures error. Software reset/kill preserves the
last image. Simulation models pixels/history, not physical transfer timing.

```text
oled rect X Y WIDTH HEIGHT [--value 0|1]
```

`rect` fills a pixel rectangle, defaulting to on (`1`). Use `--value 0` to erase
only that area. All four geometry arguments are required integer expressions;
width/height must be positive and the entire rectangle must fit the configured
display. Geometry is validated before initialization or drawing. A 1×1 rectangle
draws one pixel. The command preserves the incoming event and ignores its primary
value; only the explicit `--value` selects the pixel state. It has no `--out`.
Rectangles share the framebuffer with text/charts and do not reset chart state.
Transfers remain cooperative; multiple drawing commands do not guarantee an
atomic display frame. See [Pong](examples/pong.cw) for erasing and moving sprites.

### pulse

```text
pulse MILLISECONDS [--out NAME]
```

Any incoming event triggers a pulse, including one without a primary value.
When inactive, emit `1` immediately and retain a pending `0` for the specified
positive integer duration from `1` through `2147483647` milliseconds. While
active, each input extends the low deadline
to that input's time plus the duration without another `1`. The low event uses
the most recent triggering event's fields and timestamp. After emitting low, the
stage is inactive again. A final `--out` binds both high and low levels. There
is no implicit pin write; pipe to `write PIN`.
Zero-duration pulses are errors. Delayed downstream work belongs to the same owner job and is canceled with it. Finish an active
downstream event, including its waits, before servicing a pending delayed
continuation; timer expiry does not preempt that work.

### run

```text
run "PATH"
```

Simulator-only file execution. Read UTF-8 source and use the normal statement,
expression, and event rules. File contents do not select a legacy parser or feed
one statement's output into the next. Missing/unreadable files and invalid source
error. Firmware rejects `run` explicitly. Use `script load` to retrieve saved
source into the web editor, then Run to execute it.

### script

```text
script list [--out NAME]
script save NAME
script load NAME [--out NAME]
script delete NAME
```

Firmware stores named UTF-8 source files in LittleFS under `/scripts`. Names are
case-sensitive strings of 1–28 ASCII letters, digits, spaces, `_`, or `-`, without
surrounding spaces. Quote names containing spaces or consisting only of digits.
Paths, dots, and other characters are rejected; the internal `.cw` suffix is not
part of the name. Command/action names ignore case.

`list` produces a sorted list of names, including an empty list when no scripts
are saved. `load` produces the exact source string, including whitespace and
trailing newlines. Both preserve event fields/timestamp and support `--out`.
Nothing prints implicitly: append `| print` for console output. Loading does not
execute source, change runtime definitions, or register a boot script.

`save` requires an incoming string value. It saves bytes verbatim without parsing
or executing them, overwrites an existing name, and preserves the incoming event.
An explicit empty string creates an empty script; missing/non-string input errors.
`delete` removes a named script and preserves the incoming event. These actions
do not support `--out`. Missing scripts and filesystem failures error normally.

There are at most 32 scripts, each at most 4,096 UTF-8 bytes. Available flash and
LittleFS metadata may reduce that capacity. Replacement needs free space for the
new temporary file while the old file remains intact. Saves check writes and
read back the temporary file before atomic rename; failures preserve the old
script. An interrupted save may leave `/scripts/.pending`, which is ignored by
listing and removed before the next save. Saved files survive runtime `reset`,
reboot, and firmware-only updates that preserve the filesystem layout. Flash
erasure, filesystem image uploads, and partition changes can destroy them.

Mounting explicitly disables automatic formatting on ESP8266 and ESP32. A mount
failure reports unavailable storage and leaves other commands usable. First use
requires deliberate external provisioning of a LittleFS filesystem. The separate
`provision-littlefs.sh` ESP8266 utility requires an upload confirmation and then
`FORMAT LITTLEFS` over serial before erasing any files; see the
[setup instructions](../README.md). Startup output and `status` distinguish mount,
directory creation, and directory access errors and report the flash layout. The full
firmware still targets ESP8266; only storage is portable to ESP32. The Python
simulator models scripts in memory for the lifetime of its `ScriptRuntime`,
including across `reset`; it does not write persistent host files.

The web editor submits the same commands to `POST /api/command` as URL-encoded
fields. Set `result=script` for a structured response; this mode accepts only
one standalone `script` command. For `save`, include a separate `source` field,
even for empty content. This seeds the incoming event value without escaping
source into executable command text or consuming its 4,096-byte command budget.
The source field is rejected for other actions.

Responses are JSON: `{ "ok": true, "value": ... }`, with a name array for `list`,
source text for `load`, and `null` for `save`/`delete`. Failures return HTTP 400 and
`{ "ok": false, "error": "..." }`. Responses disable caching. Ordinary command
requests retain their console-text responses. The editor fetches lists and
contents from the device, confirms replacement/discard as before, and marks a
buffer saved only after success. Failed requests preserve its text and dirty state.

### shiftin and shiftout

```text
shiftin DATA CLOCK LOAD [msb|lsb] [--bits BITS] [--out NAME]
shiftout DATA CLOCK [LATCH] [msb|lsb] [--value VALUE] [--bits BITS]
```

Pins use `read`/`write`'s integer GPIO and ownership rules. Pins in one operation
must be distinct. Default bit order is `msb`; default frame width is 8, with
explicit `--bits` from 1 through 53. Width is always named, avoiding ambiguity
with a value or optional latch. `shiftout` without a latch uses two hardware pins. Explicit data uses `--value`;
otherwise use the incoming primary value. There is no positional payload, which
would be ambiguous with the optional latch pin.

`shiftin` pulses LOAD low/high, samples the first data bit before the first clock
pulse, clocks the frame, and replaces the primary value with its unsigned integer.
`shiftout` selects explicit data or the incoming primary, requires an integer in
`0..2^BITS-1`, clocks one frame, and optionally pulses the latch. It preserves the
incoming event even when explicit data differs. Both preserve fields/timestamp;
only `shiftin` supports `--out`. Pins remain owned until the job ends and retain
last levels afterward. Unsupported widths/order/options, duplicate pins, missing
input, out-of-range integers, and ownership conflicts error. Simulation models
bit order and GPIO histories; verify edge timing and register compatibility on
the actual circuit.

### sleep, tone, every, and schedule

```text
sleep MILLISECONDS
tone PIN --hz HZ --dur MILLISECONDS
every INTERVAL [--out NAME]
schedule { OFFSET_EXPR { BODY } ... }
```

Section 10 defines cooperative waits, periodic deadlines, and schedule capture.
`sleep` and `tone` preserve their event. Tone frequency is `0.01..1000000` Hz;
round half-periods to whole microseconds with ties upward/minimum one. A positive
duration claims the GPIO and starts low, switching after one half-period. A
zero-duration tone validates its pin/reservations/frequency but does not drive
or wait. Completion releases ownership when its
job finishes and never adds a cleanup write. Explicitly write a level afterward
if the circuit requires it. Software GPIO and loop scheduling provide best-effort
timing, not a guarantee of frequency fidelity.

`every` requires an integer interval from `1` through `2147483647` milliseconds.
It emits `0` at first service and then on its fixed registration timeline.
A final `--out` binds each emitted value, also for `clock` and `adc --watch`.
Source events have fresh fields and an actual service timestamp; registration
emits nothing. Schedules evaluate each offset once, capture event/parameters,
and read live globals when entries execute. Body braces are mandatory. Delayed
signal continuations preserve their triggering timestamps, while schedule entry
start replaces its captured timestamp with the actual start time. No source or
transformation prints implicitly; add `| print` when console data is wanted.

### split

```text
split DELIMITER [VALUE | --value VALUE] [--out NAME[,NAME...]]
```

Require a nonempty string delimiter and explicit or incoming string input.
Replace the primary value with a list of substrings, preserving empty substrings,
fields, and timestamp. Matching is literal and non-overlapping. `--out` may bind
the entire list or unpack it into exactly as many names as items. Enforce the
64-item limit; invalid kinds/empty delimiter/missing input/count mismatch error.

### toggle

```text
toggle [--out NAME]
```

Start at state `0`, flip on each incoming event, and emit `1`, then `0`, and so
on. An event does not need a primary value. Preserve fields/timestamp. State is
per owning stage occurrence, with fresh state on each function invocation.

### where

```text
where EXPR
```

Evaluate a condition once using section 3's strict numeric truth rules. Forward
the incoming event unchanged when nonzero; otherwise emit nothing. This suppresses
only the remainder of the current statement, not subsequent sibling statements.

## 13. Bounds and simulator agreement

The initial shared software limits are:

| Resource | Bound |
| --- | --- |
| Submission | 4,096 UTF-8 source bytes |
| Retained command AST nodes across definitions/tasks/timers | 128 total; shared nodes count once |
| Nested blocks / function calls / expression nesting | 8 / 8 / 16 |
| Function definitions / global variables | 16 / 32 |
| Named fields per event / list elements | 16 / 64 |
| Individual string | 4,096 UTF-8 bytes |
| Pending continuation frames across jobs/timers | 128 total |
| Active script/source jobs | 8 shared slots |
| Pending schedule entries | 32, separate until execution begins |
| Retained value payload | 8,192 bytes |
| Retained program source | 16,384 UTF-8 bytes |
| Retained failures | Last 8 |

These are initial implementation defaults, pending peak-heap measurements on an
ESP8266. Retained program accounting charges each retained submission source once,
including old definitions and active task/timer continuations. A number costs
8 payload bytes; a string costs its UTF-8 byte length; a list costs its item
payloads. Stored names contribute their UTF-8 bytes. Globals, captured parameters,
events, fields, and pending timer values remain charged for their retained lifetime.
Shared ownership of the same event/scope is counted once; copied events carry
separate payload charges. Object counts bound interpreter overhead separately;
these logical budgets do not measure allocator overhead or establish free heap.

Exceeding a bound reports an error. Do not truncate strings/lists or silently drop
work, apart from the specified skipped periodic ticks. Reject growing assignments
and definitions before replacing stored data. A rejected registration must not
leave a partially registered schedule. Queued entries need an active slot when
they actually start; data-dependent resource conflicts fail the affected job.

Shift frames are 1–53 bits, defaulting to 8. Valid frame values are integers from
0 through `2^bits - 1`, so every supported value is exactly representable in
binary64. Arbitrary 64-bit integer preservation is not provided.

The simulator must agree with firmware on value kinds, event propagation, scope,
branch selection, ordering, and error behavior. Virtual time may bound unlimited
sources with a sample count. It does not establish physical timing fidelity.
Differences in resource planning or scheduling must be listed explicitly, not
treated as alternative language semantics.
