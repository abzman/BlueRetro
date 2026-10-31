''' pytest session stuff. '''
import logging
import os
import time
import pytest
from injector import BlueRetroInjector
from device_data.br import system, dev_mode, bt_conn_type

# How long the DUT gets to boot and bring up its websocket server.
BOOT_TIMEOUT = 180

# QEMU's first serial port, as redirected by the workflow. When it is there,
# every test is checked against it for a firmware panic.
SERIAL_LOG = os.environ.get('BLUERETRO_SERIAL_LOG', 'serial_log.txt')
PANIC_MARKERS = (b'Guru Meditation Error', b'WDT_SYS_RESET', b'RTCWDT_RTC_RESET')


class BlueRetroDut(BlueRetroInjector):
    ''' BlueRetro injector with a few extra for pytest. '''
    pass


class SerialLogWatcher:
    ''' Scan the DUT serial log for panics, only reading what is new. '''
    def __init__(self, path):
        self.path = path
        self.offset = 0
        # A marker split across two reads must still match.
        self.carry = max(len(m) for m in PANIC_MARKERS)

    def new_panic(self):
        ''' Return the first panic line logged since the last call, or None. '''
        start = max(self.offset - self.carry, 0)
        try:
            with open(self.path, 'rb') as f:
                f.seek(start)
                data = f.read()
        except FileNotFoundError:
            return None
        self.offset = start + len(data)
        for marker in PANIC_MARKERS:
            idx = data.find(marker)
            if idx >= 0:
                line_start = data.rfind(b'\n', 0, idx) + 1
                return data[line_start:data.find(b'\n', idx)].decode(errors='replace').strip()
        return None


@pytest.fixture(scope="session")
def serial_log():
    ''' Watcher over the DUT serial log. '''
    return SerialLogWatcher(SERIAL_LOG)


@pytest.fixture(autouse=True)
def fail_on_panic(serial_log):
    ''' Stop the session on a firmware panic.

    Once the DUT panics it reboots, so every later test would only fail on a
    dead link and bury the one that actually broke it.
    '''
    yield
    panic = serial_log.new_panic()
    if panic:
        pytest.exit(f"DUT panicked, see {SERIAL_LOG}: {panic}", returncode=1)


@pytest.fixture(scope="session")
def blueretro_dut(serial_log):
    ''' Fixture that try to return a BlueRetroDut object. '''
    logging.StreamHandler.terminator = ""
    deadline = time.monotonic() + BOOT_TIMEOUT
    while True:
        try:
            dut = BlueRetroDut()
            break
        except Exception as e:
            logging.debug(e)
            panic = serial_log.new_panic()
            if panic:
                pytest.exit(f"DUT panicked during boot, see {SERIAL_LOG}: {panic}", returncode=1)
            if time.monotonic() > deadline:
                pytest.exit(f"DUT websocket not up after {BOOT_TIMEOUT}s: {e}", returncode=1)
            time.sleep(1)
    yield dut

    # Best effort: the coverage dump is a bonus, not part of the verdict.
    try:
        dut.ensure_connected()
        dut.send_cov_dump()
    except Exception as e:
        logging.warning("coverage dump failed: %s", e)

@pytest.fixture()
def blueretro(blueretro_dut, request):
    ''' Create and teardown a BT device on BlueRetro DUT. '''
    blueretro_dut.ensure_connected()
    blueretro_dut.disconnect()

    param = getattr(request, 'param', [system.GC, dev_mode.PAD, bt_conn_type.BT_BR_EDR])
    # The output's 2 bytes, then its keyboard's 3: full layout, US, Mac
    blueretro_dut.send_out_cfg(0, f"{param[1]:02x}00000000")
    blueretro_dut.send_system_id(param[0])
    rsp = blueretro_dut.connect(param[2])
    assert rsp['device_conn']['handle'] == 0
    assert rsp['device_conn']['device_id'] == 0
    assert rsp['device_conn']['device_type'] == 0

    yield blueretro_dut

    rsp = blueretro_dut.disconnect()
    assert rsp['device_disconn']['handle'] == 0
    assert rsp['device_disconn']['device_id'] == 0
