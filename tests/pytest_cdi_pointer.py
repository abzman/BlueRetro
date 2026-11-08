''' The CD-i outputs a Wiimote's IR pointer drives: the absolute pointing device
(GamePad Alt, the Peacekeeper light gun's protocol) and the relative mouse. '''
import pytest
from device_data.br import system, dev_mode, bt_conn_type
from pytest_wiimote_ir_controller import ir_report


DEVICE_NAME = 'Nintendo RVL-CNT-01'
CDI_HDR = 0x40          # always set in the CD-i packet's first byte
CDI_1, CDI_2 = 0x20, 0x10
WIIMOTE_A, WIIMOTE_B = 1 << 11, 1 << 10
CENTER = [(412, 384), (612, 384)]   # the sensor bar in the middle of the camera's view


def start(blueretro):
    blueretro.send_name(DEVICE_NAME)
    for _ in range(2):
        blueretro.send_hid_report(ir_report([]))


@pytest.mark.parametrize('blueretro', [[system.CDI, dev_mode.PAD_ALT, bt_conn_type.BT_BR_EDR]],
                         indirect=True)
def test_cdi_absolute_pointer_position(blueretro):
    ''' Pointing at the middle of the screen is the middle; right is +X; up is -Y (the CD-i
    counts down the screen). The camera image is mirrored, so the bar on its left means right. '''
    start(blueretro)

    out = blueretro.send_hid_report(ir_report(CENTER))['wired_output']
    assert all(abs(v) <= 2 for v in out['axes'])
    assert out['btns'] == CDI_HDR

    x, y = blueretro.send_hid_report(ir_report([(100, 384), (300, 384)]))['wired_output']['axes']
    assert x > 250 and abs(y) <= 2
    x, y = blueretro.send_hid_report(ir_report([(724, 384), (924, 384)]))['wired_output']['axes']
    assert x < -250 and abs(y) <= 2
    x, y = blueretro.send_hid_report(ir_report([(412, 50), (612, 50)]))['wired_output']['axes']
    assert y < -350 and abs(x) <= 2
    x, y = blueretro.send_hid_report(ir_report([(412, 700), (612, 700)]))['wired_output']['axes']
    assert y > 350 and abs(x) <= 2


@pytest.mark.parametrize('blueretro', [[system.CDI, dev_mode.PAD_ALT, bt_conn_type.BT_BR_EDR]],
                         indirect=True)
def test_cdi_absolute_pointer_buttons(blueretro):
    ''' B (the trigger) is button 1 and A is button 2; releasing one leaves the other held. '''
    start(blueretro)

    assert blueretro.send_hid_report(ir_report(CENTER, WIIMOTE_B))['wired_output']['btns'] == CDI_HDR | CDI_1
    assert blueretro.send_hid_report(ir_report(CENTER, WIIMOTE_A))['wired_output']['btns'] == CDI_HDR | CDI_2
    both = ir_report(CENTER, WIIMOTE_A | WIIMOTE_B)
    assert blueretro.send_hid_report(both)['wired_output']['btns'] == CDI_HDR | CDI_1 | CDI_2
    assert blueretro.send_hid_report(ir_report(CENTER, WIIMOTE_A))['wired_output']['btns'] == CDI_HDR | CDI_2
    assert blueretro.send_hid_report(ir_report(CENTER))['wired_output']['btns'] == CDI_HDR


@pytest.mark.parametrize('blueretro', [[system.CDI, dev_mode.PAD_ALT, bt_conn_type.BT_BR_EDR]],
                         indirect=True)
def test_cdi_absolute_pointer_holds_when_bar_lost(blueretro):
    ''' With the bar out of view the pointer stays where it was rather than snapping back. '''
    start(blueretro)
    held = blueretro.send_hid_report(ir_report([(100, 384), (300, 384)]))['wired_output']['axes']
    out = blueretro.send_hid_report(ir_report([]))['wired_output']['axes']
    assert all(abs(a - b) <= 2 for a, b in zip(out, held))


@pytest.mark.parametrize('blueretro', [[system.CDI, dev_mode.MOUSE, bt_conn_type.BT_BR_EDR]],
                         indirect=True)
def test_cdi_mouse_from_wiimote_pointer(blueretro):
    ''' On the CD-i mouse the pointer's motion accumulates until the console reads it. '''
    start(blueretro)
    out = blueretro.send_hid_report(ir_report(CENTER))['wired_output']
    assert out['relative'] == [1, 1]
    assert out['axes'] == [0, 0]     # the first report after acquiring the bar is no motion

    x1 = blueretro.send_hid_report(ir_report([(402, 384), (602, 384)]))['wired_output']['axes'][0]
    x2 = blueretro.send_hid_report(ir_report([(392, 384), (592, 384)]))['wired_output']['axes'][0]
    assert 0 < x1 < x2
    assert blueretro.send_hid_report(ir_report([(392, 384), (592, 384)], WIIMOTE_B))['wired_output']['btns'] & CDI_1
