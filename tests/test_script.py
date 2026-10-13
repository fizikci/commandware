import pytest

from commandware.core import HardwareState, JobManager, run_stream
from commandware.script import ScriptRuntime


NOTES = '''
func notes str {
    split " " --value $str |
        each --out note {
            split ":" --value $note --out pitch,dur |
                tone 15 --hz notehz($pitch) --dur int($dur)
        }
}
'''


def test_notes_wait_for_duration_while_led_and_console_remain_available():
    hw = HardwareState()
    runtime = ScriptRuntime(hw)
    runtime.submit(NOTES + '''
do {
    every 50 | toggle | write 12
    call notes "C4:200 E4:300"
    print "finished"
}
''')
    runtime.tick(0)
    assert hw.tone_history == [(0, 15, pytest.approx(261.625565), 200)]
    runtime.tick(0)
    assert hw.gpio[12] == 1
    runtime.tick(50)
    assert hw.gpio[12] == 0
    runtime.submit('print "console works"; sleep 1')
    runtime.tick(50)
    assert runtime.output == ["console works"]
    runtime.tick(199)
    assert len(hw.tone_history) == 1
    runtime.tick(200)
    assert hw.tone_history[-1] == (200, 15, pytest.approx(329.627557), 300)
    runtime.tick(499)
    assert "finished" not in runtime.output
    runtime.tick(500)
    assert "finished" in runtime.output
    runtime.reset()
    assert not runtime.tasks and not hw.pin_owners
    assert hw.gpio[12] == hw.gpio[15] == 0


def test_named_fields_survive_sampling_mapping_and_setting_counter():
    hw = HardwareState(analog={"A0": 512})
    result = run_stream('''do {
        var count 0
        every 100 |
            adc A0 --out raw |
            map 0 1023 0 100 --value $raw --out percent |
            set count $count + 1 |
            print "$count:$raw:$percent"
    }''', hardware=hw, count=3)
    assert result == ["1:512:50", "2:512:50", "3:512:50"]
    assert "count" not in hw._script_runtime.globals


def test_functions_persist_and_calls_have_independent_local_variables():
    hw = HardwareState()
    run_stream('''func counter {
        var count 0
        every 10 | set count $count + 1 | print $count
    }''', hardware=hw)
    assert run_stream('do { call counter; call counter }', hardware=hw, count=2) == [1, 1, 2, 2]
    run_stream("var time 0", hardware=hw)
    run_stream("set time $time + int(2.9) * 3", hardware=hw)
    assert run_stream("print $time", hardware=hw) == [6]


def test_pipe_each_and_block_each_both_wait_for_downstream_completion():
    for each in ('each --out item | print $item | sleep 10',
                 'each --out item { print $item; sleep 10 }'):
        runtime = ScriptRuntime()
        runtime.submit('split " " --value "A B C" | ' + each)
        runtime.tick(0)
        assert runtime.output == ["A"]
        runtime.tick(9)
        assert runtime.output == ["A"]
        runtime.tick(10)
        assert runtime.output == ["A", "B"]
        runtime.tick(20)
        assert runtime.output == ["A", "B", "C"]


def test_function_returns_list_with_output_binding():
    result = run_stream('''func pair str {
        split ":" --value $str
    }
    call pair "C4:200" --out pitch,dur | print "$pitch $dur"
    ''')
    assert result == ["C4 200"]


@pytest.mark.parametrize("source", [
    'do { write 12 1; unknown }',
    'do { print "unclosed }',
    'do { print "ok"',
    'do { call missing }',
    'func f x { print $x }; call f',
])
def test_static_errors_have_no_hardware_effects(source):
    hw = HardwareState()
    with pytest.raises(ValueError):
        run_stream(source, hardware=hw)
    assert hw.gpio == {}


@pytest.mark.parametrize("source,match", [
    ('var time = 0', 'invalid expression'),
    ('set missing 1', 'invalid variable'),
    ('do { var a 0; adc A0 --out a }', 'duplicate output'),
    ('split ":" --value "a:b:c" --out a,b', 'output count'),
    ('print $missing', 'unknown name'),
    ('var a 1 / 0', 'division by zero'),
    ('sleep 0', 'integer milliseconds'),
    ('every 1.5 | print $value', 'integer milliseconds'),
    ('tone 1 --hz 440 --dur 20', 'invalid or reserved'),
    ('tone 15 --hz nan --dur 20', 'frequency'),
    ('func recurse { call recurse }; call recurse', 'depth exceeds'),
])
def test_runtime_errors_are_explicit(source, match):
    with pytest.raises(ValueError, match=match):
        run_stream(source)


