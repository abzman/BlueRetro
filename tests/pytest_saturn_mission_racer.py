''' eaudunord's Saturn Mission Stick (GamePad Alt + memory card accessory) and Arcade Racer
(GamePad Alt + rumble accessory), next to the 3D Control Pad (GamePad Alt alone). Driven by the
descriptor-less Xbox layout, which has 16-bit sticks and 10-bit triggers. '''
import pytest
from device_data.br import system, dev_mode, bt_conn_type
from pytest_xbox_classic_no_descriptor import make_report


DEVICE_NAME = 'Xbox Wireless Controller'
ACC_NONE, ACC_MEM, ACC_RUMBLE = range(3)
# Saturn buttons are active low
B, C, A, START, UP, DOWN, LEFT, RIGHT = (1 << i for i in range(8))
L, Z, Y, X, R = (1 << i for i in range(11, 16))
CENTER = (0x8000, 0x7FFF, 0x8000, 0x7FFF)


def pressed(out):
    return ~out['btns'] & 0xFFFF


@pytest.fixture
def saturn(blueretro_dut, request):
    ''' A Saturn port in GamePad Alt with the given accessory. '''
    blueretro_dut.ensure_connected()
    blueretro_dut.disconnect()
    blueretro_dut.send_out_cfg(0, f"{dev_mode.PAD_ALT:02x}{request.param:02x}000000")
    blueretro_dut.send_system_id(system.SATURN)
    blueretro_dut.connect(bt_conn_type.BT_BR_EDR)
    blueretro_dut.send_name(DEVICE_NAME)
    for _ in range(2):
        blueretro_dut.send_hid_report(make_report())
    yield blueretro_dut
    blueretro_dut.disconnect()


def send(dut, **kw):
    return dut.send_hid_report(make_report(**kw))['wired_output']


@pytest.mark.parametrize('saturn', [ACC_MEM], indirect=True)
def test_saturn_mission_stick_axes(saturn):
    ''' The stick is X/Y and the right stick's Y the throttle, 0-255 with 128 centered; the
    stick also sets the D-pad bits past its thresholds, as the real stick does. '''
    out = send(saturn, axes=CENTER)
    assert out['raw_axes'][:3] == [128, 128, 128] and pressed(out) == 0

    out = send(saturn, axes=(0xFFFF, 0x7FFF, 0x8000, 0x7FFF))
    assert out['raw_axes'][0] == 255 and pressed(out) == RIGHT
    out = send(saturn, axes=(0x0000, 0x7FFF, 0x8000, 0x7FFF))
    assert out['raw_axes'][0] == 0 and pressed(out) == LEFT
    out = send(saturn, axes=(0x8000, 0xFFFF, 0x8000, 0x7FFF))
    assert out['raw_axes'][1] == 255 and pressed(out) == DOWN
    out = send(saturn, axes=(0x8000, 0x7FFF, 0x8000, 0x0000))
    assert out['raw_axes'][2] == 255 and pressed(out) == 0


@pytest.mark.parametrize('saturn', [ACC_MEM, ACC_NONE], indirect=True)
def test_saturn_l_r_release(saturn):
    ''' L and R follow the triggers and let go with them (on the Mission Stick they used to stay
    held once pressed). '''
    assert pressed(send(saturn, triggers=(1023, 0))) == L
    assert pressed(send(saturn)) == 0
    assert pressed(send(saturn, triggers=(0, 1023))) == R
    assert pressed(send(saturn)) == 0


@pytest.mark.parametrize('saturn', [ACC_RUMBLE], indirect=True)
def test_saturn_arcade_racer_wheel(saturn):
    ''' The wheel is the left stick's X, and turns the D-pad's left and right on past its
    thresholds. '''
    out = send(saturn, axes=(0xFFFF, 0x7FFF, 0x8000, 0x7FFF))
    assert out['axes'][0] == 255 and pressed(out) == RIGHT
    out = send(saturn, axes=(0x0000, 0x7FFF, 0x8000, 0x7FFF))
    assert out['axes'][0] == 0 and pressed(out) == LEFT
    out = send(saturn)
    assert pressed(out) == 0


@pytest.mark.parametrize('saturn', [ACC_MEM, ACC_RUMBLE, ACC_NONE], indirect=True)
def test_saturn_alt_buttons(saturn):
    ''' The face buttons and Start are the same whichever GamePad Alt device it is (upstream's
    layout: the bottom face button is B, the right one C). '''
    assert pressed(send(saturn, buttons=0x01)) == B
    assert pressed(send(saturn, buttons=0x02)) == C
    assert pressed(send(saturn, buttons=0x80)) == START
    assert pressed(send(saturn)) == 0
