#!/usr/bin/env python3
"""Build OSp from original sources, without a host C library or OS loader."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
SECTOR_SIZE = 512
IMAGE_SIZE = 1_474_560
MAX_KERNEL_SECTORS = 64


def tool(variable, candidates):
    override = os.environ.get(variable)
    for candidate in [override] if override else candidates:
        executable = shutil.which(candidate)
        if executable:
            return executable
    choices = ', '.join(candidates)
    raise RuntimeError(f'Missing {choices}; install the build tools or set {variable}.')


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def build(destination):
    nasm = tool('NASM', ['nasm'])
    clang = tool('CLANG', ['clang'])
    linker = tool('LD_LLD', ['ld.lld'])
    objcopy = tool('OBJCOPY', ['llvm-objcopy', 'objcopy'])
    destination.mkdir(parents=True, exist_ok=True)

    for source in ['entry', 'interrupts']:
        run(nasm, '-f', 'elf32', '-g', '-F', 'dwarf',
            ROOT / 'kernel' / f'{source}.asm', '-o', destination / f'{source}.o')

    run(clang, '--target=i686-none-elf', '-std=c11', '-ffreestanding',
        '-fno-pic', '-fno-pie', '-fno-stack-protector', '-fno-builtin',
        '-mno-sse', '-mno-mmx', '-msoft-float', '-Wall', '-Wextra', '-Werror',
        '-Os', '-g', '-c', ROOT / 'kernel/kernel.c', '-o', destination / 'kernel.o')
    run(linker, '-m', 'elf_i386', '-T', ROOT / 'kernel/linker.ld',
        '-Map', destination / 'kernel.map', '-o', destination / 'kernel.elf',
        destination / 'entry.o', destination / 'interrupts.o', destination / 'kernel.o')
    run(objcopy, '-O', 'binary', destination / 'kernel.elf', destination / 'kernel.bin')

    kernel = (destination / 'kernel.bin').read_bytes()
    sectors = (len(kernel) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if not 1 <= sectors <= MAX_KERNEL_SECTORS:
        raise RuntimeError(f'Kernel uses {sectors} sectors; loader limit is {MAX_KERNEL_SECTORS}.')
    run(nasm, '-f', 'bin', f'-DKERNEL_SECTORS={sectors}',
        ROOT / 'boot/boot.asm', '-o', destination / 'boot.bin')
    boot = (destination / 'boot.bin').read_bytes()
    if len(boot) != SECTOR_SIZE or boot[-2:] != b'\x55\xaa':
        raise RuntimeError('Boot sector size or BIOS signature is invalid.')

    image = boot + kernel
    image += bytes(IMAGE_SIZE - len(image))
    image_path = destination / 'osp.img'
    image_path.write_bytes(image)
    manifest = {
        'image': image_path.name,
        'image_bytes': len(image),
        'kernel_bytes': len(kernel),
        'kernel_sectors': sectors,
        'kernel_load_address': '0x10000',
        'sha256': hashlib.sha256(image).hexdigest(),
    }
    (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Built {image_path} ({len(kernel)} kernel bytes, {sectors} sectors)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build')
    args = parser.parse_args()
    try:
        build(args.output.resolve())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f'Build failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
