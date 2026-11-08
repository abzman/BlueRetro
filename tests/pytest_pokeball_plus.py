''' andrew171717's Poké Ball Plus decoder: its 5-byte BLE report, fed to the bridge. '''
import pytest
from bit_helper import bit
from device_data.br import system, dev_mode, bt_conn_type, bt_type, pad


DEVICE_NAME = 'Pokemon PBP'
BLE = [[system.GC, dev_mode.PAD, bt_conn_type.BT_LE]]
TOP, STICK_CLICK = 1, 2
X_MIN, X_MID, X_MAX = 32, 112, 192
Y_MIN, Y_MID, Y_MAX = 36, 108, 180


def report(btn=0, x=X_MID, y=Y_MID):
    ''' The button byte, then a 12-bit X split across two bytes, then Y. '''
    return bytes([0, btn, (x & 0xF) << 4, (x >> 4) & 0xF, y]).hex()


def start(blueretro):
    rsp = blueretro.send_name(DEVICE_NAME)
    assert rsp['device_name']['device_type'] == bt_type.PBP
    for _ in range(2):
        blueretro.send_to_bridge(0, report())


@pytest.mark.parametrize('blueretro', BLE, indirect=True)
def test_pokeball_plus_buttons(blueretro):
    ''' The top button is A and clicking the stick is Start, alone or together. '''
    start(blueretro)
    assert blueretro.send_to_bridge(0, report(TOP))['generic_input']['btns'][0] == bit(pad.RB_DOWN)
    assert blueretro.send_to_bridge(0, report(STICK_CLICK))['generic_input']['btns'][0] == bit(pad.MM)
    both = blueretro.send_to_bridge(0, report(TOP | STICK_CLICK))['generic_input']['btns'][0]
    assert both == bit(pad.RB_DOWN) | bit(pad.MM)
    assert blueretro.send_to_bridge(0, report())['generic_input']['btns'][0] == 0


@pytest.mark.parametrize('blueretro', BLE, indirect=True)
def test_pokeball_plus_stick(blueretro):
    ''' Centered is the middle; X's range end is right, Y's low end is up. '''
    start(blueretro)
    assert blueretro.send_to_bridge(0, report())['generic_input']['axes'][:2] == [0, 0]
    out = blueretro.send_to_bridge(0, report(x=X_MAX))['wired_output']
    assert out['axes'][0] > 200
    out = blueretro.send_to_bridge(0, report(x=X_MIN))['wired_output']
    assert out['axes'][0] < 56
    out = blueretro.send_to_bridge(0, report(y=Y_MIN))['wired_output']
    assert out['axes'][1] > 200
    out = blueretro.send_to_bridge(0, report(y=Y_MAX))['wired_output']
    assert out['axes'][1] < 56
