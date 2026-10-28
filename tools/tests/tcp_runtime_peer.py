#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Loopback-only, byte-checking peer for tcp_runtime_probe.c.

Run with a local port number. Port zero chooses an ephemeral port. Three
connections verify ordinary delivery, a reply after the peer's write half-close,
and a final close. The optional QEMU gate additionally proves that queued data
survives the sending child's exit; plain loopback mode proves only byte delivery.
No bytes are accepted as correct solely because the guest printed PASS.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import re
import socket
import stat
import struct
import sys
import time
import uuid


def wait_receipt(serial, offset, ready, timeout):
    """Read a complete new receipt without accepting echoed or stale commands."""
    deadline = time.monotonic() + timeout
    pattern = (rb'(?m)^TCP_CLOSED_OWNER_READY port=([0-9]+) child=([0-9]+)\r?\n' if ready else
               rb'(?m)^TCP_CLOSED_OWNER_EXITED bytes=([0-9]+) port=([0-9]+) child=([0-9]+)\r?\n')
    while True:
        matches = re.findall(pattern, serial.read_bytes()[offset:])
        if matches:
            if len(matches) != 1:
                raise ValueError('ambiguous owner-exit receipts')
            fields = dict(zip(('port', 'child') if ready else ('bytes', 'port', 'child'),
                              map(int, matches[0])))
            count, port, child = fields.get('bytes', 1), fields['port'], fields['child']
            if not (0 < count <= 65535 and 0 < port <= 65535 and child > 0):
                raise ValueError('invalid owner-exit receipt')
            return fields
        if time.monotonic() >= deadline:
            raise TimeoutError('no fresh complete owner-exit receipt')
        time.sleep(.01)


def wait_owner_exit(serial, offset, timeout=15):
    """Read the parent receipt printed only after the child's successful waitpid."""
    return wait_receipt(serial, offset, False, timeout)


def capture_log_digest(path):
    """Reject a capture that stopped after any complete or partial record."""
    data = path.read_bytes()
    if b'network dump write error' in data or b'net dump write error' in data:
        raise ValueError('QEMU capture stopped after a write failure')
    return hashlib.sha256(data).hexdigest()


def emitted_payload(data, source_port, destination_port, accepted):
    """Parse a complete Ethernet PCAP snapshot and deduplicate this TCP flow."""
    if len(data) < 24:
        raise ValueError('truncated PCAP header')
    magic = data[:4]
    if magic in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1'):
        endian = '<'
    elif magic in (b'\xa1\xb2\xc3\xd4', b'\xa1\xb2\x3c\x4d'):
        endian = '>'
    else:
        raise ValueError('unsupported PCAP format')
    major, minor, _, _, snaplen, network = struct.unpack(endian + 'HHIIII', data[4:24])
    if (major, minor, network) != (2, 4, 1) or not snaplen:
        raise ValueError('expected Ethernet PCAP 2.4')
    offset, isn, packets, intervals = 24, None, 0, []
    while offset < len(data):
        if len(data) - offset < 16:
            raise ValueError('truncated PCAP record')
        _, _, included, original = struct.unpack_from(endian + 'IIII', data, offset)
        offset += 16
        if included != original or included > snaplen or included > len(data) - offset:
            raise ValueError('truncated PCAP packet')
        frame = data[offset:offset + included]
        offset += included
        if len(frame) < 14 or frame[12:14] != b'\x08\x00':
            continue
        ip = frame[14:]
        if len(ip) < 20 or ip[0] >> 4 != 4 or ip[9] != 6:
            continue
        if ip[12:16] != socket.inet_aton('10.0.2.15') or ip[16:20] != socket.inet_aton('10.0.2.2'):
            continue
        header, total, fragment = (ip[0] & 15) * 4, int.from_bytes(ip[2:4], 'big'), int.from_bytes(ip[6:8], 'big')
        if header < 20 or total < header + 20 or total > len(ip) or fragment & 0x3fff:
            raise ValueError('invalid or fragmented captured TCP packet')
        tcp = ip[header:total]
        if struct.unpack_from('!HH', tcp) != (source_port, destination_port):
            continue
        tcp_header, flags = (tcp[12] >> 4) * 4, tcp[13]
        sequence = int.from_bytes(tcp[4:8], 'big')
        if tcp_header < 20 or tcp_header > len(tcp):
            raise ValueError('invalid TCP header')
        if flags & 2:
            if isn is not None and isn != sequence:
                raise ValueError('ambiguous captured TCP connection')
            isn = sequence
        payload = len(tcp) - tcp_header
        if payload:
            if isn is None:
                raise ValueError('payload precedes captured SYN')
            start = (sequence + bool(flags & 2) - isn - 1) & 0xffffffff
            if start + payload > accepted:
                raise ValueError('captured payload exceeds accepted bytes')
            intervals.append((start, start + payload))
            packets += 1
    if isn is None or not intervals:
        raise ValueError('no captured SYN and payload for the exited sender')
    unique, end = 0, 0
    for start, stop in sorted(intervals):
        unique += max(0, stop - max(start, end))
        end = max(end, stop)
    return {'unique_bytes': unique, 'payload_packets': packets}


