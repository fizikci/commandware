from __future__ import annotations

import sys
import shlex
from typing import Sequence

from .core import HardwareState, run_stream


def _read_stdin_values() -> list[object]:
    values: list[object] = []
    for raw_line in sys.stdin:
        stripped = raw_line.strip()
        if not stripped:
            continue
        try:
            values.append(int(stripped))
        except ValueError:
            lowered = stripped.lower()
            if lowered in {"true", "false"}:
                values.append(int(lowered == "true"))
            else:
                values.append(stripped)
    return values


def _emit_values(values: Sequence[object]) -> None:
    from .script import display
    for value in values:
        print(display(value))


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args:
        print("Usage: commandware <command> [args]", file=sys.stderr)
        return 1

    # Preserve shell-quoted LCD text when rebuilding the command line.
    # A single argument may also contain a complete Commandware pipeline.
    command = args[0] if len(args) == 1 else shlex.join(args)
    values = _read_stdin_values()
    hardware = HardwareState()
    output = run_stream(command, values=values if values else None, hardware=hardware)
    _emit_values(output)
    return 0
