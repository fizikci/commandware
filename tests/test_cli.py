"""CLI invocations with no piped stdin still receive the top-level empty event."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def cli(*args, input=''):
    env = dict(os.environ, PYTHONPATH=str(ROOT / 'src'))
    return subprocess.run([sys.executable, '-m', 'commandware', *args], input=input,
                          text=True, capture_output=True, env=env, check=True).stdout


def test_cli_runs_without_stdin_events():
    assert cli('max 5 min 10 3 | print') == '5\n'
    assert cli('print', 'Hello World') == 'Hello World\n'
    assert cli('read 12 | print') == '0\n'


def test_cli_formats_binary64_and_accepts_piped_events():
    assert cli('number 0.1 | print') == '0.10000000000000001\n'
    assert cli('max $value 5 | print', input='2\n8\n') == '5\n8\n'