class Qmp:
    """Bounded QMP connection on a dedicated, caller-owned Unix socket."""
    def __init__(self, path):
        if not stat.S_ISSOCK(path.stat().st_mode):
            raise ValueError('QMP path is not a Unix socket')
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(5)
        self.socket.connect(str(path))
        self.stream = self.socket.makefile('rwb', buffering=0)
        self.commands = []
        self.version = self.read()['QMP']['version']
        self.command('qmp_capabilities')

    def read(self):
        line = self.stream.readline(1024 * 1024)
        if not line.endswith(b'\n'):
            raise ValueError('incomplete QMP response')
        return json.loads(line)

    def command(self, name, arguments=None):
        request = {'execute': name, 'id': len(self.commands)}
        if arguments is not None:
            request['arguments'] = arguments
        self.commands.append(request)
        self.socket.sendall(json.dumps(request).encode() + b'\n')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            response = self.read()
            if response.get('id') == request['id']:
                if 'error' in response or 'return' not in response:
                    raise ValueError(f'QMP {name} failed: {response}')
                return response['return']
        raise TimeoutError(f'QMP {name} response timed out')

    def close(self):
        self.stream.close()
        self.socket.close()


@contextmanager
def dropped_output(qmp):
    """Discard guest-to-Slirp frames; null retains no frames to replay later."""
    name = 'tcp-exit-' + uuid.uuid4().hex[:12]
    qmp.command('chardev-add', {'id': name, 'backend': {'type': 'null', 'data': {}}})
    added = False
    try:
        qmp.command('object-add', {'qom-type': 'filter-redirector', 'id': name,
            'netdev': 'net0', 'queue': 'rx', 'position': 'head', 'outdev': name})
        added = True
        path = '/objects/' + name
        for key, expected in [('status', 'on'), ('queue', 'rx'), ('netdev', 'net0'), ('outdev', name)]:
            if qmp.command('qom-get', {'path': path, 'property': key}) != expected:
                raise ValueError(f'wrong drop filter {key}')
        yield path
    finally:
        if added:
            qmp.command('object-del', {'id': name})
        qmp.command('chardev-remove', {'id': name})


def exited_owner(connection, port, args, proof, serial_offset):
    """Require dropped, unacknowledged data at child exit, then actual delivery."""
    qmp = Qmp(args.qmp)
    try:
        if not qmp.command('query-status')['running']:
            raise ValueError('QEMU must be running before the TCP gate')
        ready = wait_receipt(args.serial, serial_offset, True, 5)
        with dropped_output(qmp) as path:
            offset = args.serial.stat().st_size
            connection.sendall(b'G')
            marker = wait_owner_exit(args.serial, offset)
            if any(marker[key] != ready[key] for key in ('port', 'child')):
                raise ValueError('ready and exited sender identities differ')
            # Pausing makes the capture a complete stable snapshot; it also
            # prevents another retransmit racing the pre-release inspection.
            qmp.command('stop')
            try:
                if qmp.command('query-status')['running']:
                    raise ValueError('QEMU did not pause for the capture')
                if qmp.command('qom-get', {'path': path, 'property': 'status'}) != 'on':
                    raise ValueError('drop filter was released before owner exit')
                capture_log_sha256 = capture_log_digest(args.qemu_log)
                emitted = emitted_payload(args.pcap.read_bytes(), marker['port'], port, marker['bytes'])
                if not 0 < emitted['unique_bytes'] < marker['bytes']:
                    raise ValueError('no proof of nonempty unsent data at owner exit')
                connection.setblocking(False)
                try:
                    connection.recv(1, socket.MSG_PEEK)
                except BlockingIOError:
                    pass
                else:
                    raise ValueError('payload or EOF arrived while output was discarded')
                finally:
                    connection.settimeout(30)
            finally:
                qmp.command('cont')
            qmp.command('qom-set', {'path': path, 'property': 'status', 'value': 'off'})
            if qmp.command('qom-get', {'path': path, 'property': 'status'}) != 'off':
                raise ValueError('drop filter did not release')
            checksum = receive_payload(connection, marker['bytes'])
            verify_close(connection)
            proof.update(owner_exit=marker, pre_release=emitted,
                sender_ready=ready, paused_capture_log_sha256=capture_log_sha256,
                peer_bytes_before_release=0, received_bytes=marker['bytes'],
                fnv1a64=f'{checksum:016x}', orderly_eof=True,
                method='QEMU net0 RX redirector to null; no host replay buffer; release after waitpid receipt')
        return marker['bytes'], checksum
    finally:
        proof.update(qmp_version=qmp.version, qmp_commands=qmp.commands)
        qmp.close()


