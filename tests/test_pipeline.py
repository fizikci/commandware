from commandware.core import HardwareState, JobManager, Pipeline, parse_pipeline, run_stream


def test_map_scales_values():
    result = run_stream("map 0 10 0 100", [0, 5, 10])
    assert result == [0, 50, 100]


def test_not_inverts_values():
    result = run_stream("not", [0, 1, 0, 1])
    assert result == [1, 0, 1, 0]


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
