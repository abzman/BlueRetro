''' A Bluetooth mouse on the N64 mouse, through bherdm's pointer mapper, and on an N64 controller,
where its motion keeps upstream's mapping. '''
import pytest
from device_data.br import system, dev_mode, bt_conn_type


DEVICE_NAME = 'HID Mouse'
# A boot mouse with a report ID: 3 buttons, X, Y and wheel, 8 bits each
HID_DESC = bytes([
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xC0, 0xC0,
])
N64_MOUSE_A, N64_MOUSE_B = 0x80, 0x40


def mouse(btns=0, x=0, y=0):
    return f'a101{btns:02x}{x & 0xff:02x}{y & 0xff:02x}00'


def start(blueretro):
    blueretro.send_name(DEVICE_NAME)
    blueretro.send_hid_desc(HID_DESC)
    for _ in range(2):
        blueretro.send_hid_report(mouse())


@pytest.mark.parametrize('blueretro', [[system.N64, dev_mode.MOUSE, bt_conn_type.BT_BR_EDR]],
                         indirect=True)
def test_n64_mouse_motion(blueretro):
    ''' Motion accumulates until the console reads it: right is +X, and up (HID -Y) is +Y on the
    N64 mouse. The mapper smooths, so a step can take a few reports to come through whole. '''
    start(blueretro)
    out = blueretro.send_hid_report(mouse(x=10))['wired_output']
    assert out['relative'] == [1, 1]
    x1 = out['axes'][0]
    x2 = blueretro.send_hid_report(mouse(x=10))['wired_output']['axes'][0]
    assert 0 < x1 < x2

    y = [blueretro.send_hid_report(mouse(y=-20))['wired_output']['axes'][1] for _ in range(4)]
    assert y[-1] > 0 and y == sorted(y)


@pytest.mark.parametrize('blueretro', [[system.N64, dev_mode.MOUSE, bt_conn_type.BT_BR_EDR]],
                         indirect=True)
def test_n64_mouse_buttons(blueretro):
    ''' Left is A, right is B. '''
    start(blueretro)
    assert blueretro.send_hid_report(mouse(btns=1))['wired_output']['btns'] == N64_MOUSE_A
    assert blueretro.send_hid_report(mouse(btns=2))['wired_output']['btns'] == N64_MOUSE_B
    assert blueretro.send_hid_report(mouse())['wired_output']['btns'] == 0


# barrenechea 7c77f06's wired_output for the same reports, recorded in QEMU: a mouse on an N64
# controller output goes through upstream's mapping untouched
UPSTREAM_PAD_FROM_MOUSE = [
    (dict(x=20), {'axes': [0, 0], 'btns': 2048}),
    (dict(x=-20), {'axes': [0, 0], 'btns': 2560}),
    (dict(y=20), {'axes': [0, 0], 'btns': 0}),
    (dict(y=-20), {'axes': [0, 0], 'btns': 2048}),
    (dict(btns=1), {'axes': [0, 0], 'btns': 32}),
    (dict(btns=2), {'axes': [0, 0], 'btns': 32}),
    (dict(x=5), {'axes': [0, 0], 'btns': 0}),
    (dict(), {'axes': [0, 0], 'btns': 0}),
]


@pytest.mark.parametrize('blueretro', [[system.N64, dev_mode.PAD, bt_conn_type.BT_BR_EDR]],
                         indirect=True)
def test_n64_controller_from_mouse_matches_upstream(blueretro):
    ''' bherdm's N64 mouse path applies to the N64 mouse only: on a controller output the same
    reports give what upstream gives. '''
    start(blueretro)
    for report, expected in UPSTREAM_PAD_FROM_MOUSE:
        assert blueretro.send_hid_report(mouse(**report))['wired_output'] == expected
