"""Finite block/item completion barriers coexist with retriggerable sources."""
import pytest

from commandware.script import ScriptRuntime
from test_runtime_conformance import rich_firmware, simulator


@pytest.mark.parametrize('source,expected', [
    ('do { pulse 10 | print; print "finished" }', '1\n0\nfinished\n'),
    ('split ":" "A:B" | each { pulse 10 | print }', '1\n0\n1\n0\n'),
    ('func F { pulse 10 | print }; F; print "finished"', '1\n0\nfinished\n'),
    ('do { do { pulse 10 | print }; print "finished" }', '1\n0\nfinished\n'),
])
def test_finite_statement_and_item_completion(rich_firmware, source, expected):
    assert simulator(source) == expected
    assert rich_firmware(source) == expected


def test_sibling_statement_waits_for_filtered_timer_completion():
    runtime = ScriptRuntime()
    runtime.submit('do { pulse 10 | where $value == 1 | print; print "finished" }')
    runtime.tick(0)
    assert runtime.output == [1]
    runtime.tick(9)
    assert runtime.output == [1]
    runtime.tick(10)
    assert runtime.output == [1, 'finished']
    assert not runtime.tasks


def test_repeating_source_still_retriggers_pending_stage():
    runtime = ScriptRuntime()
    runtime.repeat_count = 3
    runtime.submit('every 5 | pulse 10 | print')
    runtime.tick(0)
    runtime.tick(0)
    assert runtime.output == [1]
    for time in (5, 10, 15, 19):
        runtime.tick(time)
    assert runtime.output == [1]
    runtime.tick(20)
    assert runtime.output == [1, 0]
