import pytest

from commandware.core import HardwareState, run_stream
from commandware.script import ScriptRuntime
from commandware.oled_font import FONT


@pytest.mark.parametrize('controller,height,address', [('ssd1306',64,0x3c), ('SH1106',64,0x3d), ('ssd1306',32,0x3c), ('sh1106',32,0x3c)])
def test_init_controller_dimensions_and_address(controller, height, address):
    hw = HardwareState()
    run_stream(f'oled init {controller} --height {height} --address {address}', hardware=hw)
    assert (hw.oled.controller, hw.oled.height, hw.oled.address) == (controller.lower(), height, address)
    assert hw.oled.ready and hw.oled.reserved and not any(hw.oled.buffer)


@pytest.mark.parametrize('args,x,y', [('"hello"',0,0), ('2 2 "hello"',2,2), ('"hello" --x 2 --y 2',2,2), ('--y 2 --x 2 "hello"',2,2)])
def test_text_position_and_pixels(args, x, y):
    hw = HardwareState()
    run_stream('oled print ' + args, hardware=hw)
    assert hw.oled.text_history == [(x,y,'hello')]
    for col in range(5):
        for row in range(8):
            assert hw.oled.pixel(x+col,y+row) == bool(FONT[(ord('h')-32)*5+col] & (1 << row))


def test_pipe_value_explicit_override_and_event_preservation():
    hw = HardwareState(analog={'A0':512})
    assert run_stream('adc A0 --out raw | oled print 2 2 | print $raw', hardware=hw) == [512]
    assert hw.oled.text_history[-1] == (2,2,'512')
    run_stream('adc A0 | oled print "override" --x 8 --y 16', hardware=hw)
    assert hw.oled.text_history[-1] == (8,16,'override')
    run_stream('oled print 0; oled print ""', hardware=hw)
    assert hw.oled.text_history[-2:] == [(0,0,'0'),(0,0,'')]


def test_chart_scroll_range_clipping_and_clear():
    hw = HardwareState()
    run_stream('oled chart 0 --min 0 --max 10 --width 4 --height 4', hardware=hw)
    assert hw.oled.pixel(3,3)
    run_stream('oled chart 10 --min 0 --max 10 --width 4 --height 4', hardware=hw)
    assert hw.oled.pixel(2,3) and all(hw.oled.pixel(3,y) for y in range(4))
    run_stream('oled chart 100 --min 0 --max 10 --width 4 --height 4', hardware=hw)
    assert all(hw.oled.pixel(2,y) for y in range(4)) and hw.oled.pixel(3,0) and not hw.oled.pixel(3,3)
    run_stream('oled chart -10 --min 0 --max 10 --x 10 --y 10 --width 4 --height 4', hardware=hw)
    assert hw.oled.pixel(13,13)
    run_stream('oled clear', hardware=hw)
    assert not any(hw.oled.buffer) and hw.oled.charts == {}


def test_two_chart_areas_and_script_variables():
    hw = HardwareState()
    run_stream('''do {
        oled init sh1106
        var temp 23
        var humidity 48
        every 100 |
            oled chart $temp --min 0 --max 50 --height 32 |
            oled chart $humidity --min 0 --max 100 --y 32 --height 32
    }''', hardware=hw, count=3)
    assert len(hw.oled.charts) == 2 and len(hw.oled.chart_history) == 6
    assert [value for _,value in hw.oled.chart_history] == [23,48]*3


def test_live_watch_and_scheduled_oled():
    hw = HardwareState()
    assert run_stream('adc A0 --watch | oled print', [512,512,7], hardware=hw) == []
    assert hw.oled.text_history == [(0,0,'512'),(0,0,'7')]
    run_stream('schedule { 10 { oled print 2 2 "scheduled" } }', hardware=hw)
    assert hw.oled.text_history[-1] == (2,2,'scheduled')


@pytest.mark.parametrize('command', [
    'oled init unknown', 'oled init ssd1306 sh1106', 'oled init --address 0',
    'oled init --width 64', 'oled init --height 48', 'oled print', 'oled chart',
    'oled print "hi" --x -1', 'oled print 2 2 "hi" --x 2', 'oled print "hi" --y 64',
    'oled chart bad', 'oled chart 1 --min 5 --max 5', 'oled chart 1 --max nan',
    'oled chart 1 --width 1', 'oled chart 1 --x 120 --width 20',
    'oled clear extra', 'oled clear --x 0', 'oled print "hi" --x',
    'oled print "hi" --x 0 --x 1', 'oled print "hi" --value "there"',
])
def test_errors_have_line_and_do_not_initialize_hardware(command):
    hw = HardwareState()
    with pytest.raises(ValueError, match=r'at line 3$'):
        run_stream('do {\n # comment\n ' + command + '\n}', hardware=hw)
    assert not hw.oled.ready and not hw.oled.reserved and not any(hw.oled.buffer)


def test_chart_limits_overlap_and_range_change():
    hw = HardwareState()
    for x in range(0,40,10):
        run_stream(f'oled chart 10 --x {x} --width 8 --height 8 --max 10', hardware=hw)
    with pytest.raises(ValueError, match='limit is 4'):
        run_stream('oled chart 1 --x 40 --width 8 --height 8', hardware=hw)
    with pytest.raises(ValueError, match='must not overlap'):
        run_stream('oled chart 1 --x 1 --width 8 --height 8', hardware=hw)
    run_stream('oled chart 0 --width 8 --height 8 --max 20', hardware=hw)
    assert hw.oled.pixel(7,7) and not hw.oled.pixel(6,0)


def test_oled_pin_reservation_and_existing_owner():
    hw = HardwareState()
    runtime = ScriptRuntime(hw)
    runtime.submit('every 10 | write 4 1')
    runtime.tick(0)
    with pytest.raises(ValueError, match='owned by a job'):
        run_stream('oled init', hardware=hw)
    runtime.reset()
    run_stream('oled init', hardware=hw)
    for command in ('write 4 1', 'read 5', 'do { write 4 1 }', 'shiftout 4 12 13 msb'):
        with pytest.raises(ValueError, match='reserved'):
            run_stream(command, hardware=hw)
    assert 'OLED I2C' in '\n'.join(run_stream('pins', hardware=hw))
    run_stream('var led 12', hardware=hw)
    run_stream('write $led 1', hardware=hw)
    assert hw.script_writes[-2:] == [(runtime.now,12,1), (runtime.now,12,0)]


def test_display_contents_and_reservation_survive_reset():
    hw = HardwareState()
    run_stream('oled print "retained"', hardware=hw)
    before = bytes(hw.oled.buffer)
    run_stream('reset', hardware=hw)
    assert bytes(hw.oled.buffer) == before and hw.oled.reserved


def test_lcd_and_oled_can_share_bus_with_distinct_addresses():
    hw = HardwareState()
    run_stream('oled init; lcd init; oled print "OLED"; lcd print "LCD"', hardware=hw)
    assert hw.oled.ready and hw.lcd_ready
    with pytest.raises(ValueError, match='already used by OLED'):
        run_stream('lcd init 0x3c', hardware=hw)


def test_documented_oled_example_runs_unchanged():
    from pathlib import Path
    hw = HardwareState(analog={'A0':512})
    source = (Path(__file__).parents[1] / 'docs/examples/oled-charts.cw').read_text()
    assert run_stream(source, hardware=hw, count=3) == []
    assert hw.oled.text_history == [(0,0,'Raw A0'), (0,32,'Percent A0')]
    assert [reading for _,reading in hw.oled.chart_history] == [512,50]*3
