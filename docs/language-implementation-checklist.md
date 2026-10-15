# Commandware reference implementation checklist

This checklist records differences between the intended
[language reference](language-reference-draft.md) and the current code. It is a
planning document, not an authorization to change the firmware or simulator.
The inventory used source inspection and small simulator probes; it did not
verify physical hardware behavior. Unchecked items are not yet resolved.
The final review's shared-language recommendations have been accepted and are
specified in the reference. Their implementation remains unchecked below;
remaining catalog decisions and byte budgets are still explicit prerequisites.

## Shared language behavior

- [ ] Use one parser and command contract across direct submissions, scripts,
  background jobs, and scheduled bodies.
- [ ] Parse value arguments as complete expressions rather than whitespace tokens.
  Apply the agreed signed-argument boundary rule consistently.
- [ ] Use space-separated function arguments everywhere. Consume fixed-arity
  nested value calls recursively, including `max 5 min 10 3`, with optional
  parentheses to override grouping. Binary operators continue the current
  argument; named arguments belong to the innermost unfinished invocation.
  Collect function signatures before executable statement validation. Remove the
  old comma-separated expression-call syntax and migrate existing examples.
- [ ] Support all-positional or all-named function arguments, with named arguments
  in any order. Reject mixed forms and unknown, duplicate, missing, or extra
  arguments. Distinguish the final stage-level `--out` from parameter options.
- [ ] Remove the `call` keyword and resolve user-defined function names directly
  as pipeline stages. Preserve case-sensitive user names and reject collisions
  with built-in commands/value functions ignoring case.
- [ ] Expose pure value functions both inside expressions and as stages. Stage
  calls replace the primary value, preserve fields/timestamp, and support `--out`.
  Require explicit parameters, including `$value` for piped data. Reject `--out`
  in nested calls and user-defined command functions inside expressions.
- [ ] Add comparisons, logical operators, short-circuit evaluation, `if`/`else`,
  and `where` with strict numeric conversion. Empty, whitespace-only, and other
  nonnumeric strings must fail numeric conversion, including in conditions.
- [ ] Remove `var`; make `set` create or update a global and emit its assigned value.
- [ ] Eliminate block variable copies. Keep one global table and per-call parameters.
- [ ] Make `do` accept pipeline input. Give every body statement the original block
  event independently; use only the final statement's output as the block result.
- [ ] Apply the same body rules to functions and selected branches; retain global
  side effects without allowing field/value changes to leak into later statements.
- [ ] Require a block for `each`; continue downstream once with the original list.
- [ ] Allow replacing existing event fields with `--out`; reject collisions with
  globals/current parameters and count only new fields against capacity.
- [ ] Apply collision rules in both directions: reject `set` targets matching
  current fields or parameters; keep parameters read-only. Detect ambiguity on
  reference lookup after another job introduces a conflicting global, without
  maintaining a registry of every live field or parameter.
- [ ] Allow downstream sources, with warnings once per source location per submission
  for obvious repeated job creation; preserve resource limits and conflict checks.
- [ ] Make source and schedule registration complete without emitting an immediate
  event. A block ending in registration emits nothing; later statements still run.
- [ ] Evaluate source arguments once at registration and downstream expressions
  when reached. Use fixed periodic timelines, discard overdue ticks, and avoid
  catch-up bursts or interval drift caused by downstream waits.
- [ ] Advance clock phase across skipped transitions. At duration expiry stop
  starting events, then finish the in-progress event before releasing resources;
  `kill` and `reset` still cancel immediately.
- [ ] Give each stage occurrence independent job-owned state. Function body stages
  initialize per invocation; child sources retain their own downstream state after
  the registering invocation finishes.
- [ ] Give scheduled bodies the normal grammar and execution-time global lookup.
- [ ] Retain scheduled-entry parameter bindings and registration event data for
  their required lifetime, without capturing a copy of globals.
- [ ] Restrict function definitions to standalone top-level statements, reject
  duplicate names within a submission, and commit definitions together only after
  complete validation. Preserve active definitions during redefinition within
  bounded retained program storage.
