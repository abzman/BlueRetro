#!/usr/bin/env python3
''' How much IRAM and DRAM a firmware ELF leaves free, from its section headers.

The ESP32 runs the wired drivers from IRAM, and most of the fork's additions land there; the linker
only says something once it overflows. This reports the margin of every build and fails one that
falls under --min-iram, so a change that eats the last few KB of a board is caught before it no
longer links (or links, then misses a console's timing because code moved to flash).

    python3 tools/mem_report.py build/BlueRetro*.elf --name hw1_n64 --min-iram 2048 [--summary FILE]
'''
import argparse
import os
import subprocess
import sys

IRAM_END = 0x400A0000       # SRAM0: the 192 KB of IRAM the app may use, after the caches
DRAM_START = 0x3FFB0000     # SRAM2 + SRAM1 DRAM, what static data and the heap share
DRAM_END = 0x40000000


def sections(elf, objdump):
    out = subprocess.run([objdump, '-h', elf], capture_output=True, text=True, check=True).stdout
    secs = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and p[0].isdigit():
            secs[p[1]] = (int(p[2], 16), int(p[3], 16))
    return secs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--name', default=None)
    ap.add_argument('--min-iram', type=int, default=0, help='fail under this many free IRAM bytes')
    ap.add_argument('--objdump', default=os.environ.get('OBJDUMP', 'xtensa-esp32-elf-objdump'))
    ap.add_argument('--summary', default=os.environ.get('GITHUB_STEP_SUMMARY'))
    args = ap.parse_args()

    secs = sections(args.elf, args.objdump)
    size, addr = secs['.iram0.text_end'] if '.iram0.text_end' in secs else (0, sum(secs['.iram0.text']))
    iram_free = IRAM_END - (addr + size)
    iram_text = secs['.iram0.text'][0]
    dram_static = sum(secs[s][0] for s in ('.dram0.data', '.dram0.bss', '.noinit') if s in secs)
    flash_text = secs.get('.flash.text', (0, 0))[0]
    name = args.name or os.path.basename(args.elf)

    ok = iram_free >= args.min_iram
    line = (f'| {name} | {iram_free} | {iram_text} | {dram_static} | {flash_text} | '
            f'{"ok" if ok else f"under {args.min_iram}"} |')
    print(f'{name}: IRAM free {iram_free} B (.iram0.text {iram_text} B), static DRAM {dram_static} B, '
          f'.flash.text {flash_text} B')
    if args.summary:
        new = not os.path.exists(args.summary) or os.path.getsize(args.summary) == 0
        with open(args.summary, 'a') as f:
            if new:
                f.write('| build | IRAM free (B) | .iram0.text (B) | static DRAM (B) | .flash.text (B) | margin |\n')
                f.write('|---|---|---|---|---|---|\n')
            f.write(line + '\n')
    if not ok:
        print(f'::error::{name}: only {iram_free} bytes of IRAM left, under the {args.min_iram} floor')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
