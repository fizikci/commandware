"""Current guide/tutorial fences are runnable programs, each in a fresh session."""
from pathlib import Path
import re
import pytest
from commandware.script import ScriptRuntime

ROOT = Path(__file__).resolve().parents[1]
CASES = []
for name in ('language.md', 'tutorial.md'):
    text = (ROOT / 'docs' / name).read_text()
    for index, match in enumerate(re.finditer(r'```text\n(.*?)\n```', text, re.S), 1):
        CASES.append(pytest.param(match[1], id=f'{name}-block-{index}'))
for path in sorted((ROOT / 'docs/examples').glob('*.cw')):
    CASES.append(pytest.param(path.read_text(), id=path.name))

@pytest.mark.parametrize('source', CASES)
def test_documented_programs_execute(source):
    runtime = ScriptRuntime()
    runtime.submit(source)
    runtime.run(count=3)
    assert not runtime.failures


def test_complete_birthday_preserves_note_timeline():
    tutorial = (ROOT / 'docs/tutorial.md').read_text()
    source = re.search(r'```text\n(func birthday .*?)\n```', tutorial, re.S)[1]
    runtime = ScriptRuntime()
    runtime.submit(source)
    runtime.run()
    assert [note[0] for note in runtime.hardware.tone_history] == [
        0, 250, 500, 1000, 1500, 2000, 3000, 3250, 3500, 4000, 4500, 5000,
        6000, 6250, 6500, 7000, 7500, 8000, 8500, 9500, 9750, 10000, 10500,
        11000, 11500,
    ]
    assert runtime.now == 12400
    assert runtime.hardware.gpio[15] == 0
