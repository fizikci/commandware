from __future__ import annotations

import shlex
import re
import heapq
from math import ceil, exp, isfinite, log
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
class ScheduleEntry:
    start_ms: int
    pipeline: str


def _milliseconds(token: str, allow_zero: bool = False) -> int:
    if not re.fullmatch(r"[0-9]+", token) or not (0 if allow_zero else 1) <= int(token) <= 2147483647:
        raise ValueError("milliseconds must be an integer in the supported range")
    return int(token)


def parse_schedule(text: str) -> list[ScheduleEntry]:
    match = re.fullmatch(r"\s*schedule\s*\{(.*)\}\s*", text, re.I | re.S)
    if not match:
        raise ValueError("schedule requires { START_MS { COMMAND } ... }")
    body = match[1]
    entries = []
    while body.strip():
        entry = re.match(r'\s*([0-9]+)\s*\{((?:"(?:\\.|[^"\\])*"|[^{}"])+)\}', body, re.S)
        if not entry or not entry[2].strip():
            raise ValueError("invalid schedule entry")
        entries.append(ScheduleEntry(_milliseconds(entry[1], True), entry[2].strip()))
        body = body[entry.end():]
    if not 1 <= len(entries) <= 32:
        raise ValueError("schedule requires 1..32 entries")
    return entries


def _clock_options(args: list[str]) -> tuple[float, int | None, int | None]:
    if not args:
        raise ValueError("clock requires a frequency")
    frequency = float(args[0])
    if not isfinite(frequency) or not 0.01 <= frequency <= 1000000:
        raise ValueError("clock frequency must be between 0.01 and 1000000 Hz")
    duration = pin = None
    remaining = iter(args[1:])
    for token in remaining:
        if token == "--dur" and duration is None:
            duration = _milliseconds(next(remaining, ""))
        elif pin is None and token != "--dur":
            pin = _milliseconds(token, True)
            if pin > 16:
                raise ValueError("invalid clock pin")
        else:
            raise ValueError("invalid clock argument")
    return frequency, duration, pin


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
    lcd_lines: list[str] = field(default_factory=lambda: [" " * 16, " " * 16])
    lcd_history: list[list[str]] = field(default_factory=list)
    lcd_address: int = 0x27

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


def _shell_tokens(text: str) -> list[str]:
    # Decode row breaks before shlex removes double-quote/backslash escapes,
    # so a literal escaped backslash followed by n stays literal.
    text = re.sub(r'"(?:\\.|[^"\\])*"',
                  lambda quoted: re.sub(r'\\[\\"n]',
                                        lambda escape: "\n" if escape[0] == r"\n" else escape[0],
                                        quoted[0]), text)
    return shlex.split(text)


def parse_pipeline(line: str) -> list[Command]:
    if not line or not line.strip():
        return []
    commands: list[Command] = []
    start = 0
    quote = None
    escaped = False
    parts = []
    for index, char in enumerate(line):
        if escaped:
            escaped = False
        elif char == "\\" and quote != "'":
            escaped = True
        elif quote:
            if char == quote:
                quote = None
        elif char in {'"', "'"}:
            quote = char
        elif char == "|":
            parts.append(line[start:index])
            start = index + 1
    parts.append(line[start:])
    for part in parts:
        tokens = _shell_tokens(part)
        if not tokens:
            raise ValueError("empty pipeline stage")
        commands.append(Command(tokens[0].lower(), tokens[1:]))
    return commands


def parse_command_text(line: str) -> tuple[str, list[str]]:
    tokens = _shell_tokens(line)
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
    if not args or args[0].upper() != "A0" or args[1:] not in ([], ["--watch"]):
        raise ValueError("adc requires A0 [--watch]")
    pin = str(args[0]).upper()
    if values:
        readings = [_as_int(value) for value in values]
        return _edge(readings) if args[1:] else readings
    return [hardware.read_analog(pin)]


# Measured (A0 counts, node millivolts, resistance in ohms), for the
# 20160-ohm reference / 3357-ohm shunt circuit.
OHM_CALIBRATION = (
    (50, 60, 270), (179, 181, 995), (291, 288, 1992),
    (459, 449, 5152), (557, 543, 9959), (628, 612, 20020),
    (703, 682, 108800), (720, 697, 688900),
)
OHM_MIN_RAW = 30
# Extend the first measured segment down to the requested ADC limit.
OHM_MIN_VOLTAGE_MV = OHM_CALIBRATION[0][1] + (
    (OHM_MIN_RAW - OHM_CALIBRATION[0][0])
    * (OHM_CALIBRATION[1][1] - OHM_CALIBRATION[0][1])
    / (OHM_CALIBRATION[1][0] - OHM_CALIBRATION[0][0])
)


