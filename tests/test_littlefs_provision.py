"""Exercise provisioning upload gates with a CLI double; never access hardware."""
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture
def provision(tmp_path):
    cli = tmp_path / 'arduino-cli'
    log = tmp_path / 'calls.jsonl'
    cli.write_text(f'#!{sys.executable}\n' +
                   'import json, os, sys\n' +
                   'with open(os.environ["PROVISION_TEST_LOG"], "a") as out:\n' +
                   '    out.write(json.dumps(sys.argv[1:]) + "\\n")\n')
    cli.chmod(0o755)
    env = dict(os.environ, PATH=str(tmp_path) + os.pathsep + os.environ['PATH'],
               PROVISION_TEST_LOG=str(log))

    def run(args, answer=''):
        result = subprocess.run(['bash', str(ROOT / 'provision-littlefs.sh'), *args],
                                input=answer, text=True, capture_output=True, env=env)
        calls = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return result, calls
    return run


def test_build_only_never_uploads(provision):
    result, calls = provision(['--build-only'])
    assert result.returncode == 0
    assert [call[0] for call in calls] == ['compile', 'compile']
    assert all(call[call.index('--fqbn') + 1].endswith(':wipe=none') for call in calls)
    assert 'nothing uploaded or formatted' in result.stdout


@pytest.mark.parametrize('args', [[], ['--fqbn'], ['--fqbn', 'esp32:esp32:esp32', 'COM3'],
                                     ['--fqbn', 'esp8266:esp8266:generic:wipe=all', 'COM3'],
                                     ['--fqbn', 'esp8266:esp8266:generic:wipe=sdk', 'COM3'],
                                     ['--fqbn', 'esp8266:esp8266:generic:wipe=none,wipe=all', 'COM3'],
                                     ['--fqbn', 'esp8266:esp8266:generic:wipe=none,wipe=none', 'COM3']])
def test_invalid_requests_do_not_build_or_upload(provision, args):
    result, calls = provision(args)
    assert result.returncode != 0
    assert calls == []


def test_upload_cancelled_by_default(provision):
    result, calls = provision(['COM3'], 'no\n')
    assert result.returncode == 0
    assert [call[0] for call in calls] == ['compile', 'compile']
    assert 'Cancelled' in result.stdout


def test_only_explicit_upload_and_matching_restore_layout(provision):
    result, calls = provision(['--fqbn', 'esp8266:esp8266:generic:eesz=4M1M', 'COM3'],
                              'UPLOAD FORMATTER\n')
    assert result.returncode == 0
    assert [call[0] for call in calls] == ['compile', 'compile', 'upload']
    assert all(call[call.index('--fqbn') + 1] ==
               'esp8266:esp8266:generic:eesz=4M1M,wipe=none' for call in calls)
    assert calls[-1][-1].endswith('/tools/provision_littlefs')
    assert '--input-dir' in result.stdout and 'FORMAT LITTLEFS' in result.stdout
