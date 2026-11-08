''' RobertDaleSmith's 3DO arcade SILLY_CONTROL_PAD (Orbatak's coin and start panel), on the 3DO
output's Keyboard mode. '''
import pytest
from device_data.br import system, dev_mode, bt_conn_type
from pytest_xbox_classic_no_descriptor import make_report


SERVICE, P1_START, P2_START, P2_COIN, P1_COIN = (1 << i for i in range(5))
HAT_UP, HAT_RIGHT, HAT_DOWN, HAT_LEFT = 1, 3, 5, 7


@pytest.mark.parametrize('blueretro', [[system.REAL_3DO, dev_mode.KB, bt_conn_type.BT_BR_EDR]],
                         indirect=True)
def test_3do_silly_pad(blueretro):
    ''' It answers with the silly pad's ID; the D-pad is the start and coin buttons and Start
    the service button. '''
    blueretro.send_name('Xbox Wireless Controller')
    for _ in range(2):
        blueretro.send_hid_report(make_report())

    out = blueretro.send_hid_report(make_report())['wired_output']
    assert out['ids'] == [0xC0, 0x00] and out['btns'] == 0

    for hat, btn in ((HAT_LEFT, P1_START), (HAT_RIGHT, P2_START), (HAT_DOWN, P1_COIN), (HAT_UP, P2_COIN)):
        assert blueretro.send_hid_report(make_report(dpad=hat))['wired_output']['btns'] == btn
    assert blueretro.send_hid_report(make_report(buttons=0x80))['wired_output']['btns'] == SERVICE
    assert blueretro.send_hid_report(make_report())['wired_output']['btns'] == 0
