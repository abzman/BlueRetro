''' Tests Xbox-style Classic HID reports when the report descriptor is absent. '''
import struct
import pytest
from bit_helper import bit
from device_data.br import pad


DEVICE_NAME = 'Xbox Wireless Controller'


def make_report(dpad=0, buttons=0, extra=0, axes=(0x8000, 0x7FFF, 0x8000, 0x7FFF),
                triggers=(0, 0)):
    ''' Build the 15-byte report body observed from the Nova Lite 2. '''
    body = struct.pack('<HHHHHH', *axes, *triggers) + bytes([dpad, buttons, extra])
    return 'a101' + body.hex()


@pytest.mark.parametrize(('dpad', 'expected'), [
    (1, bit(pad.LD_UP)),
    (3, bit(pad.LD_RIGHT)),
    (5, bit(pad.LD_DOWN)),
    (7, bit(pad.LD_LEFT)),
])
def test_xbox_classic_dpad_without_descriptor(blueretro, dpad, expected):
    ''' Decode the captured discrete D-pad values without an SDP descriptor. '''
    blueretro.send_name(DEVICE_NAME)
    neutral = make_report()
    blueretro.send_hid_report(neutral)
    blueretro.send_hid_report(neutral)

    rsp = blueretro.send_hid_report(make_report(dpad=dpad))

    assert rsp['wireless_input']['hat'] == dpad
    assert rsp['generic_input']['btns'][0] == expected


@pytest.mark.parametrize(('buttons', 'extra', 'expected'), [
    (0x01, 0x00, bit(pad.RB_DOWN)),
    (0x02, 0x00, bit(pad.RB_RIGHT)),
    (0x04, 0x00, bit(pad.RB_LEFT)),
    (0x08, 0x00, bit(pad.RB_UP)),
    (0x10, 0x00, bit(pad.LS)),
    (0x20, 0x00, bit(pad.RS)),
    (0x40, 0x00, bit(pad.MS)),
    (0x80, 0x00, bit(pad.MM)),
    (0x00, 0x01, bit(pad.LJ)),
    (0x00, 0x02, bit(pad.RJ)),
    (0x00, 0x10, bit(pad.MT)),
])
def test_xbox_classic_buttons_without_descriptor(blueretro, buttons, extra, expected):
    ''' Map captured face, shoulder, menu, and stick-click bits. '''
    blueretro.send_name(DEVICE_NAME)
    neutral = make_report()
    blueretro.send_hid_report(neutral)
    blueretro.send_hid_report(neutral)

    rsp = blueretro.send_hid_report(make_report(buttons=buttons, extra=extra))

    assert rsp['generic_input']['btns'][0] == expected


def test_xbox_classic_axes_and_triggers_without_descriptor(blueretro):
    ''' Decode the captured 16-bit axes and 10-bit trigger values. '''
    blueretro.send_name(DEVICE_NAME)
    neutral = make_report()
    blueretro.send_hid_report(neutral)
    blueretro.send_hid_report(neutral)

    rsp = blueretro.send_hid_report(make_report(
        axes=(0xFFFF, 0x0000, 0x0000, 0xFFFF),
        triggers=(1023, 512),
    ))

    assert rsp['wireless_input']['axes'] == [0xFFFF, 0x0000, 0x0000, 0xFFFF]
    assert rsp['wireless_input']['triggers'] == [1023, 512]
    assert rsp['generic_input']['axes'] == [32767, -32767, -32768, 32768, 1023, 512]


def test_xbox_classic_rumble_without_descriptor(blueretro):
    ''' Console rumble goes out as the Xbox's output report 0x03: all motors enabled, the strong
    and weak motors at full, then off. '''
    blueretro.send_name(DEVICE_NAME)
    for _ in range(3):
        blueretro.send_hid_report(make_report())

    rsp = blueretro.send_fb(0, 1, '01')
    assert rsp['fb_changed'] == 1
    # After the L2CAP header: HIDP output, report 0x03, enable mask, LT, RT, strong, weak
    assert rsp['bt_tx0'][9:16] == [0xA2, 0x03, 0x0F, 0, 0, 255, 255]
    rsp = blueretro.send_fb(0, 1, '00')
    assert rsp['bt_tx0'][9:16] == [0xA2, 0x03, 0x0F, 0, 0, 0, 0]


def test_xbox_classic_after_descriptor_device(blueretro):
    ''' A slot a device with a descriptor used before still decodes the descriptor-less layout:
    the previous device's descriptor goes when the slot is reset. '''
    from pytest_hid_controller_a import HID_DESC
    blueretro.send_name('HID Generic')
    blueretro.send_hid_desc(HID_DESC)
    blueretro.disconnect()
    blueretro.connect()

    blueretro.send_name(DEVICE_NAME)
    for _ in range(2):
        blueretro.send_hid_report(make_report())
    rsp = blueretro.send_hid_report(make_report(buttons=0x01))
    assert rsp['wireless_input']['btns'] == 1
    assert rsp['generic_input']['btns'][0] == bit(pad.RB_DOWN)
