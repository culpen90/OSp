#!/usr/bin/env python3
"""Exercise the guest's real RTL8139 and protocol stack without internet access.

QEMU's socket backend carries Ethernet frames to the standard-library fixture
below. No host DHCP/DNS/HTTP service, raw socket, root privilege, or third-party
Python package is required. ``--live`` additionally checks QEMU user networking
against example.com, and consequently requires working external connectivity.
"""

import argparse
from dataclasses import dataclass, field
import heapq
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from test import BUILD, Machine


GUEST_MAC = bytes.fromhex('525400123456')
SERVER_MAC = bytes.fromhex('525400aabbcc')
BROADCAST_MAC = b'\xff' * 6
GUEST_IP = socket.inet_aton('10.0.2.15')
SERVER_IP = socket.inet_aton('10.0.2.2')
DHCP_SOURCE_IP = socket.inet_aton('10.0.2.1')
MASK = socket.inet_aton('255.255.255.0')
HTTP_BODY = b'OSp network fixture: segmented TCP reached the guest exactly once.\n'
HTTP_RESPONSE = (b'HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: '
                 + str(len(HTTP_BODY)).encode() + b'\r\nConnection: close\r\n\r\n'
                 + HTTP_BODY)


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    total = sum(struct.unpack(f'!{len(data) // 2}H', data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def ethernet(payload, kind=0x0800, destination=GUEST_MAC):
    frame = destination + SERVER_MAC + struct.pack('!H', kind) + payload
    return frame.ljust(60, b'\0')


def ipv4(payload, protocol, destination=GUEST_IP, source=SERVER_IP, ident=0):
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), ident,
                         0, 64, protocol, 0, source, destination)
    header = header[:10] + struct.pack('!H', checksum(header)) + header[12:]
    return header + payload


def pseudoheader(source, destination, protocol, length):
    return source + destination + struct.pack('!BBH', 0, protocol, length)


def udp(payload, source_port, destination_port, destination=GUEST_IP, source=SERVER_IP):
    header = struct.pack('!HHHH', source_port, destination_port, 8 + len(payload), 0)
    value = checksum(pseudoheader(source, destination, 17, len(header + payload))
                     + header + payload) or 0xffff
    return header[:6] + struct.pack('!H', value) + payload


def tcp(payload, source_port, destination_port, sequence, acknowledgement, flags):
    header = struct.pack('!HHIIBBHHH', source_port, destination_port, sequence,
                         acknowledgement, 5 << 4, flags, 8192, 0, 0)
    value = checksum(pseudoheader(SERVER_IP, GUEST_IP, 6, len(header + payload))
                     + header + payload)
    return header[:16] + struct.pack('!H', value) + header[18:] + payload


def dhcp_options(data):
    result = {}
    position = 0
    while position < len(data):
        kind = data[position]
        position += 1
        if kind == 255:
            break
        if kind == 0:
            continue
        assert position < len(data), 'Guest sent a truncated DHCP option'
        length = data[position]
        position += 1
        assert position + length <= len(data), 'Guest sent an overlong DHCP option'
        result[kind] = data[position:position + length]
        position += length
    return result


@dataclass
class Connection:
    client_port: int
    server_port: int
    receive_next: int
    send_initial: int = 0x3abc0000
    send_next: int = 0x3abc0001
    request: bytearray = field(default_factory=bytearray)
    request_packets: int = 0
    response_sent: bool = False
    peer_closed: bool = False


