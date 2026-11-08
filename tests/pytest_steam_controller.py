''' jjtcm's Steam Controller 2026 (Triton) decoder: the BLE state report 0x45, fed to the bridge
the way its GATT notification arrives. '''
import struct
import pytest
from bit_helper import bit
from device_data.br import system, dev_mode, bt_conn_type, bt_type, pad


DEVICE_NAME = 'Steam Controller 2026'
BLE = [[system.GC, dev_mode.PAD, bt_conn_type.BT_LE]]
REPORT_ID = 0x45


def state(btns=0, lt=0, rt=0, lx=0, ly=0, rx=0, ry=0):
    ''' Report ID, sequence, 32 button bits, then int16 triggers and sticks (Y + is up). '''
    return struct.pack('<BBIhhhhhh', REPORT_ID, 0, btns, lt, rt, lx, ly, rx, ry).hex()


def start(blueretro):
    rsp = blueretro.send_name(DEVICE_NAME)
    assert rsp['device_name']['device_type'] == bt_type.STEAM
    for _ in range(2):
        blueretro.send_to_bridge(REPORT_ID, state())


@pytest.mark.parametrize('blueretro', BLE, indirect=True)
@pytest.mark.parametrize(('btns', 'expected'), [
    (0x00000001, bit(pad.RB_DOWN)),     # A
    (0x00000002, bit(pad.RB_RIGHT)),    # B
    (0x00000004, bit(pad.RB_LEFT)),     # X
    (0x00000008, bit(pad.RB_UP)),       # Y
    (0x00004000, bit(pad.MM)),          # Menu
    (0x00000040, bit(pad.MS)),          # View
    (0x00010000, bit(pad.MT)),          # Steam
    (0x00080000, bit(pad.LS)),          # LB
    (0x00000200, bit(pad.RS)),          # RB
    (0x00008000, bit(pad.LJ)),          # L3
    (0x00000020, bit(pad.RJ)),          # R3
    (0x00001000, bit(pad.LD_LEFT)),
    (0x00000800, bit(pad.LD_RIGHT)),
    (0x00000400, bit(pad.LD_DOWN)),
    (0x00002000, bit(pad.LD_UP)),
])
def test_steam_controller_buttons(blueretro, btns, expected):
    start(blueretro)
    rsp = blueretro.send_to_bridge(REPORT_ID, state(btns=btns))
    assert rsp['wireless_input']['btns'] == btns
    assert rsp['generic_input']['btns'][0] == expected


@pytest.mark.parametrize('blueretro', BLE, indirect=True)
def test_steam_controller_axes(blueretro):
    ''' Full right and full up reach the GC stick's ends; the trigger is analog. '''
    start(blueretro)
    out = blueretro.send_to_bridge(REPORT_ID, state(lx=32767, ly=32767))['wired_output']
    assert out['axes'][0] > 200 and out['axes'][1] > 200
    out = blueretro.send_to_bridge(REPORT_ID, state(lx=-32767, ly=-32767))['wired_output']
    assert out['axes'][0] < 56 and out['axes'][1] < 56
    out = blueretro.send_to_bridge(REPORT_ID, state(lt=32767))['wired_output']
    assert out['axes'][4] > 200
    out = blueretro.send_to_bridge(REPORT_ID, state())['wired_output']
    assert out['axes'][:4] == [128, 128, 128, 128]