def test_error_and_kill_release_acquired_pins():
    hw, jobs = HardwareState(), JobManager()
    runtime = ScriptRuntime(hw, jobs)
    runtime.submit('do { write 12 1; sleep 100; print $missing }')
    runtime.tick(0)
    assert hw.gpio[12] == 1
    with pytest.raises(ValueError, match="unknown name"):
        runtime.tick(100)
    assert hw.gpio[12] == 0 and not hw.pin_owners and not jobs.jobs
    runtime.submit('tone 15 --hz 440 --dur 100')
    runtime.tick(100)
    job_id = next(iter(runtime.tasks))
    assert runtime.kill(job_id)
    assert not runtime.kill(job_id)
    assert hw.gpio[15] == 0 and not hw.pin_owners


def test_comments_quoted_separators_and_multiline_continuations():
    assert run_stream('''do {
        # { " these do not affect parsing
        split ':' --value 'a|{b}:c' |
            each --out part |
            print $part
    }''') == ["a|{b}", "c"]


def test_documented_program_runs_unchanged():
    from pathlib import Path
    hw = HardwareState()
    source = (Path(__file__).parents[1] / "docs/examples/notes-and-blink.cw").read_text()
    assert run_stream(source, hardware=hw, count=3) == ["Melody finished; LED continues blinking"]
    assert [(time, duration) for time, _, _, duration in hw.tone_history] == [(0, 200), (200, 300)]
    assert [(time, value) for time, pin, value in hw.script_writes if pin == 12] == [(0, 1), (500, 0), (1000, 1), (1500, 0)]


def test_legacy_background_sources_and_schedules_inside_do():
    hw = HardwareState()
    assert run_stream('''do {
        var offset 10
        schedule {
            $offset { clock 100 --dur 20 | write 15 }
            30 { clock 100 --dur 20 | write 15 }
        }
        every 10 | toggle | write 12
        print "started"
    }''', hardware=hw, count=5) == ["started"]
    assert not hw.pin_owners
    assert hw.gpio[12] == hw.gpio[15] == 0
    assert any(time == 15 and pin == 15 and value == 1 for time, pin, value in hw.script_writes)
    assert run_stream('do { clock 100 --dur 20 | print; print "started" }') == ["started", 1, 0, 1]


def test_simulator_file_loading_and_shell_job_management():
    assert run_stream("run docs/examples/notes-and-blink.cw") == ["Melody finished; LED continues blinking"]
    hw = HardwareState()
    runtime = ScriptRuntime(hw)
    runtime.submit("every 10 | toggle | write 12")
    runtime.tick(0)
    runtime.tick(0)
    job_id = next(iter(runtime.tasks))
    assert run_stream("jobs", hardware=hw)[0].startswith(f"{job_id} RUNNING")
    assert run_stream(f"kill {job_id}", hardware=hw) == [f"killed {job_id}"]
    assert hw.gpio[12] == 0
    run_stream("var persistent 1", hardware=hw)
    run_stream("reset", hardware=hw)
    assert runtime.globals == {}


def test_boolean_negation_toggles_variable_and_explicit_write_value():
    hw = HardwareState()
    run_stream('var bit 0\nevery 500 | set bit !$bit | write 5 $bit', hardware=hw, count=4)
    assert hw.script_writes[:4] == [(0, 5, 1), (500, 5, 0), (1000, 5, 1), (1500, 5, 0)]


def test_var_redeclaration_replaces_value_in_current_scope_and_across_submissions():
    hw = HardwareState()
    assert run_stream('var count 10; var count 0; print $count', hardware=hw) == [0]
    assert run_stream('var count "reset"; print $count', hardware=hw) == ["reset"]
    assert run_stream('do { var count 10; var count $count + 1; print $count }; print $count', hardware=hw) == [11, "reset"]
    assert run_stream('func reset_count count { var count 0; print $count }; call reset_count 10', hardware=hw) == [0]


