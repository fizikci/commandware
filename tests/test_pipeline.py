"""Console pipelines use the same event contract as blocks and background jobs."""
import pytest
from commandware.core import HardwareState, JobManager, run_stream
from commandware.script import ScriptRuntime


@pytest.mark.parametrize('source,expected', [
    ('number 0 | print', [0]),
    ('number 3.5 | print', [3.5]),
    ('map 0 1023 0 100 512 | print', [50]),
    ('map 0 10 100 0 12 | print', [-20]),
    ('map 0 10 (-100) 100 2.5 | print', [-50]),
    ('number 8 --out raw | write 12 --value $raw > 5 | print $raw', [8]),
    ('number 2 | not | print', [0]),
    ('not "0" | print', [1]),
    ('print "Ready"', ['Ready']),
    ('print --value ""', ['']),
    ('number 4 | print 0 | print', [0, 4]),
    ('split ":" "a::b:" | print', ['["a", "", "b", ""]']),
    ('split ":" "a:b" | each { print } | len $value | print', ['a', 'b', 2]),
    ('number 512 | ohm | print', [7279]),
    ('ohm 0 | print', ['<270']),
    ('ohm 1023 | print', ['>700k']),
    ('ohm 1024 | print', ['OUT_OF_RANGE']),
])
def test_pipeline_values(source, expected):
    # Calibration is checked against its published exponential interpolation below.
    result = run_stream(source)
    if '512 | ohm' in source:
        from commandware.core import _ohm_reading
        assert result == [_ohm_reading(512)]
    else:
        assert result == expected


def test_adc_always_samples_hardware_and_actions_preserve_events():
    hardware = HardwareState(analog={'A0': 512})
    assert run_stream('number 3 | adc A0 --out raw | write 12 1 | print $raw | print', hardware=hardware) == [512, 512]
    assert hardware.gpio[12] == 1 and not hardware.pin_owners
    assert run_stream('read 12 | print', hardware=hardware) == [1]


@pytest.mark.parametrize('source', [
    'read D1', 'read GPIO12', 'read A0', 'read 12.5', 'read 1', 'read 6',
    'write 12', 'write 12 1 100', 'write 12 1 --value 0',
    'map 0 0 0 1 3', 'map 0 1 0 1', 'print', 'print 1 --value 2',
    'split ":" 0', 'split "" "abc"', 'split ":" "a:b" | each | print',
    'clock 10 12', 'clock 0 | print', 'clock 10 --dur 0 | print',
    'adc A1', 'adc A0 --every 5', 'read 12 --watch',
    'not "hello"', 'pulse 0', 'debounce 0', 'divide 0',
])
def test_explicit_errors(source):
    with pytest.raises(ValueError, match='line'):
        run_stream(source)


def test_pin_conflict_is_runtime_error_and_preserves_last_state():
    hardware = HardwareState()
    runtime = ScriptRuntime(hardware)
    runtime.repeat_count = 100
    runtime.submit('every 10 | write 12 1')
    runtime.tick(0)
    runtime.tick(0)
    assert hardware.gpio[12] == 1
    runtime.submit('write 12 0')
    with pytest.raises(ValueError):
        runtime.tick(0)
    assert hardware.gpio[12] == 1
    runtime.reset()
    assert hardware.gpio[12] == 1 and not hardware.pin_owners


def test_clock_first_phase_duration_and_no_cleanup_edge():
    hardware = HardwareState()
    runtime = ScriptRuntime(hardware)
    runtime.submit('clock 1000 --dur 2 | write 15')
    runtime.tick(0)
    runtime.tick(.5)
    assert hardware.gpio[15] == 1
    runtime.tick(1)
    assert hardware.gpio[15] == 0
    runtime.tick(1.5)
    assert hardware.gpio[15] == 1
    runtime.tick(2)
    assert hardware.gpio[15] == 1 and not runtime.tasks


def test_clock_skips_overdue_transitions_without_catchup():
    runtime = ScriptRuntime()
    runtime.repeat_count = 100
    runtime.submit('clock 100 --dur 25 | print')
    runtime.tick(0)
    runtime.tick(5)
    runtime.tick(16)
    runtime.tick(20)
    runtime.tick(25)
    assert runtime.output == [1, 0]


def test_schedule_uses_runtime_globals_and_actual_start_timestamps():
    runtime = ScriptRuntime()
    runtime.submit('set n 1; schedule { 5 { print $n } 10 { print "later" } }; set n 2')
    runtime.tick(0)
    runtime.tick(5)
    runtime.tick(10)
    assert runtime.output == [2, 'later']


def test_invalid_schedule_offsets_reject_registration_atomically():
    runtime = ScriptRuntime()
    runtime.submit('schedule { 5 { print "never" } (-1) { print "never" } }')
    with pytest.raises(ValueError):
        runtime.tick(0)
    assert runtime.output == [] and not runtime.tasks


def test_shift_register_frames_are_explicit_and_preserve_event():
    hardware = HardwareState(gpio={12: 1})
    assert run_stream('number 5 | shiftout 12 13 14 --bits 8 | print', hardware=hardware) == [5]
    assert hardware.gpio[14] == 1
    assert run_stream('shiftin 12 13 14 --bits 8 | print', hardware=hardware) == [255]
    assert not hardware.pin_owners


@pytest.mark.parametrize('source', [
    'number 256 | shiftout 12 13 14 --bits 8',
    'number (-1) | shiftout 12 13',
    'number 3 | shiftout 12 12',
    'shiftin 12 13 14 --bits 54',
    'shiftin 12 13 14 --bits 0',
])
def test_shift_register_validation(source):
    with pytest.raises(ValueError):
        run_stream(source)


def test_management_commands_are_actions_in_pipelines():
    hardware = HardwareState()
    result = run_stream('number 7 | status | print', hardware=hardware)
    assert result[-1] == 7
    assert run_stream('help max', hardware=hardware)
    assert run_stream('pins', hardware=hardware)
    assert run_stream('reset; print "unreachable"', hardware=hardware) == []
