''' The Wiimote's IR camera is switched on only for outputs that want a pointer
(wired_ptr_caps), with the wiibrew sequence, one register write per ack. '''
import pytest
from device_data.br import system, dev_mode, bt_conn_type


DEVICE_NAME = 'Nintendo RVL-CNT-01'

# Output reports sent at init, after the 0xA2 HIDP header
LEDS = 'a21110'
IR_ON = ['a21304', 'a21a04']
IR_WRITES = [
    'a21604b000300108',                      # 0xB00030 <- 0x01: start the camera config
    'a21604b0000009020000710100aa0064',      # sensitivity block 1
    'a21604b0001a0263',                      # sensitivity block 2
    'a21604b000330103',                      # mode 3: extended, 3 bytes a dot
    'a21604b000300108',                      # 0xB00030 <- 0x08: done
]
REPORT_IR = 'a2120433'                       # continuous report 0x33: buttons, accel, IR
REPORT_CORE = 'a2120435'                     # without the camera: 0x35, buttons, accel, extension
ACK_WRITE = 'a12200001600'                   # ack of output report 0x16, no error


def bt_tx(rsp):
    ''' The ACL packets the host sent while handling one command, HIDP header on. '''
    keys = sorted((k for k in rsp if k.startswith('bt_tx')), key=lambda k: int(k[5:]))
    return [bytes(rsp[k][9:]).hex() for k in keys]


@pytest.mark.parametrize('blueretro', [
    [system.CDI, dev_mode.PAD_ALT, bt_conn_type.BT_BR_EDR],
    [system.CDI, dev_mode.MOUSE, bt_conn_type.BT_BR_EDR],
    [system.PSX, dev_mode.MOUSE, bt_conn_type.BT_BR_EDR],
    [system.N64, dev_mode.MOUSE, bt_conn_type.BT_BR_EDR],
], indirect=True)
def test_wiimote_ir_camera_on_for_pointer_outputs(blueretro):
    ''' A light gun or mouse output turns the camera on and walks the whole sequence. '''
    tx = bt_tx(blueretro.send_name(DEVICE_NAME))
    assert tx[0] == LEDS
    assert tx[1:3] == IR_ON
    assert tx[3].startswith(IR_WRITES[0])

    for write in IR_WRITES[1:]:
        tx = bt_tx(blueretro.send_hid_report(ACK_WRITE))
        assert len(tx) == 1 and tx[0].startswith(write)
    assert bt_tx(blueretro.send_hid_report(ACK_WRITE)) == [REPORT_IR]


@pytest.mark.parametrize('blueretro', [
    [system.GC, dev_mode.PAD, bt_conn_type.BT_BR_EDR],
    [system.CDI, dev_mode.PAD, bt_conn_type.BT_BR_EDR],
], indirect=True)
def test_wiimote_ir_camera_off_for_pads(blueretro):
    ''' A plain gamepad output leaves the camera off and keeps the usual report. '''
    tx = bt_tx(blueretro.send_name(DEVICE_NAME))
    assert tx == [LEDS, REPORT_CORE]
