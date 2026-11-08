''' Every HID descriptor the tests carry, through every wired system and output mode, against what
upstream BlueRetro (barrenechea 7c77f06) did with the same reports: golden/upstream_7c77f06.json.gz,
recorded by upstream_diff.py from a QEMU build of upstream.

Everything this fork did not mean to change must come out the same. What it did mean to change is
listed below with the reason, and is still held to upstream up to the point the change starts. '''
import gzip
import json
import os
import pytest
import upstream_diff
from device_data.br import system, dev_mode

GOLDEN = os.path.join(os.path.dirname(__file__), 'golden', 'upstream_7c77f06.json.gz')

STAGES = ('wireless_input', 'generic_input', 'mapped_input', 'wired_output')

# (system, mode): the first stage that differs on purpose, and why. That stage and the ones after
# it are not compared; the ones before still are.
CHANGED = {
    (system.CDI, dev_mode.PAD_ALT): ('mapped_input',
        'New: CD-i pointer (Wiimote IR / light gun) mode, the pad maps to an absolute pointer'),
    (system.CDI, dev_mode.MOUSE): ('wired_output',
        'Upstream logs no wired output for the CD-i mouse; this fork does (pytest_cdi_pointer.py)'),
    (system.N64, dev_mode.KB): ('mapped_input',
        'New: RANDnet keyboard output'),
    (system.N64, dev_mode.MOUSE): ('mapped_input',
        'Reworked N64 mouse: a relative source keeps isotropic scaling (pytest_n64_mouse.py); '
        'upstream logs no wired output for it'),
    (system.GENESIS, dev_mode.KB): ('mapped_input',
        'New: XBAND keyboard output'),
    (system.REAL_3DO, dev_mode.KB): ('mapped_input',
        'New: 3DO arcade "silly" pad output (pytest_3do_silly_pad.py)'),
}

# Keys this fork adds to a stage's log, dropped before comparing
EXTRA_KEYS = {
    'wired_output': ('raw_axes',),   # Saturn: the bytes the console reads, beside the axes
}


def load_golden():
    if not os.path.exists(GOLDEN):
        pytest.skip(f'no golden file: {GOLDEN}')
    with gzip.open(GOLDEN, 'rt') as f:
        return json.load(f)


def strip(stage, value):
    if isinstance(value, dict):
        return {k: v for k, v in value.items() if k not in EXTRA_KEYS.get(stage, ())}
    return value


def compared_stages(key):
    _, sys_name, mode_name = key.split('/')
    changed = CHANGED.get((system[sys_name], dev_mode[mode_name]))
    if changed is None:
        return STAGES
    return STAGES[:STAGES.index(changed[0])]


def is_skipped_step(up, ours):
    ''' Upstream drops a keyboard's first reports while no key has been pressed yet; this fork
    decodes them, all keys up. Nothing to compare there. '''
    w = ours.get('wireless_input')
    return ('wireless_input' not in up and isinstance(w, dict) and 'keys' in w
            and not any(w['keys']) and not w.get('modifiers'))


def mismatches(golden, records):
    out = []
    for key, steps in records:
        if key not in golden:
            out.append(f'{key}: not in the golden file')
            continue
        up_steps = golden[key]
        if len(up_steps) != len(steps):
            out.append(f'{key}: {len(steps)} reports, upstream {len(up_steps)}')
            continue
        stages = compared_stages(key)
        for i, (up, ours) in enumerate(zip(up_steps, steps)):
            if is_skipped_step(up, ours):
                continue
            for stage in stages:
                a, b = strip(stage, up.get(stage)), strip(stage, ours.get(stage))
                if a != b:
                    out.append(f'{key} report {i} {stage}:\n    upstream {json.dumps(a)}\n    ours     {json.dumps(b)}')
    return out


@pytest.mark.parametrize('entry', upstream_diff.DEVICES, ids=lambda e: f'{e[0]}.{e[1]}')
def test_upstream_golden(blueretro_dut, entry):
    golden = load_golden()
    records = upstream_diff.run_device(blueretro_dut, entry)
    expected = sum(1 for k in golden if k.startswith(f'{entry[0]}.{entry[1]}/'))
    assert len(records) == expected
    found = mismatches(golden, records)
    assert not found, f'{len(found)} differences from upstream, first ones:\n' + '\n'.join(found[:10])
