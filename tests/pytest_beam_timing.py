''' The Super Scope's timing, on the host: main/wired/beam_timing.c (where the beam is, from the
polls) fed by a model of a SNES (beam_timing_sim.c), and the scope's report byte. Nothing in QEMU
runs a wired driver, so this is what checks them before a console does. '''
import os
import re
import shutil
import subprocess
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Dots off the beam: drift of the aim, all crystal and interrupt jitter included, 10 s after
# lock; and the most it moves from one frame to the next
MAX_WANDER = 2.5
MAX_JUMP = 2.5
LOCK_FRAMES = 200       # ~3 s


@pytest.fixture(scope='module')
def sim(tmp_path_factory):
    cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc:
        pytest.skip('no host C compiler (CI installs one)')
    exe = str(tmp_path_factory.mktemp('beam') / 'beam_timing_sim')
    subprocess.run([cc, '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', os.path.join(ROOT, 'main', 'wired'),
                    '-I', os.path.join(ROOT, 'main', 'adapter', 'wired'),
                    '-o', exe, os.path.join(HERE, 'beam_timing_sim.c'),
                    os.path.join(ROOT, 'main', 'wired', 'beam_timing.c'), '-lm'], check=True)
    return exe


def run(sim, scenario):
    out = subprocess.run([sim, scenario], capture_output=True, text=True, check=True).stdout
    res = {}
    for line in out.splitlines():
        res.update({k: float(v) for k, v in re.findall(r'(\w+)=(-?[\d.]+)', line)})
    return res


@pytest.mark.parametrize('scenario', ['ntsc_auto', 'ntsc_auto_real', 'ntsc_auto_slow_cpu',
                                      'pal_auto_real', 'ntsc_manual_10'])
def test_beam_timing_tracks_the_beam(sim, scenario):
    ''' Auto-read on its 256-clock grid (NTSC and PAL) or a game's own reads, crystals 50 ppm
    apart, interrupt jitter and lag frames: locked within ~3 s, then the aim stays within a
    couple of dots of the beam. '''
    r = run(sim, scenario)
    assert 0 < r['locked_at'] <= LOCK_FRAMES
    assert r['settled_below'] <= MAX_WANDER and r['settled_above'] <= MAX_WANDER, r
    assert r['max_jump'] <= MAX_JUMP, r
    assert r['resets'] == 0


def test_beam_timing_console_reset(sim):
    ''' A console reset moves the frame: the tracker starts over and locks again. '''
    r = run(sim, 'ntsc_reset')
    assert r['resets'] == 1
    assert 5000 < r['relock_at'] <= 5000 + LOCK_FRAMES
    assert r['settled_below'] <= MAX_WANDER and r['settled_above'] <= MAX_WANDER, r


def test_super_scope_report(sim):
    ''' Fire single shot or repeating with turbo, Cursor held, Pause and the turbo switch on press,
    the null flag off the screen. '''
    out = subprocess.run([sim, 'report'], capture_output=True, text=True).stdout
    assert out.strip().endswith('report ok'), out
