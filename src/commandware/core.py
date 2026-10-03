from __future__ import annotations

import shlex
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Sequence


def parse_pin(pin: object) -> int | str:
    if pin is None:
        raise ValueError("Pin required")
    if isinstance(pin, str):
        value = pin.strip().upper()
        if value.startswith("GPIO"):
            return int(value[4:])
        if value in {"A0", "A1", "A2", "A3"}:
            return value
        return int(value)
    return int(pin)


def _as_int(value: object) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if isinstance(value, str):
        text = value.strip().lower()
        if text in {"true", "on", "high", "1"}:
            return 1
        if text in {"false", "off", "low", "0"}:
            return 0
        return int(float(text))
    raise TypeError(f"Unsupported value: {value!r}")


def _scale(value: object, in_min: int, in_max: int, out_min: int, out_max: int) -> int:
    current = _as_int(value)
    if in_max == in_min:
        return out_min
    ratio = (current - in_min) / (in_max - in_min)
    return int(round(out_min + ratio * (out_max - out_min)))


@dataclass
class Command:
    name: str
    args: list[str] = field(default_factory=list)


@dataclass
class Job:
    id: int
    pipeline: str
    pins: set[int] = field(default_factory=set)
    active: bool = True


class JobManager:
    def __init__(self):
        self.jobs: dict[int, Job] = {}
        self._next_id = 1

    def create(self, pipeline: str, pins: Iterable[int] | None = None) -> Job:
        job = Job(id=self._next_id, pipeline=pipeline, pins=set(pins or []), active=True)
        self.jobs[job.id] = job
        self._next_id += 1
        return job

    def kill(self, job_id: int) -> bool:
        job = self.jobs.pop(job_id, None)
        return job is not None

    def list_jobs(self) -> list[str]:
        result: list[str] = []
        for job_id, job in sorted(self.jobs.items()):
            pins = ",".join(str(pin) for pin in sorted(job.pins)) if job.pins else "-"
            result.append(f"{job_id} RUNNING  {job.pipeline} [{pins}]")
        return result


@dataclass
class HardwareState:
    gpio: dict[int, int] = field(default_factory=dict)
    analog: dict[str, int] = field(default_factory=dict)
    pin_owners: dict[int, int] = field(default_factory=dict)

    def claim_pin(self, pin: object, job_id: int) -> None:
        pin_number = int(parse_pin(pin))
        owner = self.pin_owners.get(pin_number)
        if owner is not None and owner != job_id:
            raise ValueError(f"GPIO{pin_number} is owned by job {owner}")
        self.pin_owners[pin_number] = job_id

    def release_pin(self, pin: object, job_id: int) -> None:
        pin_number = int(parse_pin(pin))
        if self.pin_owners.get(pin_number) == job_id:
            del self.pin_owners[pin_number]

    def set_gpio(self, pin: object, value: object) -> int:
        int_pin = int(parse_pin(pin))
        int_value = _as_int(value)
        self.gpio[int_pin] = int_value
        return int_value

    def read_gpio(self, pin: object) -> int:
        return self.gpio.get(int(parse_pin(pin)), 0)

    def set_analog(self, pin: object, value: object) -> int:
        name = str(pin).upper()
        current = max(0, min(1023, _as_int(value)))
        self.analog[name] = current
        return current

    def read_analog(self, pin: object) -> int:
        name = str(pin).upper()
        return self.analog.get(name, 512)

    def pins_snapshot(self) -> list[str]:
        rows = []
        definitions = [
            (0, "GPIO0"), (1, "GPIO1"), (2, "GPIO2"), (3, "GPIO3"),
            (4, "GPIO4"), (5, "GPIO5"), (12, "GPIO12"), (13, "GPIO13"),
            (14, "GPIO14"), (15, "GPIO15"), (16, "GPIO16"), ("A0", "A0")
        ]
        for pin, label in definitions:
            if isinstance(pin, str):
                available = "available"
                rows.append(f"{label:<8} {available}")
            else:
                status = "reserved" if pin in self.pin_owners else "available"
                rows.append(f"GPIO{pin:<2}  GPIO       {status}")
        return rows


class Pipeline:
    def __init__(self, commands: Sequence[Command]):
        self.commands = list(commands)

    def execute(self, values: Sequence[object] | None = None, hardware: HardwareState | None = None, jobs: JobManager | None = None, job_id: int | None = None) -> list[object]:
        hardware = hardware or HardwareState()
        stream = list(values or [])
        for command in self.commands:
            stream = _dispatch(command, stream, hardware, jobs, job_id)
        return stream


def parse_pipeline(line: str) -> list[Command]:
    if not line or not line.strip():
        return []
    commands: list[Command] = []
    for part in line.split("|"):
        command_text = part.strip()
        if not command_text:
            continue
        tokens = shlex.split(command_text)
        if not tokens:
            continue
        commands.append(Command(name=tokens[0].lower(), args=tokens[1:]))
    return commands


def parse_command_text(line: str) -> tuple[str, list[str]]:
    tokens = shlex.split(line)
    if not tokens:
        raise ValueError("Empty command")
    return tokens[0].lower(), tokens[1:]