def resistance_from_voltage(v_mv: float) -> int | str:
    """Measured piecewise exponential R(v); numeric results are whole ohms."""
    if not isfinite(v_mv) or v_mv < 0:
        return "OUT_OF_RANGE"
    if v_mv < OHM_MIN_VOLTAGE_MV:
        return "<270"
    if v_mv > OHM_CALIBRATION[-1][1]:
        return ">700k"
    for (_, low_mv, low_r), (_, high_mv, high_r) in zip(OHM_CALIBRATION, OHM_CALIBRATION[1:]):
        if v_mv <= high_mv:
            if v_mv == low_mv:
                return low_r
            if v_mv == high_mv:
                return high_r
            fraction = (v_mv - low_mv) / (high_mv - low_mv)
            return int(low_r * exp(fraction * log(high_r / low_r)) + 0.5)
    raise AssertionError("Voltage not covered by calibration")


def _ohm_reading(raw: int) -> int | str:
    if not 0 <= raw <= 1023:
        return "OUT_OF_RANGE"
    if raw < OHM_MIN_RAW:
        return "<270"
    if raw > OHM_CALIBRATION[-1][0]:
        return ">700k"
    for (low, low_mv, _), (high, high_mv, _) in zip(OHM_CALIBRATION, OHM_CALIBRATION[1:]):
        if raw <= high:
            voltage_mv = low_mv + (raw - low) * (high_mv - low_mv) / (high - low)
            return resistance_from_voltage(voltage_mv)
    raise AssertionError("ADC reading not covered by calibration")


def _ohm(args: list[str], hardware: HardwareState, values: Sequence[object]) -> list[object]:
    if args and (len(args) != 1 or args[0].upper() != "A0"):
        raise ValueError("ohm accepts only A0")
    readings = values if values else [hardware.read_analog("A0")]
    return [_ohm_reading(_as_int(value)) for value in readings]


def _clock(args: list[str], values: Sequence[object] | None = None, count: int | None = None) -> list[int]:
    frequency, duration, _ = _clock_options(args)
    if duration is not None:
        half_period_us = max(1, int(500000 / frequency + 0.5))
        total = max(0, ceil(duration * 1000 / half_period_us) - 1)
        if count is not None:
            total = min(total, max(0, count))
        return [(index + 1) % 2 for index in range(total)]
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


def _lcd(args: list[str], hardware: HardwareState, values: Sequence[object]) -> list[object]:
    if not args:
        raise ValueError('use lcd init [ADDRESS], lcd print "TEXT", or lcd clear')
    action = args[0].lower()
    if action == "init":
        if len(args) > 2:
            raise ValueError("lcd init accepts an I2C address")
        address = int(args[1], 0) if len(args) == 2 else 0x27
        if not 0x08 <= address <= 0x77:
            raise ValueError("invalid LCD I2C address")
        hardware.lcd_address = address
        hardware.lcd_lines = [" " * 16, " " * 16]
        return []
    if action == "clear" and len(args) == 1:
        texts = [""]
    elif action == "print" and len(args) <= 2:
        if len(args) == 2 and values:
            raise ValueError("lcd print takes text or piped input")
        texts = [args[1]] if len(args) == 2 else [str(value) for value in values]
        if not texts:
            raise ValueError("lcd print requires text or piped input")
    else:
        raise ValueError("invalid LCD command")
    for text in texts:
        cells = [" "] * 32
        pos = 0
        for char in text:
            if pos >= 32:
                break
            if char == "\n":
                pos = 16 if pos < 16 else 32
            else:
                cells[pos] = char
                pos += 1
        hardware.lcd_lines = ["".join(cells[:16]), "".join(cells[16:])]
        hardware.lcd_history.append(hardware.lcd_lines.copy())
    return []


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
        "help", "pins", "jobs", "kill", "status", "reset", "run", "schedule",
        "read", "write", "adc", "ohm", "clock", "map", "not", "edge",
        "rising", "falling", "debounce", "toggle", "divide", "pulse",
        "print", "lcd", "shiftin", "shiftout"
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
    "ohm": lambda args, values, hardware, jobs=None, job_id=None, count=None: _ohm(args, hardware, values),
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
    "lcd": lambda args, values, hardware, jobs=None, job_id=None, count=None: _lcd(args, hardware, values),
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
    return handler(command.args, values, hardware=hardware, jobs=jobs, job_id=job_id, count=count)


