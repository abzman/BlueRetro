#!/usr/bin/env python3
''' Feed the same reports through every wired system and output mode for the HID devices the
tests describe, and record what comes out: the decoded input, the mapped input and the wired
output. Recorded once from upstream into golden/, then compared against by
pytest_upstream_golden.py, so a change to a controller or console nobody meant to touch shows up.

The golden file comes from upstream (barrenechea 7c77f06) built with configs/dbg/qemu minus
CONFIG_BLUERETRO_COVERAGE (the counters leave too little heap for ~1000 connections), plus
golden/upstream_7c77f06_qemu.patch, which only lets it survive the run: the HID parser and keyboard
monitor leaks fixed here, and sys_mgr_cmd ignored under QEMU. To record it again:

    git worktree add /tmp/up 7c77f06 && cd /tmp/up && git apply <this repo>/tests/golden/upstream_7c77f06_qemu.patch
    grep -v COVERAGE <this repo>/configs/dbg/qemu > sdkconfig && idf.py build    # then start QEMU as CI does
    python3 <this repo>/tests/upstream_diff.py --record <this repo>/tests/golden/upstream_7c77f06.json.gz
'''
import argparse
import gzip
import json
import random
import sys
from device_data.br import system, dev_mode, bt_conn_type

# Every descriptor the tests carry, with the name it was captured under
DEVICES = [
    ('pytest_hid_controller_a', 'HID_DESC', 'HID Generic'),
    ('pytest_hid_controller_b', 'HID_DESC', 'HID Generic'),
    ('pytest_hid_keyboard', 'HID_DESC', 'HID Keyboard'),
    ('pytest_hid_keyboard', 'NKRO_DESC', 'HID Keyboard'),
    ('pytest_n64_mouse', 'HID_DESC', 'HID Mouse'),
    ('pytest_atari_vcs_classic', 'HID_DESC', None),
    ('pytest_atari_vcs_modern', 'HID_DESC', None),
    ('pytest_flydigi_apex_3', 'HID_DESC', None),
    ('pytest_google_stadia', 'HID_DESC', None),
    ('pytest_xbox_ble_controller', 'HID_DESC', None),
    ('pytest_xiaomi_mi', 'HID_DESC', None),
]
SYSTEMS = [s for s in system if s != system.WIRED_AUTO]
MODES = list(dev_mode)
KEPT = ('wireless_input', 'generic_input', 'mapped_input', 'wired_output')
REPORTS_PER_DEVICE = 6


# Generic Home and Capture. Their releases switch a Saturn, PC Engine, Genesis, 3DO or Dreamcast
# port's mode (a wired reset) and N64 accessories, and in combos reboot or factory reset the
# adapter. Select is left in: the one thing it switches here is the GameCube port's names/positions
# mapping, which the golden test puts back to its default before and after each device.
SPECIAL_BITS = (1 << 22) | (1 << 23)