def test_var_redeclaration_at_variable_limit_does_not_use_another_slot():
    hw = HardwareState()
    source = '; '.join(f'var v{i} {i}' for i in range(32))
    assert run_stream(source + '; var v0 99; print $v0', hardware=hw) == [99]
    with pytest.raises(ValueError, match='variable limit is 32'):
        run_stream('var extra 0', hardware=hw)


def test_var_redeclaration_still_rejects_event_field_conflicts():
    with pytest.raises(ValueError, match='invalid variable'):
        run_stream('adc A0 --out raw | var raw 0')


@pytest.mark.parametrize('expr,expected', [('!0', 1), ('!2', 0), ('!!2', 1), ('!0 + 2', 3), ('!(1 + 2)', 0)])
def test_boolean_negation_precedence(expr, expected):
    assert run_stream(f'var result {expr}\nprint $result') == [expected]


def test_missing_dollar_in_negation_is_rejected_before_job_starts():
    hw = HardwareState()
    runtime = ScriptRuntime(hw)
    with pytest.raises(ValueError, match=r'\$name'):
        runtime.submit('var bit 0\nevery 500 | set bit !bit | write 5')
    assert runtime.tasks == {} and runtime.globals == {} and hw.script_writes == []


@pytest.mark.parametrize('source,expected', [
    ('split " " "a b" | each | print', ["a", "b"]),
    ('split " " --value "a b" | each | print', ["a", "b"]),
    ('var str "a b"; split " " $str | each | print', ["a", "b"]),
    ('adc A0 | split "1" | each | print', ["5", "2"]),
    ('adc A0 | split " " "a b" | each | print', ["a", "b"]),
    ('adc A0 | split " " --value "a b" | each | print', ["a", "b"]),
    ('split ":" 0 | each | print', ["0"]),
    ('split ":" "" | each | print', [""]),
    ('adc A0 | map 0 1023 0 0 | split ":" | each | print', ["0"]),
    ('split ":" "a:" | each | split ":" | each | print', ["a", ""]),
])
def test_split_accepts_positional_named_or_piped_value(source, expected):
    assert run_stream(source, hardware=HardwareState(analog={"A0": 512})) == expected


@pytest.mark.parametrize('source,message,line', [
    ('do {\n print "ready"\n\n # comment\n split " "\n}', 'split: value is required through pipe or explicit parameter --value', 5),
    ('\n\nunknown', "unsupported script command", 3),
    ('do {\n print "ready"\n set bit !bit\n}', 'invalid expression', 3),
    ('do {\n sleep 10\n print $missing\n}', 'unknown name', 3),
    ('do {\n every 10 |\n print $missing\n}', 'unknown name', 3),
    ('do {\n print "unterminated\n}', 'unterminated string', 2),
    ('do {\n print "ok"\n', 'missing closing brace', 3),
    ('do {\n split " " --value\n}', "invalid arguments for 'split'", 2),
    ('do {\n lcd init 0\n}', 'address', 2),
    ('do {\n schedule {\n 10 {\n lcd init 0\n }\n }\n}', 'address', 4),
    ('schedule {\n 10 {\n lcd init 0\n }\n}', 'address', 3),
])
def test_script_errors_include_source_line(source, message, line):
    with pytest.raises(ValueError) as failure:
        run_stream(source)
    assert message in str(failure.value)
    assert str(failure.value).endswith(f' at line {line}')


def test_function_error_keeps_definition_line_across_submissions():
    hw = HardwareState()
    run_stream('func broken {\n sleep 10\n print $missing\n}', hardware=hw)
    with pytest.raises(ValueError, match="unknown name 'missing' at line 3$"):
        run_stream('call broken', hardware=hw)


def test_legacy_errors_include_line_one():
    with pytest.raises(ValueError, match=' at line 1$'):
        run_stream('read')


def test_legacy_file_error_uses_file_line(tmp_path):
    script = tmp_path / 'broken.cw'
    script.write_text('# comment\n\nread\n')
    with pytest.raises(ValueError, match=' at line 3$'):
        run_stream(f'run "{script.as_posix()}"')