def _listify(values: Sequence[object] | None) -> list[object]:
    return list(values or [])


def _read_pin(args: list[str], hardware: HardwareState, values: Sequence[object] | None = None) -> list[int]:
    if not args:
        raise ValueError("read requires a GPIO pin")
    pin = parse_pin(args[0])
    if values:
        return [_as_int(value) for value in values]
    return [hardware.read_gpio(pin)]


def _adc(args: list[str], hardware: HardwareState, values: Sequence[object] | None = None) -> list[int]:
    if not args:
        raise ValueError("adc requires an analog pin")
    pin = str(args[0]).upper()
    if values:
        return [_as_int(value) for value in values]
    return [hardware.read_analog(pin)]


def _clock(args: list[str], values: Sequence[object] | None = None, count: int | None = None) -> list[int]:
    total = count if count is not None else (len(values) if values is not None else 1)
    if total is None or total <= 0:
        total = 1
    result: list[int] = []
    for _ in range(total):
        result.append(len(result) % 2)
    return result


def _map(args: list[str], values: Sequence[object]) -> list[int]:
    if len(args) != 4:
        raise ValueError("map requires 4 arguments")
    in_min, in_max, out_min, out_max = (_as_int(part) for part in args)
    return [_scale(value, in_min, in_max, out_min, out_max) for value in values]


def _not(values: Sequence[object]) -> list[int]:
    return [1 - _as_int(value) for value in values]


def _edge(values: Sequence[object]) -> list[int]:
    result: list[int] = []
    previous: int | None = None
    for value in values:
        current = _as_int(value)
        if previous is None or current != previous:
            result.append(current)
        previous = current
    return result


def _rising(values: Sequence[object]) -> list[int]:
    result: list[int] = []
    previous: int | None = None
    for value in values:
        current = _as_int(value)
        if previous is not None and current > previous:
            result.append(current)
        previous = current
    return result


def _falling(values: Sequence[object]) -> list[int]:
    result: list[int] = []
    previous: int | None = None
    for value in values:
        current = _as_int(value)
        if previous is not None and current < previous:
            result.append(current)
        previous = current
    return result


def _debounce(args: list[str], values: Sequence[object]) -> list[int]:
    threshold = max(1, int(args[0])) if args else 1
    result: list[int] = []
    previous: int | None = None
    streak = 0
    for value in values:
        current = _as_int(value)
        if previous == current:
            streak += 1
        else:
            previous = current
            streak = 1
        if streak >= threshold:
            result.append(current)
    return result


def _toggle(values: Sequence[object]) -> list[int]:
    state = 0
    result: list[int] = []
    for _ in values:
        state = 1 - state
        result.append(state)
    return result


def _divide(args: list[str], values: Sequence[object]) -> list[int]:
    divisor = max(1, int(args[0])) if args else 2
    result: list[int] = []
    for index, value in enumerate(values, start=1):
        if index % divisor == 0:
            result.append(_as_int(value))
    return result


def _pulse(args: list[str], values: Sequence[object]) -> list[int]:
    duration = max(1, int(args[0])) if args else 1
    result: list[int] = []
    previous: int | None = None
    for value in values:
        current = _as_int(value)
        if previous is not None and previous == 0 and current == 1:
            result.extend([1] * duration)
        elif previous is None and current == 1:
            result.extend([1] * duration)
        previous = current
    return result


def _write(args: list[str], hardware: HardwareState, values: Sequence[object] | None = None, job_id: int | None = None) -> list[int]:
    if not args:
        raise ValueError("write requires a GPIO pin")
    pin = parse_pin(args[0])
    if len(args) >= 2:
        value = _as_int(args[1])
        if job_id is not None:
            hardware.claim_pin(pin, job_id)
        hardware.set_gpio(pin, value)
        return [value]
    if values is None:
        return []
    output: list[int] = []
    for value in values:
        int_value = _as_int(value)
        if job_id is not None:
            hardware.claim_pin(pin, job_id)
        hardware.set_gpio(pin, int_value)
        output.append(int_value)
    return output


def _print(values: Sequence[object]) -> list[object]:
    return list(values)


def _shiftin(args: list[str], hardware: HardwareState, values: Sequence[object] | None = None) -> list[int]:
    if len(args) < 4:
        raise ValueError("shiftin requires DATA CLOCK LOAD bit_order")
    bits = [_as_int(v) for v in values] if values else [hardware.read_gpio(parse_pin(args[0]))]
    integer = 0
    order = str(args[3]).lower()
    for index, bit in enumerate(bits):
        if order == "msb":
            integer = (integer << 1) | bit
        else:
            integer |= bit << index
    return [integer]


def _shiftout(args: list[str], hardware: HardwareState, values: Sequence[object] | None = None) -> list[int]:
    if len(args) < 3:
        raise ValueError("shiftout requires DATA CLOCK LATCH")
    value = _as_int(values[-1]) if values else 0
    hardware.set_gpio(parse_pin(args[0]), value & 1)
    return [value]


