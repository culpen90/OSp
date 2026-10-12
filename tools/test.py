#!/usr/bin/env python3
"""Boot the actual image and exercise real PS/2 input and VGA output in QEMU."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build'


class Machine:
    def __init__(self, qemu, image, directory, memory='32M'):
        self.directory = Path(directory)
        self.log = self.directory / 'console.log'
        self.socket_path = self.directory / 'qmp.sock'
        self.stderr = (self.directory / 'qemu.log').open('wb')
        self.process = subprocess.Popen([
            qemu, '-machine', 'pc', '-accel', 'tcg', '-m', memory,
            '-drive', f'file={image},format=raw,if=floppy,readonly=on',
            '-boot', 'order=a', '-nic', 'none', '-display', 'none',
            '-serial', 'none', '-monitor', 'none',
            '-debugcon', f'file:{self.log}',
            '-global', 'isa-debugcon.iobase=0xe9',
            '-qmp', f'unix:{self.socket_path},server=on,wait=off',
        ], stdout=subprocess.DEVNULL, stderr=self.stderr)
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(5)
        deadline = time.monotonic() + 10
        while True:
            try:
                self.socket.connect(str(self.socket_path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if self.process.poll() is not None or time.monotonic() >= deadline:
                    self.close()
                    raise RuntimeError('QEMU did not start; check its installed build.')
                time.sleep(0.05)
        self.stream = self.socket.makefile('rb')
        greeting = json.loads(self.stream.readline())
        assert 'QMP' in greeting, greeting
        self.counter = 0
        self.command('qmp_capabilities')

    def command(self, command, arguments=None):
        self.counter += 1
        request = {'execute': command, 'id': self.counter}
        if arguments:
            request['arguments'] = arguments
        self.socket.sendall(json.dumps(request).encode() + b'\n')
        while True:
            line = self.stream.readline()
            if not line:
                raise RuntimeError('QEMU disconnected unexpectedly.')
            response = json.loads(line)
            if response.get('id') == self.counter:
                if 'error' in response:
                    raise RuntimeError(str(response['error']))
                return response.get('return')

    def monitor(self, command):
        return self.command('human-monitor-command', {'command-line': command})

    def output(self):
        return self.log.read_text(errors='replace') if self.log.exists() else ''

    def wait_for(self, text, since=0, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            output = self.output()[since:]
            if text in output:
                return output
            if self.process.poll() is not None:
                break
            time.sleep(0.02)
        raise AssertionError(f'Did not see {text!r}; console tail: {self.output()[-600:]!r}')

    def key(self, key):
        self.monitor(f'sendkey {key} 1')
        # Wait for key release before the next event; also lets IRQ1 run.
        time.sleep(0.025)

    def type(self, text):
        unshifted = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal',
                     ',': 'comma', '.': 'dot', '/': 'slash', ';': 'semicolon',
                     "'": 'apostrophe', '[': 'bracket_left', ']': 'bracket_right',
                     '\\': 'backslash', '`': 'grave_accent'}
        shifted = dict(zip('!@#$%^&*()_+<>?:\"{}|~',
                           ['1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
                            'minus', 'equal', 'comma', 'dot', 'slash', 'semicolon',
                            'apostrophe', 'bracket_left', 'bracket_right',
                            'backslash', 'grave_accent']))
        for char in text:
            if char in unshifted:
                key = unshifted[char]
            elif char in shifted:
                key = 'shift-' + shifted[char]
            elif 'A' <= char <= 'Z':
                key = 'shift-' + char.lower()
            elif char.isascii() and char.isalnum():
                key = char
            else:
                raise ValueError(f'Unsupported test character: {char!r}')
            self.key(key)

    def shell(self, command, expected=None):
        start = len(self.output())
        self.type(command + '\n')
        output = self.wait_for('osp> ', since=start)
        if expected is not None:
            assert expected in output, (expected, output)
        return output

    def screen(self):
        path = self.directory / 'vga.bin'
        result = self.monitor(f'pmemsave 0xb8000 4000 "{path}"')
        assert not result, result
        data = path.read_bytes()
        assert len(data) == 4000
        rows = [''.join(chr(c) for c in data[row * 160:(row + 1) * 160:2])
                for row in range(25)]
        return rows, data

    def close(self):
        if hasattr(self, 'stream'):
            self.stream.close()
        if hasattr(self, 'socket'):
            self.socket.close()
        if hasattr(self, 'process') and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if hasattr(self, 'stderr'):
            self.stderr.close()


def assert_image():
    image = (BUILD / 'osp.img').read_bytes()
    kernel = (BUILD / 'kernel.bin').read_bytes()
    manifest = json.loads((BUILD / 'manifest.json').read_text())
    assert len(image) == 1_474_560
    assert image[510:512] == b'\x55\xaa'
    assert image[512:512 + len(kernel)] == kernel
    assert manifest['kernel_sectors'] == (len(kernel) + 511) // 512
    assert not any(image[512 + len(kernel):])
    print('PASS boot image layout and signature', flush=True)


def exercise(machine):
    machine.wait_for('osp> ', timeout=15)
    rows, _ = machine.screen()
    assert 'OSp' in '\n'.join(rows) and 'osp> ' in '\n'.join(rows)
    registers = machine.monitor('info registers')
    cr0 = re.search(r'CR0=([0-9a-fA-F]+)', registers)
    assert cr0 and int(cr0.group(1), 16) & 1, registers
    print('PASS boot, VGA banner, protected mode', flush=True)

    machine.shell('help', 'uptime')
    machine.shell('about', 'OSp')
    machine.shell('echo Hello, OSp! []{} _+-=?', 'Hello, OSp! []{} _+-=?')
    rows, _ = machine.screen()
    assert 'Hello, OSp! []{} _+-=?' in '\n'.join(rows)
    output = machine.shell('missingcommand')
    assert 'unknown' in output.lower(), output
    output = machine.shell('missingcommand arguments')
    assert 'unknown' in output.lower(), output
    machine.shell('   echo spaced   ', 'spaced')
    machine.shell('')
    print('PASS commands, Shift symbols, whitespace and unknown input', flush=True)

    machine.key('backspace')
    machine.key('left')
    machine.key('up')
    start = len(machine.output())
    machine.type('echo correctx')
    machine.key('backspace')
    machine.type('\n')
    output = machine.wait_for('osp> ', since=start)
    assert '\ncorrect\n' in output, output

    machine.shell('clear')
    start = len(machine.output())
    machine.type('echo ' + 'x' * 78)
    for _ in range(5):
        machine.key('backspace')
    machine.type('\n')
    output = machine.wait_for('osp> ', since=start)
    assert '\n' + 'x' * 73 + '\n' in output, output
    rows, _ = machine.screen()
    assert sum(row.count('x') for row in rows) == 146, rows

    output = machine.shell('echo ' + 'a' * 150)
    assert '\n' + 'a' * 122 + '\n' in output, output
    machine.shell('echo recovered', '\nrecovered\n')
    start = len(machine.output())
    machine.type('x' * 110)
    machine.key('ctrl-u')
    machine.type('echo cleared\n')
    output = machine.wait_for('osp> ', since=start)
    assert '\ncleared\n' in output, output
    start = len(machine.output())
    machine.type('echo ')
    machine.key('caps_lock')
    machine.type('abc')
    for key in ('d', 'e', 'f'):
        machine.key('shift-' + key)
    machine.key('caps_lock')
    machine.type('\n')
    output = machine.wait_for('osp> ', since=start)
    assert '\nABCdef\n' in output, output
    print('PASS empty editing, extended keys, wrapped backspace, input limit', flush=True)
    print('PASS Ctrl+U and Caps Lock with Shift', flush=True)

    first = machine.shell('uptime')
    time.sleep(1.1)
    second = machine.shell('uptime')
    values = [re.search(r'(\d+)\s+(?:seconds|second|s\b)', item) for item in (first, second)]
    assert all(values), (first, second)
    assert int(values[1].group(1)) > int(values[0].group(1)), (first, second)

    for index in range(16):
        machine.shell(f'echo line{index}')
    rows, _ = machine.screen()
    assert 'line15' in '\n'.join(rows) and 'OSp' not in '\n'.join(rows), rows
    machine.shell('color a 0')
    machine.shell('echo colored', '\ncolored\n')
    rows, data = machine.screen()
    joined = ''.join(rows)
    index = joined.rfind('colored')
    assert index >= 0 and data[index * 2 + 1] == 0x0a
    output = machine.shell('color 0 0')
    assert any(word in output.lower() for word in ('different', 'usage', 'invalid', 'readable')), output
    for arguments in ('', 'a', 'aa 0', '7 z', '1 2 3'):
        machine.shell('color ' + arguments, 'Usage:')
    machine.shell('color 0 f')
    _, data = machine.screen()
    assert all(attribute == 0xf0 for attribute in data[1::2])
    machine.shell('color 7 0')
    machine.shell('clear')
    rows, _ = machine.screen()
    assert rows[0].startswith('osp> ') and not ''.join(rows[1:]).strip(), rows
    print('PASS live timer, scrolling, VGA colors, clear', flush=True)

    start = len(machine.output())
    machine.type('reboot\n')
    machine.wait_for('OSp', since=start, timeout=15)
    machine.wait_for('osp> ', since=start, timeout=15)
    machine.shell('echo rebooted', '\nrebooted\n')
    machine.shell('clear')
    machine.shell('about', 'OSp')
    machine.shell('help', 'uptime')
    machine.command('screendump', {'filename': str(BUILD / 'osp-screen.png'), 'format': 'png'})
    print('PASS reboot and keyboard after restart; screenshot saved', flush=True)

    start = len(machine.output())
    machine.type('halt\n')
    machine.wait_for('halt', since=start)
    time.sleep(0.1)
    halted = machine.output()
    machine.type('echo shouldneverrun\n')
    time.sleep(0.1)
    assert machine.output() == halted and 'osp> ' not in halted[start:]
    registers = machine.monitor('info registers')
    assert 'HLT=1' in registers, registers
    (BUILD / 'test-console.log').write_text(machine.output())
    print('PASS halt disables interrupts and stops input', flush=True)


def track_boundary_test(qemu, temporary):
    # Increase the sector count to cross both a head and cylinder boundary.
    # A sentinel at the end proves that the loader actually read those sectors.
    sectors = 64
    boot_path = temporary / 'wide-boot.bin'
    boot_source = temporary / 'wide-boot.asm'
    # Larger kernel BSS can cover the final disk sector. Preserve the sentinel
    # before _start zeroes BSS, in unused low memory clear of BIOS/boot storage.
    address = 0x10000 + sectors * 512 - 4
    transfer = '    jmp 0x08:0x10000'
    source = (ROOT / 'boot/boot.asm').read_text()
    assert source.count(transfer) == 1
    boot_source.write_text(source.replace(transfer,
        f'    mov eax, [0x{address:x}]\n    mov [0x9000], eax\n{transfer}'))
    nasm = shutil.which(os.environ.get('NASM', 'nasm'))
    assert nasm, 'nasm is required'
    subprocess.run([nasm, '-f', 'bin', f'-DKERNEL_SECTORS={sectors}',
                    str(boot_source), '-o', str(boot_path)], check=True)
    kernel = (BUILD / 'kernel.bin').read_bytes()
    payload = kernel + bytes(sectors * 512 - len(kernel) - 4) + b'OSp!'
    image = boot_path.read_bytes() + payload
    wide_image = temporary / 'wide.img'
    wide_image.write_bytes(image + bytes(1_474_560 - len(image)))
    directory = temporary / 'wide'
    directory.mkdir()
    machine = Machine(qemu, wide_image, directory, memory='16M')
    try:
        machine.wait_for('osp> ', timeout=15)
        sentinel = directory / 'sentinel.bin'
        machine.monitor(f'pmemsave 0x9000 4 "{sentinel}"')
        assert sentinel.read_bytes() == b'OSp!'
        machine.shell('echo trackpass', '\ntrackpass\n')
        print('PASS disk loading across head/cylinder boundaries with 16 MiB RAM', flush=True)
        start = len(machine.output())
        machine.command('inject-nmi')
        output = machine.wait_for('error=0x00000000\n', since=start)
        assert 'interrupt 2 (non-maskable interrupt)' in output, output
        time.sleep(0.1)
        assert 'HLT=1' in machine.monitor('info registers')
        assert 'osp> ' not in machine.output()[start:]
        print('PASS CPU exception dispatch and diagnostic halt', flush=True)
    finally:
        machine.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default='qemu-system-i386')
    args = parser.parse_args()
    qemu = shutil.which(args.qemu)
    if not qemu:
        print(f'Missing {args.qemu}; install QEMU to run emulator tests.', file=sys.stderr)
        return 1
    machine = None
    try:
        assert_image()
        with tempfile.TemporaryDirectory(prefix='osp-test-') as directory:
            temporary = Path(directory)
            main_dir = temporary / 'main'
            main_dir.mkdir()
            machine = Machine(qemu, BUILD / 'osp.img', main_dir)
            try:
                exercise(machine)
            finally:
                (BUILD / 'test-console.log').write_text(machine.output())
                machine.close()
            track_boundary_test(qemu, temporary)
    except (AssertionError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL {error}', file=sys.stderr)
        return 1
    print('All emulator checks passed.', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
