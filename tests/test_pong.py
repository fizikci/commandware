from pathlib import Path

import pytest

from commandware.script import ScriptRuntime


SOURCE = (Path(__file__).resolve().parents[1] / 'docs/examples/pong.cw').read_text()


@pytest.fixture
def game():
    runtime = ScriptRuntime()
    runtime.repeat_count = 1000
    runtime.hardware.gpio.update({12: 1, 14: 1})
    runtime.submit(SOURCE)
    runtime.tick(0)
    runtime.tick(0)
    return runtime


def frame(game, **state):
    game.globals.update(state)
    game.tick(game.now + 50)
    assert not game.failures


def test_players_move_independently_and_clamp_during_serve(game):
    game.hardware.gpio.update({12: 0, 14: 1})
    frame(game, leftY=28, rightY=28, waiting=10)
    assert (game.globals['leftY'], game.globals['rightY']) == (26, 30)
    assert game.globals['ballX'] == 63
    game.hardware.gpio.update({12: 1, 14: 0})
    frame(game, leftY=50, rightY=10)
    assert (game.globals['leftY'], game.globals['rightY']) == (50, 10)


@pytest.mark.parametrize('state,expected', [
    (dict(ballX=6, ballY=30, velocityX=-2, velocityY=1), (4, 2)),
    (dict(ballX=120, ballY=30, velocityX=2, velocityY=1), (122, -2)),
])
def test_paddle_hits_reverse_ball(game, state, expected):
    frame(game, waiting=0, leftY=28, rightY=28, **state)
    assert (game.globals['ballX'], game.globals['velocityX']) == expected
    assert -2 <= game.globals['velocityY'] <= 2
    assert game.globals['leftScore'] == game.globals['rightScore'] == 0


@pytest.mark.parametrize('ball_y,velocity_y,expected', [(10, -2, (10, 2)), (62, 2, (62, -2))])
def test_top_and_bottom_bounce(game, ball_y, velocity_y, expected):
    frame(game, waiting=0, ballY=ball_y, velocityY=velocity_y)
    assert (game.globals['ballY'], game.globals['velocityY']) == expected


@pytest.mark.parametrize('x,vx,scorer', [(1, -2, 'rightScore'), (125, 2, 'leftScore')])
def test_misses_score_once_and_serve_without_stopping_players(game, x, vx, scorer):
    frame(game, waiting=0, ballX=x, ballY=55, velocityX=vx, velocityY=1, leftY=10, rightY=10)
    assert game.globals[scorer] == 1
    assert (game.globals['ballX'], game.globals['ballY'], game.globals['waiting']) == (63, 35, 16)
    assert game.hardware.oled.text_history[-1][2] in ('1:0', '0:1')
    frame(game)
    assert game.globals[scorer] == 1 and game.globals['waiting'] == 15
    assert game.globals['leftY'] == 14


def test_game_runs_many_frames_without_invalid_geometry_or_leaked_jobs():
    game = ScriptRuntime()
    game.submit(SOURCE)
    game.run(count=500)
    assert not game.failures
    assert game.globals['leftScore'] > 0
    assert game.globals['leftY'] == game.globals['rightY'] == 10
