# Commandware implementation and verification checklist

This records the completed language migration against the
[authoritative reference](language-reference-draft.md). The initial contracts and
software limits are implemented in the simulator and portable firmware runtime.
Checked items have source/conformance coverage; they do not certify a physical
board, waveform, display, or peak memory use.

## Parsing and values

- [x] Route console submissions, function bodies, sources, and schedules through
  the common expression/event grammar; remove active legacy routing.
- [x] Parse complete expressions with signed-argument grouping, comparisons,
  short-circuit logic, strict conversion, and binary64 round-trip formatting.
- [x] Consume fixed-arity nested calls, including `max 5 min 10 3`; support all
  positional or all named arguments and reject invalid/mixed argument sets.
- [x] Invoke user functions by their case-sensitive names; remove `call`/`var`.
  Reject built-in name collisions ignoring case.
- [x] Implement all documented number, string, and list functions both as pure
  expressions and stages, with explicit parameters and stage-only `--out`.
- [x] Specify/implement Unicode scalar indexing, invalid UTF-8 errors, ASCII
  case/trim, ties-away rounding, scalar list equality, empty substring matching,
  nonempty replacement search, strict indexes, and bounded result growth.
- [x] Preserve literal quoting/escaped dollars and interpolate only once.

## Events, variables, bodies, and functions

- [x] Share globals and keep read-only local parameters; `set` creates/updates
  globals and emits its value. Detect current field/parameter conflicts in both
  directions and ambiguity introduced by other jobs.
- [x] Preserve actions' input events while transforms replace primary values.
  Explicit action data takes precedence without changing the event.
- [x] Feed every body statement its original input independently; propagate only
  final-statement outputs. Empty bodies/false `if` without `else` forward input.
- [x] Require braces for bodies; implement `if`/`else`, `where`, and block-only
  sequential `each`, which forwards the original list once after iteration.
- [x] Rebind existing event fields and support exact list unpacking with `--out`.
- [x] Restrict definitions to standalone top-level statements, validate entire
  submissions before committing definitions, preserve active old bodies, and
  apply redefinitions to calls beginning afterward.
- [x] Initialize function-body state per invocation; retain source-owned state.
- [x] Propagate delayed final-statement output through nested body/function
  continuations; retain/cancel timers with their owner. Keep nonfinal/item-body
  output isolated and serialize delayed continuations with downstream waits.
  Finish finite statements/items, including their pending timers, before advancing
  to the next sibling statement/item; periodic pipelines can still retrigger.

## Jobs and command migration

- [x] Register sources/schedules without an immediate event. Sources generate
  fresh fields/timestamps and support per-emission `--out`.
- [x] Use fixed periodic timelines, skip overdue work without catch-up, advance
  clock phase across skipped transitions, and drain active work at duration end.
- [x] First clock high follows one rounded half-period; remove direct-pin and
  timed-write shorthand. Preserve explicit `write` for required final levels.
- [x] Capture scheduled/source parameters and scheduled events, read globals live,
  allow ordinary body grammar, and validate aggregate registration capacity.
- [x] Implement Boolean edge filters, transition-count divide, millisecond
  debounce, and retriggerable pulse; retain independent stage state.
- [x] Align read/ADC/write/print/map/not/ohm contracts across runtimes. No implicit
  ADC measurement in `ohm`, no implicit printing, and no cleanup pin writes.
- [x] Support LCD/OLED actions with event preservation and explicit-value
  precedence; preserve contents across cancellation/reset.
- [x] Support shift frames 1–53 bits (default 8), named widths, bit order,
  optional output latch, input first-bit sampling, and integer range checks.
- [x] Expose management commands inside bodies; preserve events for observations,
  stop immediately on self-kill/reset, and retain the last eight failures.
- [x] Keep `run` explicitly simulator-only and reject it in firmware.
- [x] Enforce source, object-count, string, retained-program, and retained-value
  limits, including pending callbacks, captured data, and active definitions.
  Reject growth before replacing globals/definitions; preserve prior values.
- [x] Update reference, current guide, tutorial, examples, help, and web samples
  for the direct-call syntax and shared execution model.

## Software acceptance coverage

- [x] Shared Python/C++ value conformance: nested/named calls, all value functions,
  type/index/UTF-8 errors, rounding, short-circuiting, and bounded growth.
- [x] Shared runtime conformance: body input isolation, output/filter rules,
  captures, state lifetime, definition rollback/redefinition, cancellation,
  timers, skipped deadlines, source phases, and static dead-branch validation.
- [x] Regressions for timed downstream continuations, inverted pulse ordering,
  and simulator `run` termination with pending timers.
- [x] Portable/native firmware checks for actual sketch routing, serial handling,
  schedules, GPIO operations, resources, and command behavior.
- [x] Web request regressions and executable current-guide/tutorial examples.

## Remaining hardware verification

The final software acceptance run passed 372 Python/shared conformance tests,
both native firmware executables, and both web test files. The generic ESP8266
target compiled successfully: static RAM 37,508/80,192 bytes, instruction RAM
61,427/65,536 bytes (including 32 KiB flash cache), and flash code
387,284/1,048,576 bytes. Static usage is not a peak-heap measurement.

- [x] Compile the ESP8266 generic target using Arduino CLI 1.5.1 and ESP8266
  core 3.1.2. This verifies target compilation, not physical operation or peak heap.
- [ ] Flash the final firmware and verify the actual board configuration/circuit.
- [ ] Measure free/peak heap with retained functions, strings, lists, schedule
  captures, timers, and eight active jobs. Adjust initial logical budgets if
  actual allocator/object overhead requires tighter limits.
- [ ] Observe clock/tone/pulse/debounce timing and skipped transitions under Wi-Fi
  and I2C load; check cancellation and final pin levels on actual hardware.
- [ ] Verify shift-register sampling/clock/load/latch waveforms for real devices.
- [ ] Verify LCD/OLED addresses, controller variants, bus reservations, transfer
  failures/recovery, and retained display contents on real modules.
- [ ] Recheck the resistance calibration using the original measured circuit;
  software interpolation is not evidence of physical measurement accuracy.

Initial budgets and virtual-time/native checks establish deterministic software
contracts. Physical timing, device compatibility, and memory headroom remain
explicit verification work, not alternative language semantics.
