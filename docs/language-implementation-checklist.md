# Commandware reference implementation checklist

This checklist records differences between the intended
[language reference](language-reference-draft.md) and the current code. It is a
planning document, not an authorization to change the firmware or simulator.
The inventory used source inspection and small simulator probes; it did not
verify physical hardware behavior. Unchecked items are not yet resolved.

## Shared language behavior

- [ ] Use one parser and command contract across direct submissions, scripts,
  background jobs, and scheduled bodies.
- [ ] Parse value arguments as complete expressions rather than whitespace tokens.
  Apply the agreed signed-argument boundary rule consistently.
- [ ] Add comparisons, logical operators, short-circuit evaluation, `if`/`else`,
  and `where` with the reference's value/coercion rules.
- [ ] Remove `var`; make `set` create or update a global and emit its assigned value.
- [ ] Eliminate block variable copies. Keep one global table and per-call parameters.
- [ ] Make `do` accept pipeline input. Give every body statement the original block
  event independently; use only the final statement's output as the block result.
- [ ] Apply the same body rules to functions and selected branches; retain global
  side effects without allowing field/value changes to leak into later statements.
- [ ] Require a block for `each`; continue downstream once with the original list.
- [ ] Allow replacing existing event fields with `--out`; reject collisions with
  globals/current parameters and count only new fields against capacity.
- [ ] Allow downstream sources, with warnings once per source location per submission
  for obvious repeated job creation; preserve resource limits and conflict checks.
- [ ] Give scheduled bodies the normal grammar and execution-time global lookup.
- [ ] Stop forcing outputs low on completion, `kill`, failure, or software `reset`.
  Release ownership without changing pin configuration or last driven state.
- [ ] Align binary64 conversion, literal handling, and round-trip numeric formatting.
  Existing script formatting uses limited significant digits, and `number()` is absent.
- [ ] Expose the same command catalog in every context, including stateful signal
  stages, shift-register commands, and management commands. Define each management
  command's event output before enabling it inside a script.
- [ ] Update help, tutorial, examples, and current implementation documentation
  as behavior is implemented; keep the reference's intended semantics separate.

## First six command entries

| Command | Implementation differences to resolve |
| --- | --- |
| `read` | Unify numeric pin validation and reservations. Python's older pin parser accepts additional names/analog identifiers that are not a portable digital-pin contract. Support new event behavior in all contexts. |
| `adc` | Older simulator handling can reuse incoming values as ADC readings; scripting reads hardware afresh. Make all one-shot forms read hardware. Unify watch source behavior and downstream grammar. |
| `write` | Script handlers replace the primary value with a Boolean; preserve it instead. Older firmware pipelines can interpret incoming values as durations when an explicit output value is supplied. Remove that implicit duration behavior under the new contract. |
| `print` | Direct firmware printing parses an integer, and the older simulator handler ignores explicit arguments. Script printing accepts text. Unify explicit/piped values and event preservation, including `--value`. |
| `map` | Older Python scaling rounds and returns the output minimum for a zero-width input range; scripting truncates and rejects that range. Firmware scripting casts operands to integers before scaling. Use the reference's intermediate precision, final truncation, and error rules everywhere. |
| `clock` | Current source paths select the older pipeline grammar and prohibit blocks/named outputs. Unify grammar, expression evaluation, state cleanup, and per-event source spawning. Exact initial phase differs between paths and needs an explicit contract. |

## Other command discrepancies found in the inventory

- [ ] `not`: older Python handler calculates `1 - value`, while firmware and
  script handlers test for zero. Probe: older Python `not` with input `2` yields
  `-1`; logical negation should yield `0` under the existing logical contract.
- [ ] `pulse`: firmware tracks a remaining count and emits one value per input
  sample; the older Python handler expands rising edges into repeated `1` values
  and omits other events. Define whether the intended duration is milliseconds
  or event count before drafting its entry.
- [ ] `debounce`: current filtering counts consecutive equal samples; PRD examples
  call the parameter milliseconds. Choose and document the unit explicitly.
- [ ] `lcd`/`oled`: older Python handlers return no pipeline values, whereas script
  paths preserve events. Unify explicit-value precedence and event preservation.
- [ ] `ohm`: older firmware pipeline handling prints a terminal measurement and
  consumes the event; script handling produces a numeric/string measurement.
  Decide its command output consistently before drafting its entry.
- [ ] `shiftin`/`shiftout`: firmware performs bit clocking and load/latch operations;
  simulator handlers model limited GPIO/value behavior and accept different argument
  forms. Specify parameters and simulator guarantees; both are absent from script
  validators and older firmware downstream-stage parsing.
- [ ] `run`: implemented in Python, absent from firmware. The Python file runner
  also has a path that carries accumulated outputs between separate lines; align
  file execution with the agreed statement rules when specifying device support.
- [ ] `help`/`status`/`pins`/`jobs`/`kill`/`reset`: document text versus event outputs.
  Current scripts reject these names, while direct paths expose them.

## Catalog questions left explicit in this installment

- [ ] Decide whether to retain `clock HZ PIN` shorthand, including its relationship
  to explicit downstream sinks. The first entry uses `clock HZ | write PIN`.
- [ ] Define the first clock event's phase/time and handling of rounding at short
  periods; preserve the best-effort timing boundary.
- [ ] Decide whether to retain explicit timed `write` forms. No incoming value
  should silently become a duration merely because an explicit level is supplied.
- [ ] Specify list-to-text formatting for console/display actions.
- [ ] Specify named output bindings on source forms such as `adc --watch` and `clock`.
- [ ] Draft remaining command entries and expand the alphabetical index.

## Verification when implementation is authorized

Use focused conformance examples on both runtimes for the same argument parsing,
event values/fields, ordering, filtering, state lifetime, and errors. Native
firmware tests should verify resource scheduling and cleanup rules. Physical
waveform timing and I2C/device behavior require separate hardware checks; virtual
time simulation does not establish them.
