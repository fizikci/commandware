"""Compile and run actual firmware logic on macOS/Linux (Clang/GCC) or Windows (MSVC).

No board is required. Run: python tests/run_firmware_tests.py
"""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / '.build' / 'native-scheduler'
BUILD.mkdir(parents=True, exist_ok=True)
sketch = (ROOT / 'firmware/esp12e_commandware/esp12e_commandware.ino').read_text(encoding='utf-8')
logic = sketch[sketch.index('#include "ScriptRuntime.h"'):sketch.index('void setup()')]
logic += sketch[sketch.index('void loop()'):]
(BUILD / 'scheduler-under-test.inc').write_text(logic, encoding='utf-8')
web = sketch[sketch.index('class StringPrint'):sketch.index('static void handleCommand')]
web += sketch[sketch.index('static void configureWebServer()'):sketch.index('static void serviceWebServer()')]
(BUILD / 'web-under-test.inc').write_text(web, encoding='utf-8')

provision = (ROOT / 'tools/provision_littlefs/provision_littlefs.ino').read_text(encoding='utf-8')
(BUILD / 'provision-under-test.inc').write_text(provision[provision.index('static bool finished'):], encoding='utf-8')

compiler = shutil.which('clang++') or shutil.which('g++')
sources = [('firmware_scheduler.cpp', []), ('runtime_contract.cpp', []),
           ('script_storage.cpp', []), ('script_storage.cpp', ['ESP32']),
           ('littlefs_provision.cpp', [])]
if compiler:
    for source, defines in sources:
        executable = BUILD / (Path(source).stem + ('-esp32' if defines else ''))
        subprocess.run([compiler, '-std=c++14', '-Wall', '-Wextra',
                        '-Wno-missing-field-initializers', *['-D' + define for define in defines], '-I' + str(ROOT / 'tests/fakes'), '-I' + str(BUILD),
                        '-I' + str(ROOT / 'firmware/esp12e_commandware'),
                        str(ROOT / 'tests' / source), '-o', str(executable)], check=True)
        subprocess.run([str(executable), str(ROOT / 'docs/examples/pong.cw')], check=True)
else:
    vs = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Microsoft Visual Studio'
    candidates = sorted(vs.glob('*/Community/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe'))
    if not candidates:
        raise SystemExit('Install Clang, GCC, or Visual Studio C++ tools to run native firmware tests.')
    compiler = candidates[-1]
    toolchain = compiler.parents[3]
    kits = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Windows Kits/10'
    sdk = sorted((kits / 'Include').iterdir())[-1].name
    env = dict(os.environ)
    env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
    for source, defines in sources:
        executable = BUILD / (Path(source).stem + ('-esp32' if defines else '') + '.exe')
        subprocess.run([str(compiler), '/nologo', '/EHsc', '/std:c++14', '/W3',
                        *['/D' + define for define in defines],
                        f'/I{toolchain / "include"}', f'/I{ROOT / "tests/fakes"}', f'/I{BUILD}',
                        f'/I{ROOT / "firmware/esp12e_commandware"}',
                        *(f'/I{kits / "Include" / sdk / part}' for part in ('ucrt', 'shared', 'um')),
                        str(ROOT / 'tests' / source), f'/Fe:{executable}',
                        f'/Fo:{BUILD / (Path(source).stem + ".obj")}', '/link',
                        f'/LIBPATH:{toolchain / "lib/x64"}',
                        *(f'/LIBPATH:{kits / "Lib" / sdk / part / "x64"}' for part in ('ucrt', 'um'))],
                       env=env, cwd=BUILD, check=True)
        subprocess.run([str(executable), str(ROOT / 'docs/examples/pong.cw')], check=True)
