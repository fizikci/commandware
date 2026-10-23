"""Unicode, list, allocation, and rounding conformance in both value engines."""
import json
from pathlib import Path
import shutil
import subprocess

import pytest

from commandware.expressions import Expression, format_number

ROOT = Path(__file__).resolve().parents[1]
SCOPE = {"items": ["é", "雪", ""], "mixed": [2, "03"], "empty": [],
         "oversized": [1] * 65, "bad": "\ud800"}


@pytest.fixture(scope="module")
def value_firmware(tmp_path_factory):
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        pytest.skip("C++ compiler required")
    executable = tmp_path_factory.mktemp("value-functions") / "runner"
    subprocess.run([compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(ROOT / "firmware/esp12e_commandware"),
                    str(ROOT / "tests/value_function_runner.cpp"), "-o", str(executable)], check=True)
    return lambda source: subprocess.run([str(executable)], input=source, text=True,
                                         capture_output=True, check=True).stdout


def display(value):
    if isinstance(value, list):
        return "[" + ", ".join(json.dumps(x, ensure_ascii=False) if isinstance(x, str) else format_number(x) for x in value) + "]"
    return value if isinstance(value, str) else format_number(value)


@pytest.mark.parametrize("source,expected", [
    ('round 2.5', '3'), ('round (-2.5)', '-3'), ('round 0.49999999999999994', '0'),
    ('round 4503599627370495.5', '4503599627370496'), ('round 1e308', '1e+308'),
    ('len "é雪😀"', '3'), ('slice "é雪😀" 1 3', '雪😀'),
    ('slice --end 2 --x "é雪😀" --start 0', 'é雪'),
    ('len $items', '3'), ('at $items 1', '雪'),
    ('slice $items 0 2', '["é", "雪"]'), ('slice $empty 0 0', '[]'),
    ('join $items ":"', 'é:雪:'), ('join $empty ":"', ''),
    ('contains $mixed "2"', '1'), ('contains $mixed "3"', '0'),
    ('contains $mixed 3', '1'), ('contains "é雪" "雪"', '1'),
    ('contains "abc" ""', '1'), ('contains $empty 2', '0'),
    ('lower "AZÉİ"', 'azÉİ'), ('upper "azéß"', 'AZéß'),
    ('trim "  é雪  "', 'é雪'), ('trim "\u00a0a\u00a0"', '\u00a0a\u00a0'),
    ('replace "aaaaa" "aa" "X"', 'XXa'), ('replace "é雪é" "é" "😀"', '😀雪😀'),
    ('replace "" "x" "z"', ''), ('len "' + 'é' * 512 + '"', '512'),
    ('len join $items ":"', '4'),
])
def test_value_results(value_firmware, source, expected):
    assert display(Expression(source, SCOPE.__getitem__).run()) == expected
    assert value_firmware(source) == 'ok:' + expected


@pytest.mark.parametrize("source,error", [
    ('len 2', 'expected string'), ('trim 2', 'expected string'),
    ('join $mixed ","', 'expected string'), ('join "abc" ","', 'expected list'),
    ('at $empty 0', 'index out of range'), ('at $items (-1)', 'nonnegative integer'),
    ('at $items 0.5', 'nonnegative integer'), ('at $items "1"', 'nonnegative integer'),
    ('slice "abc" 2 1', 'invalid slice range'), ('slice "abc" 0 4', 'range'),
    ('replace "abc" "" "x"', 'empty replacement search'),
    ('contains "abc" 2', 'expected string'), ('contains $items $empty', 'list comparison'),
    ('string $items', 'expected numeric value'), ('"$items"', 'expected numeric value'),
    ('len $oversized', 'list exceeds 64'), ('len $bad', 'invalid UTF-8'),
    ('len "' + 'é' * 2049 + '"', 'string exceeds 4096'),
    ('replace "' + 'x' * 2100 + '" "x" "yy"', 'string exceeds 4096'),
    ('join $items "' + 'x' * 2048 + '"', 'string exceeds 4096'),
])
def test_value_errors(value_firmware, source, error):
    with pytest.raises(ValueError, match=error):
        Expression(source, SCOPE.__getitem__).run()
    assert error in value_firmware(source)
