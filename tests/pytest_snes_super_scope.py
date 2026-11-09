''' The SNES Super Scope (Gamepad Alt on the 2nd port) driven by a Wiimote's IR pointer, up to
what the adapter hands the SNES driver: the buttons held and the aim. The driver's report and the
timing of its light pulse are checked on the host (pytest_beam_timing.py). '''
import pytest
from device_data.br import system, dev_mode, bt_conn_type
from pytest_wiimote_ir_controller import ir_report
from pytest_wiimote_ir_init import bt_tx, LEDS, IR_ON, IR_WRITES, REPORT_CORE
from pytest_xbox_classic_no_descriptor import make_report


WIIMOTE = 'Nintendo RVL-CNT-01'
XBOX = 'Xbox Wireless Controller'

# What the adapter hands the driver: the scope's report bits, held
FIRE, CURSOR, TURBO, PAUSE, OFFSCREEN = 0x80, 0x40, 0x20, 0x10, 0x02
# Wiimote core buttons
W_LEFT, W_RIGHT, W_DOWN, W_UP, W_PLUS = (1 << i for i in range(5))
W_2, W_1, W_B, W_A, W_MINUS = (1 << i for i in range(8, 13))

CENTER = [(412, 384), (612, 384)]   # the sensor bar in the middle of the camera's view


def setup(dut, port1_mode=dev_mode.PAD):
    ''' Port 1 a pad on handle 0, port 2 the scope; returns the response to naming the Wiimote,
    connected on handle 1, so device 1, port 2. '''
    dut.ensure_connected()
    for handle in (1, 0):
        dut.handle = handle
        dut.disconnect()
    dut.send_out_cfg(0, f"{port1_mode:02x}00000000")
    dut.send_out_cfg(1, f"{dev_mode.PAD_ALT:02x}00000000")
    dut.send_system_id(system.SNES)
    dut.handle = 0
    dut.connect(bt_conn_type.BT_BR_EDR)
    dut.send_name(XBOX)
    dut.handle = 1
    dut.connect(bt_conn_type.BT_BR_EDR)
    return dut.send_name(WIIMOTE)


@pytest.fixture
def scope(blueretro_dut):
    setup(blueretro_dut)
    for _ in range(2):
        blueretro_dut.send_hid_report(ir_report([]))
    yield blueretro_dut
    for handle in (1, 0):
        blueretro_dut.handle = handle
        blueretro_dut.disconnect()
    blueretro_dut.handle = 0


def aim(dut, dots, btns=0):
    out = dut.send_hid_report(ir_report(dots, btns))['wired_output']
    return out['x'], out['y'], out['held']


def test_super_scope_turns_wiimote_camera_on(blueretro_dut):
    ''' The scope wants an absolute pointer: the Wiimote's camera is set up. '''
    tx = bt_tx(setup(blueretro_dut))
    assert tx[0].startswith('a211') and tx[1:3] == IR_ON and tx[3].startswith(IR_WRITES[0])
    for handle in (1, 0):
        blueretro_dut.handle = handle
        blueretro_dut.disconnect()
    blueretro_dut.handle = 0


def test_super_scope_aim(scope):
    ''' The aim covers the picture: 256 dots by 224 lines, right is more X, down more Y. '''
    x, y, held = aim(scope, CENTER)
    assert abs(x - 128) <= 2 and abs(y - 112) <= 2 and held == 0
    x, _, _ = aim(scope, [(100, 384), (300, 384)])
    assert 200 < x <= 255
    x, _, _ = aim(scope, [(724, 384), (924, 384)])
    assert 0 <= x < 56
    _, y, _ = aim(scope, [(412, 50), (612, 50)])
    assert 0 <= y < 40
    _, y, _ = aim(scope, [(412, 700), (612, 700)])
    assert 184 < y <= 223


def test_super_scope_buttons(scope):
    ''' B fires, A is Cursor, + and 2 Pause, - and 1 flip the turbo switch. '''
    for btn, held in ((W_B, FIRE), (W_A, CURSOR), (W_PLUS, PAUSE), (W_2, PAUSE),
                      (W_MINUS, TURBO), (W_1, TURBO)):
        assert aim(scope, CENTER, btn)[2] == held, hex(btn)
        assert aim(scope, CENTER)[2] == 0
    assert aim(scope, CENTER, W_B | W_A)[2] == FIRE | CURSOR


def test_super_scope_offscreen(scope):
    ''' No sensor bar in view, or any D-pad direction held: off the screen. '''
    assert aim(scope, [])[2] == OFFSCREEN
    assert aim(scope, [], W_B)[2] == OFFSCREEN | FIRE
    for d in (W_LEFT, W_RIGHT, W_DOWN, W_UP):
        assert aim(scope, CENTER, d | W_B)[2] == OFFSCREEN | FIRE
    assert aim(scope, CENTER)[2] == 0


def test_super_scope_gamepad_stick(blueretro_dut):
    ''' Without a pointer source the left stick aims. '''
    setup(blueretro_dut)
    blueretro_dut.handle = 1
    blueretro_dut.disconnect()
    blueretro_dut.connect(bt_conn_type.BT_BR_EDR)
    blueretro_dut.send_name(XBOX)
    for _ in range(2):
        blueretro_dut.send_hid_report(make_report())
    out = blueretro_dut.send_hid_report(make_report(axes=(0xFFFF, 0x7FFF, 0x8000, 0x7FFF)))['wired_output']
    assert out['x'] >= 252 and abs(out['y'] - 112) <= 3
    for handle in (1, 0):
        blueretro_dut.handle = handle
        blueretro_dut.disconnect()
    blueretro_dut.handle = 0


def test_super_scope_only_on_port_2(blueretro_dut):
    ''' Port 1's pin 6 doesn't reach the counter latch: Gamepad Alt there is a pad, and a Wiimote
    on it keeps its camera off. '''
    blueretro_dut.ensure_connected()
    blueretro_dut.handle = 0
    blueretro_dut.disconnect()
    blueretro_dut.send_out_cfg(0, f"{dev_mode.PAD_ALT:02x}00000000")
    blueretro_dut.send_system_id(system.SNES)
    blueretro_dut.connect(bt_conn_type.BT_BR_EDR)
    assert bt_tx(blueretro_dut.send_name(WIIMOTE)) == [LEDS, REPORT_CORE]
    rsp = blueretro_dut.send_hid_report('a1310000808080')
    rsp = blueretro_dut.send_hid_report('a1310400808080')
    assert 'held' not in rsp.get('wired_output', {})
    blueretro_dut.disconnect()