def _system_help(_args: list[str], _values: Sequence[object] | None = None, **_: object) -> list[str]:
    return [
        "help", "pins", "jobs", "kill", "status", "reset", "run",
        "read", "write", "adc", "clock", "map", "not", "edge",
        "rising", "falling", "debounce", "toggle", "divide", "pulse",
        "print", "shiftin", "shiftout"
    ]


def _system_pins(_args: list[str], _values: Sequence[object] | None = None, hardware: HardwareState | None = None, **_: object) -> list[str]:
    hw = hardware or HardwareState()
    return hw.pins_snapshot()


def _system_jobs(_args: list[str], _values: Sequence[object] | None = None, jobs: JobManager | None = None, **_: object) -> list[str]:
    if jobs is None:
        return []
    return jobs.list_jobs()


def _system_status(_args: list[str], _values: Sequence[object] | None = None, **_: object) -> list[str]:
    return ["OK"]


def _system_reset(_args: list[str], _values: Sequence[object] | None = None, hardware: HardwareState | None = None, **_: object) -> list[str]:
    hw = hardware or HardwareState()
    hw.gpio.clear()
    hw.analog.clear()
    hw.pin_owners.clear()
    return ["reset"]


def _system_kill(args: list[str], _values: Sequence[object] | None = None, jobs: JobManager | None = None, **_: object) -> list[str]:
    if not args:
        return ["ERROR: kill requires job id"]
    job_id = int(args[0])
    if jobs is None or not jobs.kill(job_id):
        return [f"ERROR: unknown job {job_id}"]
    return [f"killed {job_id}"]


def _system_run(args: list[str], _values: Sequence[object] | None = None, hardware: HardwareState | None = None, jobs: JobManager | None = None, **_: object) -> list[object]:
    if not args:
        return ["ERROR: run requires a script path"]
    path = Path(args[0])
    if not path.exists():
        return [f"ERROR: file not found: {args[0]}"]
    output: list[object] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        output.extend(run_stream(stripped, values=output, hardware=hardware, jobs=jobs))
    return output


COMMANDS: dict[str, Callable[..., list[object]]] = {
    "read": lambda args, values, hardware, jobs=None, job_id=None, count=None: _read_pin(args, hardware, values),
    "adc": lambda args, values, hardware, jobs=None, job_id=None, count=None: _adc(args, hardware, values),
    "clock": lambda args, values, hardware, jobs=None, job_id=None, count=None: _clock(args, values, count),
    "map": lambda args, values, hardware, jobs=None, job_id=None, count=None: _map(args, values),
    "not": lambda args, values, hardware, jobs=None, job_id=None, count=None: _not(values),
    "edge": lambda args, values, hardware, jobs=None, job_id=None, count=None: _edge(values),
    "rising": lambda args, values, hardware, jobs=None, job_id=None, count=None: _rising(values),
    "falling": lambda args, values, hardware, jobs=None, job_id=None, count=None: _falling(values),
    "debounce": lambda args, values, hardware, jobs=None, job_id=None, count=None: _debounce(args, values),
    "toggle": lambda args, values, hardware, jobs=None, job_id=None, count=None: _toggle(values),
    "divide": lambda args, values, hardware, jobs=None, job_id=None, count=None: _divide(args, values),
    "pulse": lambda args, values, hardware, jobs=None, job_id=None, count=None: _pulse(args, values),
    "write": lambda args, values, hardware, jobs=None, job_id=None, count=None: _write(args, hardware, values, job_id),
    "print": lambda args, values, hardware, jobs=None, job_id=None, count=None: _print(values),
    "shiftin": lambda args, values, hardware, jobs=None, job_id=None, count=None: _shiftin(args, hardware, values),
    "shiftout": lambda args, values, hardware, jobs=None, job_id=None, count=None: _shiftout(args, hardware, values),
    "help": _system_help,
    "pins": _system_pins,
    "jobs": _system_jobs,
    "kill": _system_kill,
    "status": _system_status,
    "reset": _system_reset,
    "run": _system_run,
}


def _dispatch(command: Command, values: Sequence[object], hardware: HardwareState, jobs: JobManager | None = None, job_id: int | None = None, count: int | None = None) -> list[object]:
    handler = COMMANDS.get(command.name)
    if handler is None:
        raise ValueError(f"Unsupported command: {command.name}")
    return handler(command.args, values, hardware, jobs=jobs, job_id=job_id, count=count)


def run_stream(command: str, values: Sequence[object] | None = None, hardware: HardwareState | None = None, jobs: JobManager | None = None, count: int | None = None, job_id: int | None = None) -> list[object]:
    hardware = hardware or HardwareState()
    if not command or not command.strip():
        return []
    if "|" in command:
        pipeline = Pipeline(parse_pipeline(command))
        return pipeline.execute(values, hardware=hardware, jobs=jobs, job_id=job_id)

    name, args = parse_command_text(command)
    stream = _listify(values)
    handler = COMMANDS.get(name)
    if handler is None:
        raise ValueError(f"Unsupported command: {name}")
    return handler(args, stream, hardware, jobs=jobs, job_id=job_id, count=count)
