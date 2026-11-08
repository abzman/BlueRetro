''' The PlayStation GunCon (Mouse mode with the rumble accessory) driven by a Wiimote's IR pointer,
or by a gamepad's left stick. '''
import pytest
from device_data.br import system, dev_mode, bt_conn_type
from pytest_wiimote_ir_controller import ir_report
from pytest_wiimote_ir_init import bt_tx, LEDS, IR_ON, IR_WRITES
from pytest_xbox_classic_no_descriptor import make_report


WIIMOTE = 'Nintendo RVL-CNT-01'
XBOX = 'Xbox Wireless Controller'
ACC_NONE, ACC_MEM, ACC_RUMBLE, ACC_BOTH = range(4)

# The gun's buttons, active low, in the pad's bit positions
TRIGGER, A, B = 1 << 13, 1 << 3, 1 << 14
# Wiimote core buttons
W_LEFT, W_RIGHT, W_DOWN, W_UP, W_PLUS = (1 << i for i in range(5))
W_2, W_1, W_B, W_A, W_MINUS = (1 << i for i in range(8, 13))

X_MIN, X_MAX, Y_MIN, Y_MAX = 0x4D, 0x1CD, 0x19, 0xF8
OFFSCREEN = (0x01, 0x0A)
CENTER = [(412, 384), (612, 384)]   # the sensor bar in the middle of the camera's view


def connect(dut, sys_id, acc, name):
    dut.ensure_connected()
    dut.disconnect()
    dut.send_out_cfg(0, f"{dev_mode.MOUSE:02x}{acc:02x}000000")
    dut.send_system_id(sys_id)
    dut.connect(bt_conn_type.BT_BR_EDR)
    return dut.send_name(name)


@pytest.fixture
def guncon(blueretro_dut, request):
    ''' A GunCon port with a Wiimote; param: (system, accessory) '''
    sys_id, acc = getattr(request, 'param', (system.PSX, ACC_RUMBLE))
    connect(blueretro_dut, sys_id, acc, WIIMOTE)
    for _ in range(2):
        blueretro_dut.send_hid_report(ir_report([]))
    yield blueretro_dut
    blueretro_dut.disconnect()


def aim(dut, dots, btns=0):
    out = dut.send_hid_report(ir_report(dots, btns))['wired_output']
    return out['x'], out['y'], out['btns']


def pressed(btns):
    return ~btns & 0xFFFF


@pytest.mark.parametrize('acc', [ACC_RUMBLE, ACC_BOTH])
def test_guncon_turns_wiimote_camera_on(blueretro_dut, acc):
    ''' The GunCon wants an absolute pointer, so the Wiimote's camera is set up. '''
    tx = bt_tx(connect(blueretro_dut, system.PSX, acc, WIIMOTE))
    assert tx[0] == LEDS and tx[1:3] == IR_ON and tx[3].startswith(IR_WRITES[0])
    blueretro_dut.disconnect()


@pytest.mark.parametrize('guncon', [(system.PSX, ACC_RUMBLE), (system.PS2, ACC_RUMBLE),
                                    (system.PSX, ACC_BOTH)], indirect=True)
def test_guncon_position(guncon):
    ''' The middle of the screen is the middle of both ranges; right is more X (the camera image is
    mirrored, so the bar on its left means aiming right), and down is more Y. '''
    x, y, btns = aim(guncon, CENTER)
    assert abs(x - (X_MIN + X_MAX) // 2) <= 2 and abs(y - (Y_MIN + Y_MAX) // 2) <= 2
    assert btns == 0xFFFF

    x, y, _ = aim(guncon, [(100, 384), (300, 384)])
    assert x > X_MAX - 120 and x <= X_MAX
    x, y, _ = aim(guncon, [(724, 384), (924, 384)])
    assert x < X_MIN + 120 and x >= X_MIN
    x, y, _ = aim(guncon, [(412, 50), (612, 50)])
    assert y < Y_MIN + 60 and y >= Y_MIN
    x, y, _ = aim(guncon, [(412, 700), (612, 700)])
    assert y > Y_MAX - 60 and y <= Y_MAX


def test_guncon_buttons(guncon):
    ''' B fires; A is the gun's A; 1, 2 and - are its B. One button released while another
    source of the same gun button is held leaves it pressed. '''
    for btn, gun in ((W_B, TRIGGER), (W_A, A), (W_PLUS, A), (W_1, B), (W_2, B), (W_MINUS, B)):
        assert pressed(aim(guncon, CENTER, btn)[2]) == gun, hex(btn)
        assert pressed(aim(guncon, CENTER)[2]) == 0

    assert pressed(aim(guncon, CENTER, W_1 | W_2)[2]) == B
    assert pressed(aim(guncon, CENTER, W_2)[2]) == B
    assert pressed(aim(guncon, CENTER, W_B | W_A)[2]) == TRIGGER | A
    assert pressed(aim(guncon, CENTER)[2]) == 0


def test_guncon_offscreen_without_bar(guncon):
    ''' Before the bar is seen, and whenever it is lost, the gun sees no light: shooting there is
    the reload shot. Aiming back at the screen picks the position up again. '''
    x, y, _ = aim(guncon, [])
    assert (x, y) == OFFSCREEN
    assert aim(guncon, CENTER)[:2] != OFFSCREEN
    x, y, btns = aim(guncon, [], W_B)
    assert (x, y) == OFFSCREEN and pressed(btns) == TRIGGER
    x, y, _ = aim(guncon, CENTER)
    assert abs(x - (X_MIN + X_MAX) // 2) <= 2


def test_guncon_dpad_aims_offscreen(guncon):
    ''' Any D-pad direction aims off the screen while held, bar in view or not. '''
    for d in (W_LEFT, W_RIGHT, W_DOWN, W_UP):
        x, y, btns = aim(guncon, CENTER, d | W_B)
        assert (x, y) == OFFSCREEN and pressed(btns) == TRIGGER
        assert aim(guncon, CENTER)[:2] != OFFSCREEN


def test_guncon_gamepad_stick(blueretro_dut):
    ''' Without a pointer source the left stick aims: full right is the right edge. '''
    connect(blueretro_dut, system.PSX, ACC_RUMBLE, XBOX)
    for _ in range(2):
        blueretro_dut.send_hid_report(make_report())
    out = blueretro_dut.send_hid_report(make_report(axes=(0x8000, 0x7FFF, 0x8000, 0x7FFF)))['wired_output']
    assert abs(out['x'] - (X_MIN + X_MAX) // 2) <= 4
    out = blueretro_dut.send_hid_report(make_report(axes=(0xFFFF, 0x7FFF, 0x8000, 0x7FFF)))['wired_output']
    assert out['x'] >= X_MAX - 4
    blueretro_dut.disconnect()


@pytest.mark.parametrize('acc', [ACC_NONE, ACC_MEM])
def test_mouse_without_rumble_accessory_is_a_mouse(blueretro_dut, acc):
    ''' Mouse mode stays the PlayStation mouse without the rumble accessory. '''
    connect(blueretro_dut, system.PSX, acc, WIIMOTE)
    for _ in range(2):
        blueretro_dut.send_hid_report(ir_report([]))
    rsp = blueretro_dut.send_hid_report(ir_report(CENTER, W_B))
    assert 'x' not in rsp.get('wired_output', {})
    blueretro_dut.disconnect()
