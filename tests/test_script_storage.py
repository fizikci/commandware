"""Saved-script semantics in the simulator (flash failures use the native harness)."""
import pytest
from commandware.script import Event, ScriptRuntime


def execute(runtime, source, events=None):
    runtime.submit(source, events=events)
    runtime.tick(runtime.now)


def test_script_round_trip_and_reset():
    runtime = ScriptRuntime()
    source = '  print "$literal é雪"\n\n'
    execute(runtime, 'script save "OLED demo"', [Event(value=source)])
    execute(runtime, 'script load "OLED demo" --out source | script save copy | print $source')
    assert runtime.output == [source]
    execute(runtime, 'script list | print')
    assert runtime.output[-1] == '["OLED demo", "copy"]'
    execute(runtime, 'reset')
    execute(runtime, 'script load copy | print')
    assert runtime.output[-1] == source
    execute(runtime, 'number 7 | script delete copy | print')
    assert runtime.output[-1] == 7
    assert 'copy' not in runtime.saved_scripts


def test_full_size_and_empty_sources():
    runtime = ScriptRuntime()
    for source in ('', 'é' * 2048):
        execute(runtime, 'script save demo', [Event(value=source)])
        execute(runtime, 'script load demo | print')
        assert runtime.output[-1] == source
    assert not runtime.failures
    assert runtime.saved_scripts['demo'] == 'é' * 2048


@pytest.mark.parametrize('command', [
    'script save "../bad"', 'script save "a/b"', 'script save "a.b"',
    'script save " leading"', 'script save "trailing "', 'script save "é"',
    'script save ""', 'script save "abcdefghijklmnopqrstuvwxyz123"',
    'script save demo', 'number 1 | script save demo',
    'script load missing', 'script delete missing',
])
def test_invalid_operations_preserve_saved_scripts(command):
    runtime = ScriptRuntime()
    execute(runtime, 'string "original" | script save demo')
    with pytest.raises(ValueError):
        execute(runtime, command)
    assert runtime.saved_scripts == {'demo': 'original'}


def test_script_count_and_syntax():
    runtime = ScriptRuntime()
    for index in range(32):
        execute(runtime, f'string "x" | script save s{index}')
    execute(runtime, 'string "updated" | script save s0')
    with pytest.raises(ValueError, match='limit is 32'):
        execute(runtime, 'string "x" | script save overflow')
    assert runtime.saved_scripts['s0'] == 'updated'
    for source in ('script', 'script list extra', 'script unknown demo',
                   'script load', 'script save demo --out x', 'script delete demo --out x'):
        with pytest.raises(ValueError):
            runtime.submit(source)
