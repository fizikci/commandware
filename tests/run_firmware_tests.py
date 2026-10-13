"""Run actual sketch logic on a Windows host with MSVC; no board is required.

Usage: python tests/run_firmware_tests.py
"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".build" / "native-scheduler"
BUILD.mkdir(parents=True, exist_ok=True)
sketch = (ROOT / "firmware/esp12e_commandware/esp12e_commandware.ino").read_text(encoding="utf-8")
# Include all pipeline, parser, and scheduler code verbatim; replace only the
# Arduino IO, web transport, and setup/loop with the test harness.
logic = sketch[sketch.index("enum StageKind"):sketch.index("void setup()")]
logic += sketch[sketch.index("void loop()") :]
(BUILD / "scheduler-under-test.inc").write_text(logic, encoding="utf-8")
vs = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Microsoft Visual Studio"
compiler = sorted(vs.glob("*/Community/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"))[-1]
tools = compiler.parents[3]
kits = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Windows Kits/10"
sdk = sorted((kits / "Include").iterdir())[-1].name
env = dict(os.environ)
env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
exe = BUILD / "scheduler-tests.exe"
subprocess.run([
    str(compiler), "/nologo", "/EHsc", "/std:c++14", "/W3",
    f"/I{tools / 'include'}", f"/I{BUILD}",
    f"/I{ROOT / 'firmware/esp12e_commandware'}",
    *(f"/I{kits / 'Include' / sdk / part}" for part in ("ucrt", "shared", "um")),
    str(ROOT / "tests/firmware_scheduler.cpp"), f"/Fe:{exe}", f"/Fo:{BUILD / 'scheduler-tests.obj'}",
    "/link", f"/LIBPATH:{tools / 'lib/x64'}",
    *(f"/LIBPATH:{kits / 'Lib' / sdk / part / 'x64'}" for part in ("ucrt", "um")),
], env=env, cwd=BUILD, check=True)
tutorial = (ROOT / "docs/tutorial.md").read_text(encoding="utf-8")
song = tutorial.split("## Happy Birthday\n", 1)[1].split("```text\n", 1)[1].split("```", 1)[0].strip()
song_path = BUILD / "happy-birthday.cw"
song_path.write_text(song, encoding="utf-8")
subprocess.run([str(exe), str(song_path)], check=True)