- [ ] Stop forcing outputs low on completion, `kill`, failure, or software `reset`.
  Release ownership without changing pin configuration or last driven state.
- [ ] Align binary64 conversion, literal handling, and round-trip numeric formatting.
  Existing script formatting uses limited significant digits, and `number` is absent.
- [ ] Select and enforce byte limits for individual strings, retained programs,
  and retained values, including old definitions, captured events, and parameters.
  Repeated submissions must not bypass budgets. Reject growth before committing
  an assignment or definition; do not truncate or partially replace stored data.
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
| `clock` | Current source paths select the older pipeline grammar and prohibit blocks/named outputs. Unify grammar, expression evaluation, state cleanup, and per-event source spawning. Apply fixed timelines, phase advancement across skipped transitions, and draining of an in-progress event at duration expiry. Exact initial phase and timer rounding still need explicit contracts. |

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
  validators and older firmware downstream-stage parsing. Reconcile supported
  frame widths and numeric ranges with binary64's exact-integer range; do not
  imply arbitrary 64-bit integer preservation.
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

## Value-function catalog

- [ ] Implement conversions: `number x`, `int x`, and `string x`; reject implicit
  list conversion and preserve strict numeric conversion and number formatting.
- [ ] Implement numeric functions: `abs x`, `min a b`, `max a b`,
  `clamp x low high`, `round x`, `floor x`, and `ceil x`.
- [ ] Implement string/list functions: `len x`, `contains x item`, and
  `slice x start end`; validate kinds and integer bounds without mutation.
- [ ] Implement string functions: `trim text`, `lower text`, `upper text`, and
  `replace text old new`.
- [ ] Implement list functions: `at items index` and `join items separator`.
  Keep `split` as a pipeline command; do not add list literals or mutation.
- [ ] Migrate `notehz` to the common invocation contract: `notehz text` as a
  stage and `(notehz $pitch)` as an expression.
- [ ] Before implementing the affected functions, define halfway rounding,
  text encoding and length/slice units, case and whitespace rules, list-membership
  equality, and empty search-text behavior for `contains` and `replace`.

## Implementation sequence when authorized

1. Resolve remaining catalog decisions and choose memory byte budgets before
   implementing the affected behavior. Establish shared conformance cases for
   the accepted contracts.
2. Unify parsing and values: expression arguments, strict conversion, operators,
   literal handling, interpolation, numeric formatting, and the common positional/
   named function-call syntax with arity-based nested value calls.
3. Implement event, block, variable, and function execution: result propagation,
   isolated statement inputs, global state, parameters, collisions, definition
   registration, and per-invocation stage state.
4. Unify jobs and scheduling: source registration, retained event/parameter data,
   fixed timelines, clock phase, duration expiry, limits, failures, and pin cleanup.
5. Migrate commands to the common contracts, remove obsolete execution paths,
   and update help, tutorials, and examples as each command becomes conformant.

These phases describe dependency order, not permission to implement. Keep each
change reviewable and verify both runtimes against the same acceptance cases.

## Verification when implementation is authorized

Use focused conformance examples on both runtimes for the same argument parsing,
event values/fields, ordering, filtering, state lifetime, and errors. Native
firmware tests should verify resource scheduling and cleanup rules. Physical
waveform timing and I2C/device behavior require separate hardware checks; virtual
time simulation does not establish them.

Include cases for registration as a final block statement versus a preceding
statement; direct stateful stages versus repeated function calls and independent
jobs; skipped periodic deadlines and clock phase; duration expiry during a wait;
strict conversion of `""`, `"   "`, `"hello"`, and `"20O"`; collisions introduced
by assignments and other jobs; failed-validation definition rollback; scheduled
parameter retention; and memory exhaustion across repeated submissions. Verify
that rejected growth preserves prior values and active definitions.

Verify equivalent positional/named calls, nested calls mixed with arithmetic,
rejection of old invocation syntax, direct user-function invocation, name
collisions, stage result propagation versus pure expression evaluation, and
explicit use of `$value`. Add shared cases for each value function, including
type errors, empty values, invalid indexes/ranges, and memory-bound growth.