def input_reports(hid_reports):
    ''' Each input report in a parsed descriptor: (report ID, length in bytes, usages). '''
    out = []
    for rep in hid_reports:
        if rep['report_tag'] != 0 or rep.get('report_type', -1) < 0 or not rep.get('usages'):
            continue
        bits = max(u['bit_offset'] + u['bit_size'] for u in rep['usages'])
        out.append((rep['report_id'], (bits + 7) // 8, rep['usages']))
    return out


def set_field(data, offset, size, value):
    for i in range(size):
        byte, bit = divmod(offset + i, 8)
        if value >> i & 1:
            data[byte] |= 1 << bit
        else:
            data[byte] &= ~(1 << bit)


def presses(usages):
    ''' Everything in a report that is a button: one bit of a button bitfield, or one key code
    in a keyboard's key array slot. As (offset, size, value). '''
    out = []
    for u in usages:
        if u['usage_page'] == 7 and u['bit_size'] == 8 and u['usage'] < 0xE0:
            out += [(u['bit_offset'], 8, key) for key in range(4, 0x64)]
        elif u['usage_page'] in (7, 9, 12) or (u['usage_page'] == 1 and u['bit_size'] == 1):
            out += [(u['bit_offset'] + i, 1, 1) for i in range(u['bit_size'])]
    return out


def is_value(u):
    return not (u['usage_page'] in (7, 9, 12) or (u['usage_page'] == 1 and u['bit_size'] == 1))


def report(rid, length, fields):
    data = bytearray(length)
    for offset, size, value in fields:
        set_field(data, offset, size, value)
    return f'a1{rid:02x}' + data.hex()


def safe_presses(dut, rid, length, usages):
    ''' The presses that reach none of SPECIAL_BITS (nor the 4th, BR combo, word), as decoded
    on the GameCube output the device is connected to. '''
    safe = []
    for press in presses(usages):
        rsp = dut.send_hid_report(report(rid, length, [press]))
        btns = rsp.get('generic_input', {}).get('btns', [0, 0, 0, 0])
        if not btns[0] & SPECIAL_BITS and not btns[3]:
            safe.append(press)
        dut.send_hid_report(report(rid, length, []))
    return safe


def report_payloads(rid, length, usages, safe, rng):
    ''' Two neutral reports, then random ones: random values in the axes and hats, and at most one
    button held. '''
    payloads = [report(rid, length, [])] * 2
    values = [u for u in usages if is_value(u)]
    for _ in range(REPORTS_PER_DEVICE):
        fields = [(u['bit_offset'], u['bit_size'], rng.getrandbits(u['bit_size'])) for u in values]
        if safe and rng.random() < 0.8:
            fields.append(rng.choice(safe))
        payloads.append(report(rid, length, fields))
    return payloads


def connect(dut, sys_id, mode, name, desc):
    dut.ensure_connected()
    dut.disconnect()
    dut.send_out_cfg(0, f'{mode:02x}00000000')
    dut.send_system_id(sys_id)
    dut.connect(bt_conn_type.BT_BR_EDR)
    dut.send_name(name)
    return dut.send_hid_desc(desc).get('hid_reports', [])


def device_name(module, name):
    return name or getattr(module, 'DEVICE_NAME', 'HID Generic')


# The GameCube port's face buttons follow their positions by default; releasing Select switches
# them to their names and back, and that outlives the connection. Select is pressed in the runs below
# (and in other tests), so each device starts from the default and leaves the port there.
# Probed with the HID Generic pad of pytest_hid_controller_a: its 2nd button (generic RB_RIGHT) is
# GC btns 0x8024 by position; its 11th is Select.
GC_PROBE_NEUTRAL = 'a101808000000000'
GC_PROBE_RB_RIGHT = 'a101808002000000'
GC_PROBE_SELECT = 'a101808000040000'
GC_RB_RIGHT_BY_POSITION = 0x8024


def gc_mapping_to_default(dut):
    import pytest_hid_controller_a
    connect(dut, system.GC, dev_mode.PAD, 'HID Generic', pytest_hid_controller_a.HID_DESC)
    for report in (GC_PROBE_NEUTRAL, GC_PROBE_RB_RIGHT, GC_PROBE_NEUTRAL):
        rsp = dut.send_hid_report(report)
    if rsp['wired_output']['btns'] != 0x8020:
        raise RuntimeError(f'GC probe: neutral is {rsp}')
    rsp = dut.send_hid_report(GC_PROBE_RB_RIGHT)
    dut.send_hid_report(GC_PROBE_NEUTRAL)
    if rsp['wired_output']['btns'] != GC_RB_RIGHT_BY_POSITION:
        dut.send_hid_report(GC_PROBE_SELECT)
        dut.send_hid_report(GC_PROBE_NEUTRAL)
        rsp = dut.send_hid_report(GC_PROBE_RB_RIGHT)
        dut.send_hid_report(GC_PROBE_NEUTRAL)
        if rsp['wired_output']['btns'] != GC_RB_RIGHT_BY_POSITION:
            raise RuntimeError(f'GC probe: mapping did not switch back: {rsp}')
    dut.disconnect()


def run_device(dut, entry):
    ''' One device through every system and mode: a list of (key, [response subset per report]). '''
    mod_name, desc_name, name = entry
    module = __import__(mod_name)
    desc = getattr(module, desc_name)
    name = device_name(module, name)
    gc_mapping_to_default(dut)
    parsed = connect(dut, system.GC, dev_mode.PAD, name, desc)
    reports = [(rid, length, usages, safe_presses(dut, rid, length, usages))
               for rid, length, usages in input_reports(parsed)]
    records = []
    for sys_id in SYSTEMS:
        for mode in MODES:
            key = f'{mod_name}.{desc_name}/{sys_id.name}/{mode.name}'
            rng = random.Random(key)
            connect(dut, sys_id, mode, name, desc)
            steps = []
            for rid, length, usages, safe in reports:
                for payload in report_payloads(rid, length, usages, safe, rng):
                    rsp = dut.send_hid_report(payload)
                    steps.append({k: rsp[k] for k in KEPT if k in rsp})
            records.append((key, steps))
    dut.disconnect()
    gc_mapping_to_default(dut)
    return records


def run(dut):
    ''' Every system, mode and device. '''
    records = []
    for entry in DEVICES:
        records += run_device(dut, entry)
    return records


def main():
    from injector import BlueRetroInjector
    parser = argparse.ArgumentParser()
    parser.add_argument('--record', required=True, help='gzipped JSON file to write')
    args = parser.parse_args()
    dut = BlueRetroInjector()
    records = run(dut)
    with gzip.open(args.record, 'wt') as f:
        json.dump(dict(records), f, separators=(',', ':'), sort_keys=True)
    print(f'{len(records)} combinations, {sum(len(s) for _, s in records)} reports', file=sys.stderr)


if __name__ == '__main__':
    main()
