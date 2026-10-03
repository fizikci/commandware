from __future__ import annotations

import sys
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
    for value in values:
        print(value)


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args:
        print("Usage: commandware <command> [args]", file=sys.stderr)
        return 1

    command = " ".join(args)
    values = _read_stdin_values()
    hardware = HardwareState()
    output = run_stream(command, values=values, hardware=hardware)
    _emit_values(output)
    return 0
