from __future__ import annotations

from math import exp, isfinite, log
from dataclasses import dataclass, field
from typing import Iterable, Sequence
from .oled import OledDisplay


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
    lcd_ready: bool = False
    oled: OledDisplay = field(default_factory=OledDisplay)
    tone_history: list[tuple[int, int, float, int]] = field(default_factory=list)
    script_writes: list[tuple[int, int, int]] = field(default_factory=list)

    def claim_pin(self, pin: object, job_id: int) -> None:
        pin_number = int(parse_pin(pin))
        if self.oled.reserved and pin_number in (4, 5):
            raise ValueError(f"GPIO{pin_number} is reserved for OLED I2C")
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
                status = "OLED I2C" if self.oled.reserved and pin in (4, 5) else "reserved" if pin in self.pin_owners else "available"
                rows.append(f"GPIO{pin:<2}  GPIO       {status}")
        return rows


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
        if hardware.oled.reserved and hardware.oled.address == address:
            raise ValueError("LCD address is already used by OLED")
        hardware.lcd_address = address
        hardware.lcd_ready = True
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
        hardware.lcd_ready = True
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


def _run_stream(command: str, values: Sequence[object] | None = None, hardware: HardwareState | None = None, jobs: JobManager | None = None, count: int | None = None, job_id: int | None = None) -> list[object]:
    """One grammar and runtime for interactive, piped, and scheduled work."""
    from .script import ScriptRuntime, Event
    hardware = hardware or HardwareState()
    if not command or not command.strip():
        return []
    runtime = getattr(hardware, "_script_runtime", None)
    if runtime is None:
        runtime = ScriptRuntime(hardware, jobs)
    runtime.submit(command, None if values is None else [Event(value, timestamp=runtime.now) for value in values])
    return runtime.run(count if count is not None else 1)


def run_stream(command: str, values: Sequence[object] | None = None, hardware: HardwareState | None = None, jobs: JobManager | None = None, count: int | None = None, job_id: int | None = None) -> list[object]:
    """Evaluate commands through the shared language runtime."""
    from .script import located
    try:
        result = _run_stream(command, values, hardware, jobs, count, job_id)
    except (ValueError, TypeError, OverflowError, OSError) as exc:
        raise ValueError(located(str(exc), 1)) from exc
    return [located(value, 1) if isinstance(value, str) and value.startswith("ERROR:") else value for value in result]
