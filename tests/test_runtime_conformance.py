"""Execution contracts across Python and the actual portable firmware runtime."""
from pathlib import Path
import shutil
import subprocess
import pytest
from commandware.script import ScriptRuntime, display

ROOT = Path(__file__).resolve().parents[1]

@pytest.fixture(scope="module")
def rich_firmware(tmp_path_factory):
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        pytest.skip("C++ compiler required")
    executable = tmp_path_factory.mktemp("runtime-conformance") / "runner"
    subprocess.run([compiler, "-std=c++14", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                    "-I" + str(ROOT / "firmware/esp12e_commandware"),
                    str(ROOT / "tests/runtime_conformance_runner.cpp"), "-o", str(executable)], check=True)
    return lambda source: subprocess.run([str(executable)], input=source, text=True,
                                         capture_output=True, check=True).stdout


def simulator(source):
    runtime = ScriptRuntime()
    runtime.repeat_count = 10000
    try:
        for part in source.split("\n---NEXT---\n"):
            runtime.submit(part)
            runtime.tick(0)
        for ms in range(201):
            runtime.tick(ms)
            if not runtime.tasks:
                break
    except ValueError as exc:
        return ''.join(display(v) + '\n' for v in runtime.output) + 'ERROR:' + str(exc)
    return ''.join(display(v) + '\n' for v in runtime.output)


@pytest.mark.parametrize("source,expected", [
    ('number 7 | do {} | print', '7\n'),
    ('number 7 | do { number 3; print } | print', '7\n7\n'),
    ('number 7 | do { number 3; number 4 } | print', '4\n'),
    ('number 7 | if 0 { number 3 } | print', '7\n'),
    ('number 7 | if 0 { number 3 } else { number 4 } | print', '4\n'),
    ('number 7 | do { number 3 | where 0 } | print; print "after"', 'after\n'),
    ('number 7 | do { every 10 | print } | print "registered"', '0\n'*21),
    ('set n 0; do { set n 3; print $n }; print $n', '3\n3\n'),
    ('F 4; func F x { max $x 3 | print }', '4\n'),
    ('func F x y { min $x $y }; F --y 3 --x max 2 8 | print', '3\n'),
    ('func F { toggle }; F | print; F | print', '1\n1\n'),
    ('split ":" "a:b" | each { print }; print "end"', 'a\nb\nend\n'),
    ('split ":" "a:b" | each --out item { print $item } | len $value | print', 'a\nb\n2\n'),
    ('number 2 --out x | number 3 --out x | print $x', '3\n'),
    ('func F p { schedule { 3 { print $p } } }; F 6', '6\n'),
    ('set g 1; schedule { 5 { print $g } }; set g 2', '2\n'),
    ('reset; print "after"', ''),
    ('kill 1; print "after"', ''),
    ('print "Literal \\$missing"', 'Literal $missing\n'),
    ('print "\ue000"', '\ue000\n'),
    ('func F { sleep 10; print "old" }; F; schedule {20 { F }}\n---NEXT---\nfunc F { print "new" }', 'old\nnew\n'),
    ('pulse 10 | print', '1\n0\n'),
    ('pulse 10 | sleep 20 | print', '1\n0\n'),
    ('do { pulse 10 } | print', '1\n0\n'),
    ('func F { pulse 10 }; F | print', '1\n0\n'),
    ('do { pulse 10; number 7 } | print', '7\n'),
    ('do { do { pulse 10 } } | print', '1\n0\n'),
    ('pulse 10 --out bit | print $bit', '1\n0\n'),
    ('clock 100 --dur 21 | edge | print', '0\n1\n0\n'),
    ('clock 100 --dur 21 | divide 2 | print', '0\n1\n'),
    ('clock 100 --dur 21 | debounce 2 | print', '1\n0\n1\n0\n'),
    ('clock 100 --dur 21 | pulse 7 | print', '1\n0\n'),
    ('schedule { 1 + 2 { print "x" } }', 'x\n'),
    ('schedule { max 1 3 { print "x" } }', 'x\n'),
    ('split ":" "0:0:1:1:0:0:1:0" | each { number $value | divide 2 | print }', '0\n1\n0\n'),
    ('split ":" "0:0:1:1:0:0:1:0" | each { number $value | edge | print }', '1\n0\n1\n0\n'),
    ('split ":" "0:0:1:1:0:0:1:0" | each { number $value | rising | print }', '1\n1\n'),
    ('split ":" "0:0:1:1:0:0:1:0" | each { number $value | falling | print }', '0\n0\n'),
])
def test_execution_contracts(rich_firmware, source, expected):
    assert simulator(source) == expected
    assert rich_firmware(source) == expected


@pytest.mark.parametrize("source", [
    'do', 'if 1', 'func F', 'each', 'schedule',
    'func F x x { print }; F 1 2',
    'func print {}', 'do { func F {} }',
    'func F {}; func F {}',
    'set p 1; func F p { print $p }; F 2',
    'number 1 --out p | set p 2',
    'func F p { set p 2 }; F 1',
    'func F p { number 1 --out p }; F 1',
    'number 1 --out p | func F p {}',
    'func F { F }; F',
    'func F x { print $x }; F --x 1 --x 2',
    'print 1; if 1 { print 2 } else if 0 { print 3 }',
    'set value 1',
])
def test_execution_errors(rich_firmware, source):
    assert 'ERROR:' in simulator(source)
    assert 'ERROR:' in rich_firmware(source)


@pytest.mark.parametrize("bad", [
    "shiftin 1", "shiftout 1", "tone 12 --hz 2", "write 12 --dur 4",
    "read 12 5", "adc A1", "map 0 10 0", "print 1 2",
])
def test_invalid_unselected_branch_prevents_prior_actions(rich_firmware, bad):
    source = 'print "must not execute"; if 0 { ' + bad + ' }'
    assert simulator(source).startswith('ERROR:')
    assert rich_firmware(source).startswith('ERROR:')


def test_rejected_definitions_preserve_previous_function():
    runtime = ScriptRuntime()
    runtime.submit('func F { print "old" }')
    with pytest.raises(ValueError):
        runtime.submit('func F { print "new" }; unknown')
    runtime.submit('F')
    assert runtime.run() == ['old']


def test_active_definitions_count_against_program_budget():
    runtime = ScriptRuntime()
    for i in range(4):
        runtime.submit('func F { sleep 1000; print ' + str(i) + ' }; F #' + 'x'*3900)
        runtime.tick(0)
    prior = runtime.functions['F']
    with pytest.raises(ValueError, match='retained program'):
        runtime.submit('func F { print 99 } #' + 'x'*3900)
    assert runtime.functions['F'] is prior


def test_run_with_waiting_timer_continuation_terminates():
    import os
    import sys
    environment = dict(os.environ, PYTHONPATH=str(ROOT / 'src'))
    result = subprocess.run([sys.executable, '-c',
        "from commandware.script import ScriptRuntime; r=ScriptRuntime(); "
        "r.submit('pulse 10 | sleep 20 | print'); r.run(); print(r.output)"],
        text=True, capture_output=True, timeout=3, check=True, env=environment)
    assert result.stdout.strip() == '[1, 0]'
