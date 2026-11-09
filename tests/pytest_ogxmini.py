''' OGX-Mini output: the Pico's SET_PAD packet built from each controller (pressure where the
controller has it, 255 for a digital press), its rumble passed back to the controller only with
the Rumble accessory, and the packet code itself on the host (layout against OGX-Mini's own
structs, turbo, PacketOut). Nothing in QEMU runs the I2C driver. '''
import os
import shutil
import subprocess
import pytest
from bit_helper import swap32
from device_data.br import system, dev_mode, bt_conn_type
from device_data.ps3 import ps3
from pytest_xbox_classic_no_descriptor import make_report as xbox_report

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DS3 = 'PLAYSTATION(R)3'
XBOX = 'Xbox Wireless Controller'
ACC_NONE, ACC_MEM, ACC_RUMBLE, ACC_BOTH = range(4)
FB_TYPE_RUMBLE = 1

# PadIn
DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT = 0x01, 0x02, 0x04, 0x08
BTN_A, BTN_B, BTN_X, BTN_Y = 0x0001, 0x0002, 0x0004, 0x0008
BTN_L3, BTN_R3, BTN_BACK, BTN_START = 0x0010, 0x0020, 0x0040, 0x0080
BTN_LB, BTN_RB, BTN_SYS, BTN_MISC = 0x0100, 0x0200, 0x0400, 0x0800
AN_UP, AN_DOWN, AN_LEFT, AN_RIGHT, AN_A, AN_B, AN_X, AN_Y, AN_LB, AN_RB = range(10)

# DS3 report: axes[] index of each pressure
DS3_AX = {'lx': 0, 'ly': 1, 'rx': 2, 'ry': 3, 'up': 8, 'right': 9, 'down': 10, 'left': 11,
          'l2': 12, 'r2': 13, 'l1': 14, 'r1': 15, 't': 16, 'c': 17, 'x': 18, 's': 19}
DS3_TAIL = '000000030516ffd4000033ad77004001ef027401d801fe'


def ds3_report(btns=0, **axes):
    ''' A DS3 input report: buttons by ps3 bit, sticks and pressures by name. '''
    data = [0x80] * 4 + [0] * 16
    for name, value in axes.items():
        data[DS3_AX[name]] = value
    return 'a101' + f'{swap32(btns):08x}' + '00' + bytes(data).hex() + DS3_TAIL


def connect(dut, name, acc=ACC_RUMBLE, mode=dev_mode.PAD, first=None):
    ''' A controller on an OGX-Mini output; first: its first reports, if not at rest '''
    dut.ensure_connected()
    dut.disconnect()
    dut.send_out_cfg(0, f"{mode:02x}{acc:02x}000000")
    dut.send_system_id(system.OGX_MINI)
    dut.connect(bt_conn_type.BT_BR_EDR)
    dut.send_name(name)
    rest = first or (ds3_report() if name == DS3 else xbox_report())
    for _ in range(3):
        dut.send_hid_report(rest)
    return dut


@pytest.fixture
def ds3(blueretro_dut):
    yield connect(blueretro_dut, DS3)
    blueretro_dut.disconnect()


@pytest.fixture
def xbox(blueretro_dut, request):
    yield connect(blueretro_dut, XBOX, acc=getattr(request, 'param', ACC_RUMBLE))
    blueretro_dut.disconnect()


def out(rsp):
    return rsp['wired_output']


def test_ogxmini_at_rest(ds3):
    o = out(ds3.send_hid_report(ds3_report()))
    assert o['dpad'] == 0 and o['btns'] == 0 and o['triggers'] == [0, 0]
    assert o['axes'] == [0, 0, 0, 0]
    assert o['analog'] == [0] * 10


@pytest.mark.parametrize(('btn', 'axis', 'field', 'value', 'analog'), [
    (ps3.X, 'x', 'btns', BTN_A, AN_A),
    (ps3.C, 'c', 'btns', BTN_B, AN_B),
    (ps3.S, 's', 'btns', BTN_X, AN_X),
    (ps3.T, 't', 'btns', BTN_Y, AN_Y),
    (ps3.L1, 'l1', 'btns', BTN_LB, AN_LB),
    (ps3.R1, 'r1', 'btns', BTN_RB, AN_RB),
    (ps3.D_UP, 'up', 'dpad', DPAD_UP, AN_UP),
    (ps3.D_DOWN, 'down', 'dpad', DPAD_DOWN, AN_DOWN),
    (ps3.D_LEFT, 'left', 'dpad', DPAD_LEFT, AN_LEFT),
    (ps3.D_RIGHT, 'right', 'dpad', DPAD_RIGHT, AN_RIGHT),
])
def test_ogxmini_ds3_pressure(ds3, btn, axis, field, value, analog):
    ''' A DS3's pressure goes through: a light press is the button with its pressure, a full
    press 255 (give or take the source's pull back) '''
    o = out(ds3.send_hid_report(ds3_report(1 << btn, **{axis: 0x40})))
    assert o[field] == value
    assert 0x38 <= o['analog'][analog] <= 0x48
    assert sum(o['analog']) == o['analog'][analog]

    o = out(ds3.send_hid_report(ds3_report(1 << btn, **{axis: 0xFF})))
    assert o[field] == value and o['analog'][analog] == 255

    o = out(ds3.send_hid_report(ds3_report()))
    assert o[field] == 0 and o['analog'][analog] == 0


