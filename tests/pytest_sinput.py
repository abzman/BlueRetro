''' adamhleslie's SInput gamepads: told apart by name, then decoded as generic HID from the
descriptor they send, exactly as a generic HID gamepad with that descriptor is. '''
from device_data.br import bt_type
from pytest_hid_controller_a import HID_DESC


# HIDP data, report ID 1 for a descriptor without one: X, Y, then 32 buttons
REPORTS = ['a101' '8080' '01000000', 'a101' '8080' '08000000', 'a101' 'ff00' '00000000', 'a101' '00ff' '00800000']


def outputs(blueretro, name):
    rsp = blueretro.send_name(name)
    blueretro.send_hid_desc(HID_DESC)
    for _ in range(2):
        blueretro.send_hid_report('a101' '8080' '00000000')
    out = [blueretro.send_hid_report(r) for r in REPORTS]
    return rsp['device_name']['device_type'], [(o['generic_input'], o['wired_output']) for o in out]


def test_sinput_detected_and_decoded(blueretro):
    sinput_type, sinput = outputs(blueretro, 'SInput Gamepad')
    assert sinput_type == bt_type.SINPUT
    blueretro.disconnect()
    blueretro.connect()
    generic_type, generic = outputs(blueretro, 'HID Generic')
    assert generic_type == bt_type.HID_GENERIC
    assert sinput == generic
    assert any(g['btns'][0] for g, _ in sinput)