def _simulate_schedule(entries: list[ScheduleEntry], hardware: HardwareState, jobs: JobManager | None, count: int | None) -> list[object]:
    """Run concurrent jobs on a virtual microsecond timeline, without sleeping.

    Unlimited clocks retain the simulator's bounded sampling behavior (one
    sample by default); firmware clocks actually run until killed.
    """
    prepared = []
    events = []
    output = []
    allowed = set(COMMANDS) - {"run", "reset", "kill", "jobs"}
    for index, entry in enumerate(entries):
        commands = parse_pipeline(entry.pipeline)
        if not commands or any(c.name not in allowed for c in commands):
            raise ValueError("unsupported scheduled command")
        frequency = duration = pin = None
        pins = set()
        if commands[0].name == "clock":
            frequency, duration, pin = _clock_options(commands[0].args)
            if duration is not None and entry.start_ms + duration > 2147483647:
                raise ValueError("schedule deadline exceeds supported range")
            if any(c.name not in {"map", "ohm", "not", "edge", "rising", "falling", "debounce", "toggle", "divide", "pulse", "write", "print", "lcd"} for c in commands[1:]):
                raise ValueError("unsupported clock pipeline stage")
            if pin is not None and not any(c.name in {"write", "print", "lcd"} for c in commands[1:]):
                commands.append(Command("write", [str(pin)]))
            if len(commands) > 9:
                raise ValueError("pipeline too long")
            pins = {int(parse_pin(c.args[0])) for c in commands[1:] if c.name == "write"}
            for previous in prepared:
                old, _, old_duration, old_pins, *_ = previous
                if pins & old_pins and entry.start_ms < (old.start_ms + old_duration if old_duration else float("inf")) and old.start_ms < (entry.start_ms + duration if duration else float("inf")):
                    raise ValueError("overlapping clock writes to the same pin")
            for gpio in pins:
                if gpio in hardware.pin_owners:
                    raise ValueError(f"GPIO{gpio} is owned by job {hardware.pin_owners[gpio]}")
        interval = max(1, int(500000 / frequency + 0.5)) if frequency else None
        prepared.append((entry, commands, duration, pins, interval, [{} for _ in commands]))
        heapq.heappush(events, (entry.start_ms * 1000, 1, index, 0))
        if duration is not None:
            heapq.heappush(events, ((entry.start_ms + duration) * 1000, 0, index, 0))
    clocks = [p for p in prepared if p[4] is not None]
    for entry, *_ in clocks:
        if sum(other.start_ms <= entry.start_ms and (duration is None or entry.start_ms < other.start_ms + duration)
               for other, _, duration, *_ in clocks) > 8:
            raise ValueError("schedule exceeds 8 concurrent clocks")
    owners = {}
    while events:
        time_us, kind, index, tick = heapq.heappop(events)
        entry, commands, duration, pins, interval, states = prepared[index]
        if kind == 0:
            for pin in pins:
                hardware.set_gpio(pin, 0)
                hardware.release_pin(pin, owners[index])
            if jobs is not None and index in owners:
                jobs.kill(owners[index])
            continue
        if kind == 1:
            if interval is None:
                output.extend(run_stream(entry.pipeline, hardware=hardware, jobs=jobs, count=count))
                continue
            owner = jobs.create(entry.pipeline, pins).id if jobs is not None else index + 1
            owners[index] = owner
            for pin in pins:
                hardware.claim_pin(pin, owner)
                hardware.set_gpio(pin, 0)
        else:
            stream = [tick % 2]
            for stage_index, stage in enumerate(commands[1:], 1):
                state = states[stage_index]
                if not stream:
                    break
                value = stream[0]
                previous = state.get("previous")
                state["previous"] = value
                state["count"] = state.get("count", 0) + 1
                if stage.name in {"edge", "rising", "falling"}:
                    emit = (previous is None or value != previous) if stage.name == "edge" else previous is not None and (value > previous if stage.name == "rising" else value < previous)
                    stream = [value] if emit else []
                elif stage.name == "toggle":
                    state["toggle"] = 1 - state.get("toggle", 0)
                    stream = [state["toggle"]]
                elif stage.name == "divide":
                    divisor = max(1, int(stage.args[0])) if stage.args else 2
                    stream = stream if state["count"] % divisor == 0 else []
                elif stage.name == "debounce":
                    state["streak"] = state.get("streak", 0) + 1 if previous == value else 1
                    threshold = max(1, int(stage.args[0])) if stage.args else 1
                    stream = stream if state["streak"] >= threshold else []
                elif stage.name == "pulse":
                    length = max(1, int(stage.args[0])) if stage.args else 1
                    stream = [1] * length if value == 1 and previous in {None, 0} else []
                else:
                    stream = _dispatch(stage, stream, hardware, jobs, owners[index])
            output.extend(stream)
        next_tick = tick + 1
        next_time = entry.start_ms * 1000 + next_tick * interval
        limit = count if count is not None else (1 if duration is None else None)
        if (limit is None or next_tick <= limit) and (duration is None or next_time < (entry.start_ms + duration) * 1000):
            heapq.heappush(events, (next_time, 2, index, next_tick))
    return output


def run_stream(command: str, values: Sequence[object] | None = None, hardware: HardwareState | None = None, jobs: JobManager | None = None, count: int | None = None, job_id: int | None = None) -> list[object]:
    hardware = hardware or HardwareState()
    if not command or not command.strip():
        return []
    if re.match(r"\s*schedule\b", command, re.I):
        return _simulate_schedule(parse_schedule(command), hardware, jobs, count)
    commands = parse_pipeline(command)
    if commands and commands[0].name == "clock" and _clock_options(commands[0].args)[1] is not None:
        return _simulate_schedule([ScheduleEntry(0, command)], hardware, jobs, count)
    if "|" in command:
        pipeline = Pipeline(parse_pipeline(command))
        return pipeline.execute(values, hardware=hardware, jobs=jobs, job_id=job_id)

    name, args = parse_command_text(command)
    stream = _listify(values)
    handler = COMMANDS.get(name)
    if handler is None:
        raise ValueError(f"Unsupported command: {name}")
    return handler(args, stream, hardware=hardware, jobs=jobs, job_id=job_id, count=count)