class EthernetFixture:
    """A deliberately small Ethernet peer with packet/checksum assertions."""

    def __init__(self):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.peer = None
        self.lock = threading.Lock()
        self.stopped = threading.Event()
        self.connected = threading.Event()
        self.thread = threading.Thread(target=self.run, name='osp-ethernet-fixture', daemon=True)
        self.errors = []
        self.counts = {}
        self.requests = []
        self.connections = {}
        self.syns = {}
        self.scheduled = []
        self.schedule_id = 0
        self.packet_id = 0
        self.drop_first_syn = True
        self.drop_first_request = True
        self.lease_seconds = 3600
        self.suppress_dhcp = False
        self.expected_echo_reply = None
        self.thread.start()

    def count(self, name):
        self.counts[name] = self.counts.get(name, 0) + 1

    def assert_healthy(self):
        assert not self.errors, '\n'.join(self.errors)
        assert self.thread.is_alive(), 'Ethernet fixture stopped unexpectedly'

    def send(self, frame, delay=0):
        with self.lock:
            self.schedule_id += 1
            heapq.heappush(self.scheduled, (time.monotonic() + delay, self.schedule_id, frame))

    def send_ip(self, payload, protocol, destination=GUEST_IP, delay=0):
        self.packet_id = (self.packet_id + 1) & 0xffff
        self.send(ethernet(ipv4(payload, protocol, destination, ident=self.packet_id)), delay)

    def flush(self):
        with self.lock:
            while self.scheduled and self.scheduled[0][0] <= time.monotonic():
                _, _, frame = heapq.heappop(self.scheduled)
                self.peer.sendall(struct.pack('!I', len(frame)) + frame)

    def run(self):
        try:
            while not self.stopped.is_set():
                try:
                    self.peer, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
            if self.peer is None:
                return
            self.peer.settimeout(0.01)
            self.connected.set()
            buffer = bytearray()
            while not self.stopped.is_set():
                self.flush()
                try:
                    chunk = self.peer.recv(65536)
                    if not chunk:
                        break
                    buffer.extend(chunk)
                except socket.timeout:
                    continue
                while len(buffer) >= 4:
                    length = struct.unpack('!I', buffer[:4])[0]
                    assert 14 <= length <= 65535, f'Invalid outbound Ethernet length {length}'
                    if len(buffer) < 4 + length:
                        break
                    frame = bytes(buffer[4:4 + length])
                    del buffer[:4 + length]
                    self.receive(frame)
        except Exception as error:
            if not self.stopped.is_set():
                self.errors.append(f'{type(error).__name__}: {error}')

    def receive(self, frame):
        assert frame[6:12] == GUEST_MAC, f'Unexpected guest MAC {frame[6:12].hex()}'
        self.count('ethernet')
        kind = struct.unpack('!H', frame[12:14])[0]
        if kind == 0x0806:
            self.arp(frame[14:])
            return
        if kind != 0x0800:
            return
        packet = frame[14:]
        assert len(packet) >= 20 and packet[0] >> 4 == 4, 'Invalid outbound IPv4 header'
        header_length = (packet[0] & 15) * 4
        length = struct.unpack('!H', packet[2:4])[0]
        assert 20 <= header_length <= length <= len(packet), 'Invalid outbound IPv4 lengths'
        assert checksum(packet[:header_length]) == 0, 'Invalid outbound IPv4 checksum'
        assert struct.unpack('!H', packet[6:8])[0] & 0x3fff == 0, 'Unexpected IP fragmentation'
        source, destination = packet[12:16], packet[16:20]
        payload = packet[header_length:length]
        protocol = packet[9]
        if source != b'\0' * 4:
            assert source == GUEST_IP, f'Unexpected source address {socket.inet_ntoa(source)}'
            expected_mac = BROADCAST_MAC if destination == b'\xff' * 4 else SERVER_MAC
            assert frame[:6] == expected_mac, 'IPv4 route did not use the resolved peer MAC'
        if protocol == 17:
            assert len(payload) >= 8, 'Truncated outbound UDP header'
            source_port, destination_port, udp_length, udp_checksum = struct.unpack('!HHHH', payload[:8])
            assert 8 <= udp_length == len(payload), 'Invalid outbound UDP length'
            if udp_checksum:
                assert checksum(pseudoheader(source, destination, 17, len(payload)) + payload) == 0, \
                    'Invalid outbound UDP checksum'
            self.count('udp')
            if source_port == 68 and destination_port == 67:
                self.dhcp(payload[8:])
            elif destination_port == 53:
                assert destination == SERVER_IP, 'Guest did not send DNS to its configured server'
                self.dns(source_port, payload[8:])
        elif protocol == 1:
            assert len(payload) >= 8 and checksum(payload) == 0, 'Invalid outbound ICMP checksum'
            if payload[0] == 8:
                self.count('icmp_echo')
                if destination != SERVER_IP:
                    return  # A routed but silent destination exercises bounded timeout.
                reply = b'\0' + payload[1:2] + b'\0\0' + payload[4:]
                reply = reply[:2] + struct.pack('!H', checksum(reply)) + reply[4:]
                # A corrupt reply must be ignored before the real reply arrives.
                self.send_ip(reply[:2] + b'\x12\x34' + reply[4:], 1)
                self.send_ip(reply, 1, delay=0.04)
            elif payload[0] == 0:
                assert destination == SERVER_IP, 'Echo reply went to the wrong destination'
                assert self.expected_echo_reply is not None, 'Unexpected unsolicited ICMP echo reply'
                assert payload == self.expected_echo_reply, \
                    'ICMP echo reply did not preserve code, identifier, sequence, or payload'
                self.expected_echo_reply = None
                self.count('icmp_reply')
        elif protocol == 6:
            assert len(payload) >= 20, 'Truncated outbound TCP header'
            assert checksum(pseudoheader(source, destination, 6, len(payload)) + payload) == 0, \
                'Invalid outbound TCP checksum'
            assert destination == SERVER_IP, 'Unexpected TCP destination'
            self.tcp(payload)

    def arp(self, packet):
        assert len(packet) >= 28, 'Truncated outbound ARP'
        hardware, protocol, hlen, plen, operation = struct.unpack('!HHBBH', packet[:8])
        assert (hardware, protocol, hlen, plen) == (1, 0x0800, 6, 4), 'Invalid outbound ARP'
        assert packet[8:14] == GUEST_MAC, 'ARP sender MAC mismatch'
        if operation == 1 and packet[24:28] == SERVER_IP:
            self.count('arp_request')
            response = struct.pack('!HHBBH', 1, 0x0800, 6, 4, 2)
            response += SERVER_MAC + SERVER_IP + GUEST_MAC + packet[14:18]
            self.send(ethernet(response, 0x0806))

    def dhcp(self, packet):
        assert len(packet) >= 240, 'Truncated outbound DHCP'
        assert packet[:3] == b'\x01\x01\x06', 'Invalid BOOTP request'
        assert packet[28:34] == GUEST_MAC, 'BOOTP hardware address mismatch'
        assert packet[236:240] == b'\x63\x82\x53\x63', 'Missing DHCP magic cookie'
        options = dhcp_options(packet[240:])
        kind = options.get(53)
        assert kind in (b'\x01', b'\x03'), f'Unexpected DHCP message type {kind!r}'
        if kind == b'\x01':
            self.count('dhcp_discover')
            response_kind = 2
        else:
            self.count('dhcp_request')
            assert options.get(50) == GUEST_IP or packet[12:16] == GUEST_IP, \
                'DHCP REQUEST did not request its offered lease'
            assert options.get(54, SERVER_IP) == SERVER_IP, 'DHCP REQUEST selected another server'
            if packet[12:16] == GUEST_IP:
                self.count('dhcp_renew')
                assert 50 not in options and 54 not in options, \
                    'DHCP renewal must use ciaddr instead of requested-IP/server-ID options'
            response_kind = 5
        if self.suppress_dhcp:
            self.count('dhcp_suppressed')
            return
        response = bytearray(240)
        response[:3] = b'\x02\x01\x06'
        response[4:8] = packet[4:8]
        response[10:12] = packet[10:12]
        response[16:20] = GUEST_IP
        response[20:24] = SERVER_IP
        response[28:44] = packet[28:44]
        response[236:240] = b'\x63\x82\x53\x63'
        response.extend(bytes((53, 1, response_kind, 54, 4)) + SERVER_IP)
        response.extend(bytes((1, 4)) + MASK + bytes((3, 4)) + SERVER_IP)
        response.extend(bytes((6, 4)) + SERVER_IP)
        response.extend(bytes((51, 4)) + struct.pack('!I', self.lease_seconds))
        response.extend(bytes((58, 4)) + struct.pack('!I', self.lease_seconds // 2))
        response.extend(bytes((59, 4)) + struct.pack('!I', self.lease_seconds * 7 // 8) + b'\xff')
        destination = b'\xff' * 4
        # A multihomed DHCP server may reply from a different local IPv4 address
        # than its option-54 identifier; the lease still selects SERVER_IP.
        datagram = udp(bytes(response), 67, 68, destination, source=DHCP_SOURCE_IP)
        self.send(ethernet(ipv4(datagram, 17, destination, source=DHCP_SOURCE_IP),
                           destination=BROADCAST_MAC))

    def dns(self, client_port, question):
        assert len(question) >= 17, 'Truncated outbound DNS question'
        ident, flags, qdcount, ancount, nscount, arcount = struct.unpack('!HHHHHH', question[:12])
        assert flags & 0x8000 == 0 and qdcount == 1, 'Invalid outbound DNS question'
        position = 12
        labels = []
        while position < len(question) and question[position]:
            length = question[position]
            assert length <= 63 and position + 1 + length <= len(question), 'Invalid DNS label'
            labels.append(question[position + 1:position + 1 + length].decode('ascii'))
            position += 1 + length
        assert position + 5 <= len(question), 'Truncated outbound DNS question type'
        assert question[position + 1:position + 5] == b'\0\x01\0\x01', 'Expected DNS A/IN lookup'
        name = '.'.join(labels)
        self.count('dns_query')
        self.count('dns_' + name)
        if name == 'silent.test':
            return
        echoed_question = question[12:position + 5]
        if name == 'missing.test':
            answer = struct.pack('!HHHHHH', ident, 0x8183, 1, 0, 0, 0) + echoed_question
        elif name == 'loop.test':
            # The owner name points to itself; a parser must terminate safely.
            offset = 12 + len(echoed_question)
            record = struct.pack('!HHHIH', 0xc000 | offset, 1, 1, 60, 4) + SERVER_IP
            answer = struct.pack('!HHHHHH', ident, 0x8180, 1, 1, 0, 0) + echoed_question + record
        else:
            assert name == 'guest.test', f'Unexpected DNS fixture query {name!r}'
            record = b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 60, 4) + SERVER_IP
            answer = struct.pack('!HHHHHH', ident, 0x8180, 1, 1, 0, 0) + echoed_question + record
            # Wrong transaction IDs and truncated unsolicited DNS are harmless.
            wrong = struct.pack('!H', ident ^ 0xffff) + answer[2:]
            self.send_ip(udp(wrong, 53, client_port), 17)
            self.send_ip(udp(b'\x00\x01\x80', 53, client_port), 17)
        self.send_ip(udp(answer, 53, client_port), 17, delay=0.04)

    def tcp_send(self, connection, payload=b'', flags=0x10, sequence=None, delay=0):
        if sequence is None:
            sequence = connection.send_next
        segment = tcp(payload, connection.server_port, connection.client_port,
                      sequence, connection.receive_next, flags)
        self.send_ip(segment, 6, delay=delay)

    def tcp(self, packet):
        client_port, server_port, sequence, ack = struct.unpack('!HHII', packet[:12])
        header_length, flags = (packet[12] >> 4) * 4, packet[13]
        assert 20 <= header_length <= len(packet), 'Invalid TCP data offset'
        assert server_port == 8080, f'Unexpected fixture TCP port {server_port}'
        payload = packet[header_length:]
        key = (client_port, server_port)
        if flags & 2:
            assert not payload, 'Guest attached HTTP request bytes to a TCP SYN/retransmission'
            self.count('tcp_syn')
            self.syns[key] = self.syns.get(key, 0) + 1
            if self.drop_first_syn and self.syns[key] == 1:
                self.count('tcp_syn_dropped')
                return
            connection = self.connections.setdefault(key, Connection(client_port, server_port,
                                                                       (sequence + 1) & 0xffffffff))
            self.tcp_send(connection, flags=0x12, sequence=connection.send_initial)
            return
        assert key in self.connections, 'TCP data arrived before SYN'
        connection = self.connections[key]
        if flags & 4:
            self.count('tcp_reset')
            return
        if payload:
            self.count('tcp_data')
            connection.request_packets += 1
            if self.drop_first_request and connection.request_packets == 1:
                self.count('tcp_request_dropped')
                return
            if sequence == connection.receive_next:
                connection.request.extend(payload)
                connection.receive_next = (connection.receive_next + len(payload)) & 0xffffffff
            else:
                assert sequence < connection.receive_next, 'Guest sent a TCP sequence gap'
            self.tcp_send(connection)
            if b'\r\n\r\n' in connection.request and not connection.response_sent:
                request = bytes(connection.request)
                assert request.startswith(b'GET / HTTP/1.'), f'Unexpected HTTP request: {request!r}'
                assert b'\r\nHost: guest.test' in request, 'HTTP request omitted its Host header'
                self.requests.append(request)
                connection.response_sent = True
                # Split in the status line, header terminator, and body; resend
                # one segment to verify duplicate sequence numbers aren't printed.
                boundaries = (0, 9, 52, HTTP_RESPONSE.index(b'\r\n\r\n') + 3,
                              len(HTTP_RESPONSE) - 18, len(HTTP_RESPONSE))
                for index, (start, stop) in enumerate(zip(boundaries, boundaries[1:])):
                    segment_sequence = connection.send_next + start
                    self.tcp_send(connection, HTTP_RESPONSE[start:stop], 0x18,
                                  segment_sequence, delay=0.03 * (index + 1))
                    if index == 1:
                        self.tcp_send(connection, HTTP_RESPONSE[start:stop], 0x18,
                                      segment_sequence, delay=0.03 * (index + 1) + 0.005)
                connection.send_next += len(HTTP_RESPONSE)
                self.tcp_send(connection, flags=0x11, delay=0.21)
                connection.send_next += 1
                self.count('http_response')
        if flags & 1:
            if sequence == connection.receive_next:
                connection.receive_next = (connection.receive_next + len(payload) + 1) & 0xffffffff
            connection.peer_closed = True
            self.count('tcp_fin')
            self.tcp_send(connection)

    def stress_receive_ring(self):
        # More than 64 KiB forces ring wrap. Invalid IP checksums, oversized
        # total lengths, short headers, and unknown EtherTypes must be discarded.
        for index in range(96):
            packet = bytearray(ipv4(bytes(1100), 253))
            packet[10] ^= 0x80
            self.send(ethernet(bytes(packet)), delay=index * 0.002)
        malformed = bytearray(ipv4(b'\0' * 8, 17))
        malformed[2:4] = b'\xff\xff'
        malformed[10:12] = b'\0\0'
        malformed[10:12] = struct.pack('!H', checksum(bytes(malformed[:20])))
        self.send(ethernet(bytes(malformed)), delay=0.20)
        self.send(ethernet(b'\x45\x00\x00'), delay=0.21)
        self.send(ethernet(b'ignored' * 200, 0x88b5), delay=0.22)

    def inject_echo_request(self):
        request = struct.pack('!BBHHH', 8, 0, 0, 0x5151, 0x3434) + b'OSp inbound echo fixture'
        request = request[:2] + struct.pack('!H', checksum(request)) + request[4:]
        reply = b'\0\0\0\0' + request[4:]
        self.expected_echo_reply = reply[:2] + struct.pack('!H', checksum(reply)) + reply[4:]
        self.send_ip(request, 1)

    def close(self):
        self.stopped.set()
        if self.peer:
            try:
                self.peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.peer.close()
        self.listener.close()
        self.thread.join(timeout=2)


class NetworkMachine(Machine):
    def __init__(self, qemu, image, directory, network_arguments):
        self.directory = Path(directory)
        self.log = self.directory / 'console.log'
        self.socket_path = self.directory / 'qmp.sock'
        self.stderr = (self.directory / 'qemu.log').open('wb')
        self.process = subprocess.Popen([
            qemu, '-machine', 'pc', '-accel', 'tcg', '-m', '32M',
            '-drive', f'file={image},format=raw,if=floppy,readonly=on',
            '-boot', 'order=a', *network_arguments, '-display', 'none',
            '-serial', 'none', '-monitor', 'none', '-debugcon', f'file:{self.log}',
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

    def shell(self, command, expected=None, timeout=20):
        start = len(self.output())
        self.type(command + '\n')
        output = self.wait_for('osp> ', since=start, timeout=timeout)
        if expected is not None:
            assert expected in output, (expected, output)
        return output


def expect_failure(output):
    assert any(word in output.lower() for word in
               ('failed', 'timeout', 'timed out', 'unavailable', 'no network', 'no nic',
                'not configured', 'no adapter', 'no rtl8139 adapter', 'not found',
                'no ipv4 answer', 'no ipv4 address', 'down',
                'invalid', 'error')), output


def wait_for_count(fixture, name, minimum, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        fixture.assert_healthy()
        if fixture.counts.get(name, 0) >= minimum:
            return
        time.sleep(0.02)
    raise AssertionError(f'Expected {name} count >= {minimum}; fixture counters: {fixture.counts}')


def exercise_lease_lifecycle(machine, fixture):
    fixture.lease_seconds = 4
    renewals = fixture.counts.get('dhcp_renew', 0)
    machine.shell('dhcp', 'DHCP: lease acquired')
    wait_for_count(fixture, 'dhcp_renew', renewals + 1)
    # Allow the scheduled ACK to reach the guest before querying its state.
    time.sleep(0.10)
    output = machine.shell('net', 'Configuration: DHCP')
    assert 'IPv4: 10.0.2.15' in output, output
    machine.shell('ping 10.0.2.2', 'Reply from 10.0.2.2')

    discoveries = fixture.counts.get('dhcp_discover', 0)
    fixture.suppress_dhcp = True
    wait_for_count(fixture, 'dhcp_discover', discoveries + 1, timeout=7)
    output = machine.shell('net', 'IPv4: 0.0.0.0')
    assert 'Configuration: DHCP pending' in output or 'Configuration: unconfigured' in output, output
    expect_failure(machine.shell('ping 10.0.2.2'))
    assert fixture.counts.get('dhcp_suppressed', 0) >= 2, fixture.counts
    fixture.lease_seconds = 3600
    fixture.suppress_dhcp = False
    machine.shell('dhcp', 'DHCP: lease acquired')
    machine.shell('ping 10.0.2.2', 'Reply from 10.0.2.2')
    fixture.assert_healthy()
    print('PASS automatic DHCP ciaddr renewal, lease expiry without server, and reacquisition', flush=True)


def exercise_fixture(machine, fixture):
    machine.wait_for('osp> ', timeout=15)
    fixture.assert_healthy()
    output = machine.shell('net')
    for expected in ('IPv4: 10.0.2.15', 'Mask: 255.255.255.0',
                     'Gateway: 10.0.2.2', 'DNS: 10.0.2.2'):
        assert expected in output, (expected, output)
    assert fixture.counts.get('dhcp_discover', 0) >= 1, fixture.counts
    assert fixture.counts.get('dhcp_request', 0) >= 1, fixture.counts
    machine.shell('dhcp', 'DHCP: lease acquired')
    print('PASS RTL8139 boot discovery and DHCP lease/reacquisition with guest Ethernet traffic', flush=True)

    machine.shell('ping 10.0.2.2', 'Reply from 10.0.2.2')
    assert fixture.counts.get('arp_request', 0) >= 1, fixture.counts
    assert fixture.counts.get('icmp_echo', 0) >= 1, fixture.counts
    machine.shell('nslookup guest.test', 'guest.test = 10.0.2.2')
    fixture.assert_healthy()
    print('PASS ARP, ICMP echo, DNS compressed A record, and packet checksum validation', flush=True)

    output = machine.shell('http http://guest.test:8080/', 'HTTP: connection closed')
    assert 'HTTP/1.0 200 OK' in output, output
    assert output.count(HTTP_BODY.decode()) == 1, output
    assert fixture.counts.get('tcp_syn', 0) >= 2, fixture.counts
    assert fixture.counts.get('tcp_request_dropped', 0) == 1, fixture.counts
    assert fixture.counts.get('tcp_data', 0) >= 2, fixture.counts
    assert len(fixture.requests) == 1, fixture.requests
    fixture.assert_healthy()
    print('PASS HTTP over TCP with SYN/request loss, retransmission, segmentation, duplicate data, FIN', flush=True)

    expect_failure(machine.shell('nslookup missing.test'))
    expect_failure(machine.shell('nslookup loop.test'))
    machine.shell('nslookup guest.test', 'guest.test = 10.0.2.2')
    fixture.stress_receive_ring()
    time.sleep(0.30)
    machine.shell('ping 10.0.2.2', 'Reply from 10.0.2.2')
    fixture.assert_healthy()
    print('PASS NXDOMAIN, malformed DNS compression, truncated IP, bad checksums, RX ring wrap', flush=True)

    output = machine.shell('ifconfig 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.2',
                           'Static IPv4 configuration applied')
    arp_requests = fixture.counts.get('arp_request', 0)
    echo_replies = fixture.counts.get('icmp_reply', 0)
    fixture.inject_echo_request()
    wait_for_count(fixture, 'icmp_reply', echo_replies + 1)
    assert fixture.counts.get('arp_request', 0) > arp_requests, \
        'Guest replied to inbound echo without resolving its empty ARP cache'
    print('PASS inbound ICMP echo resolves uncached peer and preserves identifier/sequence/payload', flush=True)
    expect_failure(machine.shell('ping 203.0.113.9'))
    expect_failure(machine.shell('nslookup silent.test'))
    machine.shell('ping 10.0.2.2', 'Reply from 10.0.2.2')
    machine.shell('echo networkalive', '\nnetworkalive\n')
    fixture.assert_healthy()
    print('PASS static address, gateway routing, bounded ICMP/DNS timeouts, shell recovery', flush=True)

    machine.command('set_link', {'name': 'n0', 'up': False})
    try:
        expect_failure(machine.shell('ping 10.0.2.2'))
    finally:
        machine.command('set_link', {'name': 'n0', 'up': True})
    machine.shell('ping 10.0.2.2', 'Reply from 10.0.2.2')
    fixture.assert_healthy()
    print('PASS link loss and recovery', flush=True)
    exercise_lease_lifecycle(machine, fixture)


def exercise_without_nic(machine):
    machine.wait_for('osp> ', timeout=15)
    output = machine.shell('net')
    assert any(word in output.lower() for word in ('unavailable', 'not found', 'no rtl', 'no nic',
                                                  'no network', 'no adapter')), output
    for command in ('dhcp', 'ping 10.0.2.2', 'nslookup guest.test', 'http http://guest.test:8080/'):
        expect_failure(machine.shell(command))
    machine.shell('echo nonicalive', '\nnonicalive\n')
    print('PASS no-NIC status, command failures, and responsive shell', flush=True)


def exercise_live(machine):
    machine.wait_for('osp> ', timeout=15)
    machine.shell('net', 'IPv4: 10.0.2.15')
    machine.shell('ping 10.0.2.2', 'Reply from 10.0.2.2')
    output = machine.shell('nslookup example.com', 'example.com = ')
    address = output.split('example.com = ', 1)[1].split()[0]
    ipaddress.IPv4Address(address)
    output = machine.shell('http http://example.com/', 'HTTP response:', timeout=25)
    assert 'HTTP/1.0 ' in output or 'HTTP/1.1 ' in output, output
    assert 'Example Domain' in output, output
    machine.command('screendump', {'filename': str(BUILD / 'osp-network-screen.png'), 'format': 'png'})
    print('PASS live external DNS and example.com HTTP through the guest network stack', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default=os.environ.get('QEMU', 'qemu-system-i386'))
    parser.add_argument('--live', action='store_true', help='also test real internet through QEMU user networking')
    args = parser.parse_args()
    qemu = shutil.which(args.qemu)
    if not qemu:
        print(f'Missing {args.qemu}; install QEMU to run networking tests.', file=sys.stderr)
        return 1
    image = BUILD / 'osp.img'
    if not image.is_file():
        print('Missing build/osp.img; run make first.', file=sys.stderr)
        return 1
    machine = None
    fixture = None
    fixture_errors = []
    try:
        with tempfile.TemporaryDirectory(prefix='osp-network-') as directory:
            temporary = Path(directory)
            fixture = EthernetFixture()
            fixture_dir = temporary / 'fixture'
            fixture_dir.mkdir()
            machine = NetworkMachine(qemu, image, fixture_dir, [
                '-netdev', f'socket,id=n0,connect=127.0.0.1:{fixture.port}',
                '-device', 'rtl8139,netdev=n0,mac=52:54:00:12:34:56',
            ])
            try:
                assert fixture.connected.wait(5), 'QEMU did not connect to Ethernet fixture'
                exercise_fixture(machine, fixture)
            finally:
                (BUILD / 'network-console.log').write_text(machine.output())
                machine.close()
                machine = None
                fixture_errors.extend(fixture.errors)
                fixture.close()
                fixture = None
            no_nic_dir = temporary / 'no-nic'
            no_nic_dir.mkdir()
            machine = NetworkMachine(qemu, image, no_nic_dir, ['-nic', 'none'])
            try:
                exercise_without_nic(machine)
            finally:
                (BUILD / 'network-no-nic-console.log').write_text(machine.output())
                machine.close()
                machine = None
            if args.live:
                live_dir = temporary / 'live'
                live_dir.mkdir()
                machine = NetworkMachine(qemu, image, live_dir, [
                    '-netdev', 'user,id=n0,net=10.0.2.0/24',
                    '-device', 'rtl8139,netdev=n0,mac=52:54:00:12:34:56',
                ])
                try:
                    exercise_live(machine)
                finally:
                    (BUILD / 'network-live-console.log').write_text(machine.output())
                    machine.close()
                    machine = None
    except (AssertionError, RuntimeError, OSError, ValueError) as error:
        if fixture and fixture.errors:
            fixture_errors.extend(fixture.errors)
        if fixture_errors:
            print('\n'.join(fixture_errors), file=sys.stderr)
        print(f'FAIL {error}', file=sys.stderr)
        return 1
    finally:
        if machine:
            machine.close()
        if fixture:
            fixture.close()
    print('All networking checks passed.', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
