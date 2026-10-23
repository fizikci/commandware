"""Every command inserted by the web UI is checked against the language runtime."""
import json
from pathlib import Path
import shutil
import subprocess

import pytest
from commandware.core import run_stream

ROOT = Path(__file__).resolve().parents[1]


def test_web_sample_commands_execute_with_current_language():
    node = shutil.which('node')
    if not node:
        pytest.skip('Node is required to extract the actual web UI sample array')
    source = (ROOT / 'firmware/esp12e_commandware/esp12e_commandware.ino').read_text()
    sample_code = source.split('    const p=', 1)[1].split('    let busy=', 1)[0].strip().removesuffix(';')
    result = subprocess.run([node, '-e', 'const vm=require("node:vm");const fs=require("node:fs");'
                             'const p=vm.runInNewContext("("+fs.readFileSync(0,"utf8")+")");'
                             'process.stdout.write(JSON.stringify(p.flatMap(g=>g.items.map(i=>[i[0],i[2]]))));'],
                            input=sample_code, text=True, capture_output=True, check=True)
    for title, command in json.loads(result.stdout):
        if command.startswith('kill '):
            continue  # The UI explicitly asks the user to substitute an active ID.
        try:
            run_stream(command, count=2)
        except ValueError as exc:
            pytest.fail(f'Web sample {title!r} rejected: {exc}')
