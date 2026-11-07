''' Tests for the Wiimote IR camera pointer (report 0x33).

Axes are [abs X, abs Y, rel dX, rel dY]: absolute pointer position on
LX/LY (light gun style outputs) and per report motion on RX/RY (mouse
outputs). '''
from bit_helper import swap16


DEVICE_NAME = 'Nintendo RVL-CNT-01'
NO_DOT = 'ffffff'


def ir_dot(x, y, size=3):
    ''' Encode one extended mode IR dot. '''
    return f'{x & 0xff:02x}{y & 0xff:02x}{((y >> 8) << 6) | ((x >> 8) << 4) | size:02x}'


def ir_report(dots, btns=0):
    ''' Build a 0x33 report: buttons, accel, 4 IR dots. '''
    ir = ''.join(ir_dot(x, y) for x, y in dots)
    ir += NO_DOT * (4 - len(dots))
    return f'a133{swap16(btns):04x}808099{ir}'


def test_wiimote_ir_pointer(blueretro):
    ''' Sensor bar position maps to centered absolute pointer axes. '''
    rsp = blueretro.send_name(DEVICE_NAME)
    assert rsp['device_name']['device_type'] == 2
    assert rsp['device_name']['device_subtype'] == 0

    for _ in range(2):
        blueretro.send_hid_report(ir_report([]))

    # Bar centered in camera view => pointer centered
    rsp = blueretro.send_hid_report(ir_report([(412, 384), (612, 384)]))
    x, y, dx, dy = rsp['wireless_input']['axes']
    assert abs(x) <= 2 and abs(y) <= 2
    assert rsp['wireless_input']['ir_dots'] == 2
    # First report after (re)acquire never produces motion
    assert dx == 0 and dy == 0

    # Small move: relative motion matches absolute change
    rsp = blueretro.send_hid_report(ir_report([(402, 384), (602, 384)]))
    x2, _, dx, dy = rsp['wireless_input']['axes']
    assert dx == x2 - x and dx > 0 and dy == 0

    # Camera image is mirrored: bar on the left => pointing right
    rsp = blueretro.send_hid_report(ir_report([(100, 384), (300, 384)]))
    assert rsp['wireless_input']['axes'][0] > 250

    # Bar high in camera image => pointing up (positive Y)
    rsp = blueretro.send_hid_report(ir_report([(412, 50), (612, 50)]))
    assert rsp['wireless_input']['axes'][1] > 350

    # One dot lost: midpoint rebuilt from remaining dot, no jump
    rsp = blueretro.send_hid_report(ir_report([(400, 300), (600, 300)]))
    before = rsp['wireless_input']['axes']
    rsp = blueretro.send_hid_report(ir_report([(600, 300)]))
    assert rsp['wireless_input']['ir_dots'] == 1
    assert abs(rsp['wireless_input']['axes'][0] - before[0]) <= 2

    # Nothing seen: hold last position, no motion
    rsp = blueretro.send_hid_report(ir_report([]))
    assert rsp['wireless_input']['ir_dots'] == 0
    assert abs(rsp['wireless_input']['axes'][0] - before[0]) <= 2
    assert rsp['wireless_input']['axes'][2:] == [0, 0]

    # Reacquire far away: position jumps, motion doesn't
    rsp = blueretro.send_hid_report(ir_report([(800, 100), (1000, 100)]))
    assert rsp['wireless_input']['axes'][2:] == [0, 0]

    # Buttons still decoded (B = bit 10), and B doubles as mouse left click
    rsp = blueretro.send_hid_report(ir_report([(412, 384), (612, 384)], 1 << 10))
    assert rsp['wireless_input']['btns'] == 1 << 10
    if 'generic_input' in rsp:
        assert rsp['generic_input']['btns'][0] & (1 << 28)  # MOUSE_LEFT
