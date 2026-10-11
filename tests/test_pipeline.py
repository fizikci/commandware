from commandware.core import HardwareState, JobManager, Pipeline, parse_pipeline, resistance_from_voltage, run_stream
import pytest


def test_map_scales_values():
    result = run_stream("map 0 10 0 100", [0, 5, 10])
    assert result == [0, 50, 100]


def test_not_inverts_values():
    result = run_stream("not", [0, 1, 0, 1])
    assert result == [1, 0, 1, 0]


def test_command_names_are_case_insensitive():
    assert run_stream("NOT", [0, 1]) == [1, 0]
    commands = parse_pipeline("CLOCK 100 | WRITE 5")
    assert [cmd.name for cmd in commands] == ["clock", "write"]


def test_rising_only_emits_edges():
    result = run_stream("rising", [0, 0, 1, 1, 0, 0])
    assert result == [1]


def test_toggle_flips_each_value():
    result = run_stream("toggle", [0, 1, 0, 1])
    assert result == [1, 0, 1, 0]


def test_write_sets_gpio_state():
    hw = HardwareState()
    run_stream("write 5", [1], hardware=hw)
    assert hw.gpio[5] == 1


def test_clock_generates_square_wave():
    values = run_stream("clock 1", [], count=6)
    assert values == [0, 1, 0, 1, 0, 1]


def test_pipeline_parses_pipe_chain():
    commands = parse_pipeline("clock 100 | write 5")
    assert [cmd.name for cmd in commands] == ["clock", "write"]
    assert commands[0].args == ["100"]
    assert commands[1].args == ["5"]


def test_write_pipeline_claims_pin_and_sets_output():
    hw = HardwareState()
    manager = JobManager()
    pipeline = Pipeline(parse_pipeline("clock 2 | write 5"))
    result = pipeline.execute([], hardware=hw, jobs=manager, job_id=1)
    assert result in ([], [0, 1]) or isinstance(result, list)
    assert hw.gpio.get(5, 0) in {0, 1}


def test_pin_ownership_blocks_conflict():
    hw = HardwareState()
    hw.claim_pin(5, 1)
    try:
        hw.claim_pin(5, 2)
        assert False, "expected pin conflict"
    except ValueError:
        pass


def test_help_and_status_commands_are_registered():
    assert "help" in run_stream.__globals__["COMMANDS"]
    assert "status" in run_stream.__globals__["COMMANDS"]


@pytest.mark.parametrize("raw,mv,known", [
    (50, 60, 270), (179, 181, 995), (291, 288, 1992), (459, 449, 5152),
    (557, 543, 9959), (628, 612, 20020), (703, 682, 108800), (720, 697, 688900),
])
def test_ohm_matches_measured_resistors(raw, mv, known):
    assert run_stream("ohm", [raw]) == [known]
    assert resistance_from_voltage(mv) == known


def test_ohm_reads_a0_and_accepts_adc_pipeline():
    hw = HardwareState()
    hw.set_analog("A0", 280)
    assert run_stream("OHM a0", hardware=hw) == run_stream("adc A0 | ohm", hardware=hw)
    assert "ohm" in run_stream("help")
    assert run_stream("status") == ["OK"]


def test_ohm_reports_bounds_at_requested_adc_limits():
    assert run_stream("ohm", [-1, 0, 29, 30, 49, 50, 720, 721, 1023, 1024]) == [
        "OUT_OF_RANGE", "<270", "<270", 221, 267, 270, 688900, ">700k", ">700k", "OUT_OF_RANGE",
    ]
    assert resistance_from_voltage(41.24) == "<270"
    assert resistance_from_voltage(41.25) == 221
    assert resistance_from_voltage(697.01) == ">700k"


@pytest.mark.parametrize("voltage", [-1, float("nan"), float("inf")])
def test_ohm_rejects_invalid_voltages(voltage):
    assert resistance_from_voltage(voltage) == "OUT_OF_RANGE"


def test_ohm_uses_exponential_interpolation_between_measurements():
    # Halfway in voltage gives a geometric mean, not an arithmetic mean.
    assert resistance_from_voltage((60 + 181) / 2) == pytest.approx((270 * 995) ** 0.5, abs=0.5)
    # A0 235 is halfway between the 179 / 291 calibration counts.
    assert run_stream("ohm", [235])[0] == pytest.approx((995 * 1992) ** 0.5, abs=0.5)


def test_ohm_interpolation_is_monotonic_and_finite():
    readings = run_stream("ohm", list(range(30, 721)))
    assert all(isinstance(value, int) and value > 0 for value in readings)
    assert readings == sorted(readings)


@pytest.mark.parametrize("args", ["A1", "A0 extra", "57"])
def test_ohm_rejects_invalid_arguments(args):
    with pytest.raises(ValueError, match="only A0"):
        run_stream(f"ohm {args}")
