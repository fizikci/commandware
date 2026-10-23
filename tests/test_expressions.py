"""The same language cases exercise Python and the compiled firmware interpreter."""
from pathlib import Path
import shutil
import subprocess

import pytest

from commandware.core import run_stream
from commandware.script import Event, ScriptRuntime, expression

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def firmware(tmp_path_factory):
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        pytest.skip("C++ compiler is required for firmware expression conformance")
    executable = tmp_path_factory.mktemp("expressions") / "expression-runner"
    subprocess.run([compiler, "-std=c++14", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                    "-I" + str(ROOT / "firmware/esp12e_commandware"),
                    str(ROOT / "tests/expression_runner.cpp"), "-o", str(executable)], check=True)

    def run(source, mode="expression"):
        result = subprocess.run([str(executable), mode], input=source, text=True, capture_output=True, check=True)
        return result.stdout
    return run


CASES = [
    ("max 5 min 10 3", "5"),
    ("MAX --b min --b 3 --a 10 --a 5", "5"),
    ("max 5 (min 10 3)", "5"),
    ("(max 5 (min 10 3))", "5"),
    ("max 5 min 10 3 + 2", "5"),
    ("max 0 ((min 10 3) + 2)", "5"),
    ("max 5 (-1)", "5"),
    ("min ($five + 1) 10", "6"),
    ('number " -0x10 "', "-16"),
    ('int "-200.9"', "-200"),
    ('"20" + "5"', "25"),
    ('"05" == "5"', "0"),
    ('(number "05") == (number "5")', "1"),
    ('"hello" != "world"', "1"),
    ("$zero != 0 && 5 / $zero > 2", "0"),
    ("1 || number $missing", "1"),
    ("0 && number $missing", "0"),
    ("!0 && !5 == 0", "1"),
    ("(1 < 2) == 1", "1"),
    ("1 == (2 > 1)", "1"),
    ("-5 % 3", "-2"),
    ("floor (-1.2)", "-2"),
    ("ceil (-1.2)", "-1"),
    ("abs (-5)", "5"),
    ("clamp 20 0 10", "10"),
    ('notehz "A4"', "440"),
    ('"Inserted $text"', "Inserted $missing"),
    ('"Literal \\$missing"', "Literal $missing"),
    ("0.1 + 0.2", "0.30000000000000004"),
    ("9007199254740993", "9007199254740992"),
    ("-0", "0"),
]


@pytest.mark.parametrize("source,expected", CASES)
def test_shared_expression_values(firmware, source, expected):
    from commandware.script import display
    scope = {"zero": 0, "five": 5, "text": "$missing"}
    assert display(expression(source, scope, Event())) == expected
    assert firmware(source) == "ok:" + expected


ERRORS = [
    ("max 5 -1", "missing expression"),
    ("max 5", "missing expression"),
    ("max 5 10 20", "invalid expression"),
    ("max --a 5 --a 10", "duplicate"),
    ("max --wrong 5 --b 10", "unknown"),
    ("max 5 --b 10", "mix"),
    ("max --a 5 10", "missing named"),
    ("max --a 5 --b 10 --a 20", "invalid expression"),
    ("max(5, 10)", "require spaces"),
    ("int(2.9)", "require spaces"),
    ('number ""', "invalid numeric text"),
    ('number "   "', "invalid numeric text"),
    ('int "hello"', "invalid numeric text"),
    ('int "20O"', "invalid numeric text"),
    ('number "0x1p2"', "invalid numeric text"),
    ('"hello" > 5', "invalid numeric text"),
    ("1 / 0", "division by zero"),
    ("1e308 * 10", "nonfinite"),
    ('number "0x' + 'f' * 300 + '"', "nonfinite"),
    ("clamp 3 10 0", "low exceeds high"),
    ("1 < 2 < 3", "comparison chaining"),
    ("1 == 2 < 3", "comparison chaining"),
    ("1 || (2 + )", "invalid expression"),
    ("max 5 (min 10 3", "closing parenthesis"),
    ("(" * 18 + "1" + ")" * 18, "nesting exceeds"),
]


@pytest.mark.parametrize("source,error", ERRORS)
def test_shared_expression_errors(firmware, source, error):
    with pytest.raises(ValueError, match=error):
        expression(source, {}, Event())
    result = firmware(source)
    assert result.startswith("error:") and error in result


@pytest.mark.parametrize("source", ['0 && (1 / 0)', '1 || number "hello"'])
def test_syntax_validation_never_evaluates_operands(firmware, source):
    assert expression(source, {}, Event(), syntax_only=True) == 1
    assert firmware(source, "syntax") == "ok:1"


@pytest.mark.parametrize("source,expected", [
    ("max 5 min 10 3 | print", "5\n"),
    ("max --a 5 --b min --a 10 --b 3 --out result | print $result", "5\n"),
    ("set result max 5 min 10 3; print $result", "5\n"),
    ("print 1 + 2 * 3", "7\n"),
    ("print 0 || 1", "1\n"),
    ("print 1 || $missing", "1\n"),
    ('min 5 10 | print "Result $value" | print', "Result 5\n5\n"),
    ('string 0.1 | print', "0.10000000000000001\n"),
    ('max 5 10 --out min | print $min', "10\n"),
    ('set max 5; set max min $max 3; print $max', "3\n"),
])
def test_shared_script_arguments(firmware, source, expected):
    runtime = ScriptRuntime()
    runtime.submit(source)
    runtime.run()
    from commandware.script import display
    assert "\n".join(map(display, runtime.output)) + "\n" == expected
    assert firmware(source, "script") == "ok:" + expected


@pytest.mark.parametrize("source", ["print 1 +", "max 5 -1", "max --a 5 --a 10", "print 1 || (2 + )"])
def test_invalid_arguments_reject_submission_before_actions(firmware, source):
    from commandware.core import HardwareState
    hardware = HardwareState()
    with pytest.raises(ValueError):
        ScriptRuntime(hardware).submit("write 12 1; " + source)
    assert not hardware.gpio
    assert firmware('print "Should not execute"; ' + source, "script").startswith("error:")


def test_console_routes_value_functions_to_script_runtime():
    assert run_stream("max 5 min 10 3 | print") == [5]
    assert run_stream('number "20" | abs $value | print') == [20]
    assert run_stream('max 5 10 | print $value > 5 || $missing') == [1]
    assert run_stream('set x 0; print 1 || $x / 0') == [1]