def receive_payload(connection, size):
    """Check every ordered payload byte and return its 64-bit FNV-1a digest."""
    received = 0
    checksum = 14695981039346656037
    pattern = bytes((number * 29 + 7) & 255 for number in range(256))
    while received < size:
        part = connection.recv(min(16384, size - received))
        if not part:
            raise ValueError(f'short payload: {received}/{size} bytes')
        start = received % 256
        expected = (pattern * ((start + len(part) + 255) // 256))[start:start + len(part)]
        if part != expected:
            raise ValueError(f'payload mismatch at or after byte {received}')
        for byte in part:
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        received += len(part)
    return checksum


def verify_close(connection):
    """Require orderly EOF and reject unexpected trailing bytes."""
    if connection.recv(1):
        raise ValueError('payload exceeds the expected size')


def serve(port, args=None, proof=None):
    """Serve the three fixed contracts on IPv4 loopback with bounded waits."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        serial_offset = args.serial.stat().st_size if args is not None else 0
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', port))
        listener.listen(3)
        print(f'TCP_PEER_READY {listener.getsockname()[1]}', flush=True)
        for case, size in [(1, 2097152), (2, 8192), (3, 65535)]:
            # The first command is entered through the existing paced QMP
            # keyboard harness. Only this pre-connection wait includes typing;
            # all established I/O and later connections retain a 30-second bound.
            listener.settimeout(120 if case == 1 else 30)
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(30)
                if case == 2:
                    connection.sendall(b'peer-fin')
                    connection.shutdown(socket.SHUT_WR)
                if case == 3 and args is not None:
                    size, checksum = exited_owner(connection, listener.getsockname()[1], args, proof, serial_offset)
                else:
                    if case == 3:
                        connection.sendall(b'G')
                    checksum = receive_payload(connection, size)
                if case == 1:
                    connection.sendall(checksum.to_bytes(8, 'big'))
                if case != 3 or args is None:
                    verify_close(connection)
                print(f'TCP_PEER_VERIFIED case={case} bytes={size} fnv1a64={checksum:016x}',
                      flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port', type=int)
    for name in ('qmp', 'serial', 'pcap', 'qemu-log', 'proof'):
        parser.add_argument('--' + name, type=Path)
    args = parser.parse_args()
    if not 0 <= args.port <= 65535:
        parser.error('port must be between 0 and 65535')
    paths = [args.qmp, args.serial, args.pcap, args.qemu_log, args.proof]
    strict = any(paths)
    if strict and (not all(paths) or not all(path.is_absolute() for path in paths)):
        parser.error('--qmp, --serial, --pcap, --qemu-log and --proof require five absolute paths')
    if strict and args.proof.exists():
        parser.error('proof output already exists')
    proof = {'passed': False}
    result = 0
    try:
        serve(args.port, args if strict else None, proof)
        proof['passed'] = True
    except (OSError, ValueError, KeyError) as error:
        proof['error'] = str(error)
        print(f'TCP_PEER_ERROR: {error}', file=sys.stderr, flush=True)
        result = 1
    finally:
        if strict:
            with args.proof.open('x') as stream:
                json.dump(proof, stream, indent=2)
                stream.write('\n')
    return result


if __name__ == '__main__':
    raise SystemExit(main())
