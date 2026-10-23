"""Shared globals, local parameters, block outputs and cooperative execution."""
from pathlib import Path
import pytest
from commandware.core import HardwareState, run_stream
from commandware.script import ScriptRuntime


@pytest.mark.parametrize('source,expected', [
    ('set count 0; set count $count + 1 | print; print $count', [1, 1]),
    ('number 9 | do { number 1; print } | print', [9, 9]),
    ('number 9 | do { number 1; number 2 } | print', [2]),
    ('number 9 | do {} | print', [9]),
    ('number 9 | if 0 { print "no" } | print', [9]),
    ('number 9 | if 1 {} | print', [9]),
    ('number 9 | if 0 {}\nelse { number 3 } | print', [3]),
    ('number 9 | where 0 | print; print "next"', ['next']),
    ('number 9 | do { where 0; print "next" }', ['next']),
    ('number 9 | do { print "first"; where 0 } | print', ['first']),
    ('func Pair text { split ":" $text }; Pair "C4:200" --out pitch,dur | print "$pitch:$dur"', ['C4:200']),
    ('Pair --text "A:B" | join $value "/" | print; func Pair text { split ":" $text }', ['A/B']),
    ('func F x { set total $x }; F 3 | print; print $total', [3, 3]),
    ('func F { toggle }; F | print; F | print', [1, 1]),
    ('split ":" "A:B" | each --out item { print $item } | join $value "/" | print', ['A', 'B', 'A/B']),
    ('number 1 --out raw | number 2 --out raw | print $raw', [2]),
    ('print "Literal \\$raw"', ['Literal $raw']),
    ('set text "\\$missing"; print "$text"', ['$missing']),
    ('print "# ; | { }" # comment\nprint 2', ['# ; | { }', 2]),
    ('max 5 |\n print', []),
])
def test_execution_contracts(source, expected):
    if source.startswith('max 5 |'):
        with pytest.raises(ValueError):
            run_stream(source)
    else:
        assert run_stream(source) == expected


@pytest.mark.parametrize('source', [
    'var count 0', 'call F', 'do', 'if 1', 'each', 'func F',
    'func F x x {}', 'func print {}', 'func MAX {}',
    'func F {}; func F {}', 'do { func F {} }',
    'set value 1', 'set x $missing',
    'number 1 --out x | set x 2',
    'set x 1; func F x {}; F 2',
    'func F x { set x 3 }; F 2',
    'func F x { number 1 --out x }; F 2',
    'func F {}; f', 'func F { F }; F',
    'func F x {}; F --x 1 --x 2',
    'if 1 {} else if 0 {}',
])
def test_errors_are_explicit_and_located(source):
    with pytest.raises(ValueError, match='line'):
        run_stream(source)


def test_function_redefinition_preserves_suspended_invocation():
    runtime = ScriptRuntime()
    runtime.submit('func F { sleep 10; print "old" }; F')
    runtime.tick(0)
    runtime.submit('func F { print "new" }')
    runtime.tick(10)
    runtime.submit('F')
    runtime.run()
    assert runtime.output == ['old', 'new']


def test_failed_validation_does_not_replace_definition_or_touch_hardware():
    hardware = HardwareState()
    runtime = ScriptRuntime(hardware)
    runtime.submit('func F { print "old" }')
    with pytest.raises(ValueError):
        runtime.submit('func F { print "new" }; write 12 1; bogus')
    assert not hardware.gpio
    runtime.submit('F')
    assert runtime.run() == ['old']


def test_global_created_in_another_job_can_make_captured_parameter_ambiguous():
    runtime = ScriptRuntime()
    runtime.submit('func F p { sleep 10; print $p }; F 3')
    runtime.tick(0)
    runtime.submit('set p 4')
    runtime.tick(0)
    with pytest.raises(ValueError, match='ambiguous'):
        runtime.tick(10)
    assert runtime.globals['p'] == 4 and runtime.failures


def test_schedule_captures_parameters_not_globals():
    assert run_stream('set n 1; func F p { schedule { 5 { print "$p:$n" } } }; F 3; set n 2') == ['3:2']


def test_independent_child_survives_parent_error():
    runtime = ScriptRuntime()
    runtime.repeat_count = 2
    runtime.submit('every 10 | print "child"; print $missing')
    with pytest.raises(ValueError):
        runtime.tick(0)
    assert runtime.tasks
    runtime.tick(0)
    runtime.tick(10)
    assert runtime.output == ['child', 'child']


def test_error_and_kill_release_pins_without_cleanup_writes():
    hardware = HardwareState()
    runtime = ScriptRuntime(hardware)
    runtime.submit('write 12 1; sleep 10; print $missing')
    runtime.tick(0)
    with pytest.raises(ValueError):
        runtime.tick(10)
    assert hardware.gpio[12] == 1 and not hardware.pin_owners
    runtime.submit('write 12 0; sleep 10')
    runtime.tick(10)
    runtime.kill(next(iter(runtime.tasks)))
    assert hardware.gpio[12] == 0 and not hardware.pin_owners


def test_variable_capacity_and_failed_growth_preserve_prior_values():
    runtime = ScriptRuntime()
    for i in range(32):
        runtime.submit(f'set v{i} 0')
        runtime.run()
    runtime.submit('set v0 99')
    runtime.run()
    runtime.submit('set extra 1')
    with pytest.raises(ValueError):
        runtime.run()
    assert len(runtime.globals) == 32 and runtime.globals['v0'] == 99


def test_source_registration_has_no_immediate_block_result():
    assert run_stream('do { every 10 | print "tick" } | print "registered"', count=2) == ['tick', 'tick']
    assert run_stream('every 10 | print "tick"; print "registered"', count=2) == ['registered', 'tick', 'tick']


def test_documented_notes_and_blink_program():
    hardware = HardwareState()
    source = (Path(__file__).parents[1] / 'docs/examples/notes-and-blink.cw').read_text()
    assert run_stream(source, hardware=hardware, count=3) == ['Melody finished; LED continues blinking']
    assert [entry[-1] for entry in hardware.tone_history] == [200, 300]
    assert not hardware.pin_owners


def test_reset_clears_globals_failures_and_work_but_preserves_functions():
    runtime = ScriptRuntime()
    runtime.submit('func F { print "retained" }; set n 1; sleep 100')
    runtime.tick(0)
    runtime.reset()
    assert not runtime.globals and not runtime.tasks and not runtime.failures
    runtime.submit('F')
    assert runtime.run() == ['retained']