@pytest.mark.parametrize(('btn', 'value'), [
    (ps3.START, BTN_START), (ps3.SELECT, BTN_BACK), (ps3.PS, BTN_SYS),
    (ps3.L3, BTN_L3), (ps3.R3, BTN_R3),
])
def test_ogxmini_ds3_digital_buttons(ds3, btn, value):
    o = out(ds3.send_hid_report(ds3_report(1 << btn)))
    assert o['btns'] == value and o['dpad'] == 0 and o['analog'] == [0] * 10


def test_ogxmini_ds3_sticks_and_triggers(ds3):
    ''' 16-bit sticks, Y down positive as the Pico takes them; 8-bit triggers '''
    o = out(ds3.send_hid_report(ds3_report(lx=0xFF, ly=0x00, rx=0x00, ry=0xFF, l2=0xFF, r2=0x80)))
    lx, ly, rx, ry = o['axes']
    assert lx > 30000 and ly < -30000 and rx < -30000 and ry > 30000
    assert o['triggers'][0] == 255 and 0x78 <= o['triggers'][1] <= 0x88
    assert o['btns'] == 0


def test_ogxmini_digital_press_reads_255(xbox):
    ''' A controller without pressure: its face buttons, bumpers and D-pad read 255 '''
    o = out(xbox.send_hid_report(xbox_report(buttons=0x01)))
    assert o['btns'] == BTN_A and o['analog'][AN_A] == 255
    o = out(xbox.send_hid_report(xbox_report(dpad=1)))
    assert o['dpad'] == DPAD_UP and o['analog'][AN_UP] == 255 and o['analog'][AN_A] == 0
    o = out(xbox.send_hid_report(xbox_report(triggers=(1023, 0))))
    assert o['triggers'] == [255, 0]


def test_ogxmini_no_center_calibration(blueretro_dut):
    ''' A stick held while the controller connects stays where it is: no calibration to its
    first report on OGX-Mini (games and the console bring their own dead zones). '''
    held = ds3_report(lx=0xC0)
    dut = connect(blueretro_dut, DS3, first=held)
    try:
        assert out(dut.send_hid_report(held))['axes'][0] > 10000
        assert out(dut.send_hid_report(ds3_report()))['axes'][0] == 0
    finally:
        dut.disconnect()


@pytest.mark.parametrize('xbox', [ACC_RUMBLE, ACC_BOTH], indirect=True)
def test_ogxmini_rumble_to_controller(xbox):
    ''' The Pico's two motors, any strength, go on to the controller, then stop '''
    rsp = xbox.send_fb(0, FB_TYPE_RUMBLE, '8040')
    assert rsp['fb_changed'] == 1
    # After the L2CAP header: HIDP output, report 0x03, enable mask, LT, RT, then the two motors
    assert rsp['bt_tx0'][9:14] == [0xA2, 0x03, 0x0F, 0, 0]
    assert sorted(rsp['bt_tx0'][14:16]) == [0x40, 0x80]
    rsp = xbox.send_fb(0, FB_TYPE_RUMBLE, '0000')
    assert rsp['fb_changed'] == 1 and rsp['bt_tx0'][14:16] == [0, 0]


@pytest.mark.parametrize('xbox', [ACC_NONE, ACC_MEM], indirect=True)
def test_ogxmini_rumble_needs_accessory(xbox):
    ''' Without the Rumble accessory set, the Pico's rumble stays off the controller '''
    rsp = xbox.send_fb(0, FB_TYPE_RUMBLE, 'ffff')
    # The Xbox controller takes its output report whatever changed: both motors stay off in it
    assert 'bt_tx0' not in rsp or rsp['bt_tx0'][14:16] == [0, 0]


@pytest.fixture(scope='module')
def pad_test(tmp_path_factory):
    cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc:
        pytest.skip('no host C compiler (CI installs one)')
    exe = str(tmp_path_factory.mktemp('ogxm') / 'ogxmini_pad_test')
    src = os.path.join(ROOT, 'main', 'adapter', 'wired')
    subprocess.run([cc, '-O2', '-Wall', '-Wextra', '-Werror', '-I', src, '-o', exe,
                    os.path.join(HERE, 'ogxmini_pad_test.c'), os.path.join(src, 'ogxmini_pad.c')],
                   check=True)
    return exe


def test_ogxmini_packets_on_host(pad_test):
    ''' Packet layout against OGX-Mini's structs, modes, turbo, pressure fill, PacketOut check '''
    res = subprocess.run([pad_test], capture_output=True, text=True)
    assert res.returncode == 0 and res.stdout.strip() == 'ok', res.stdout
