# SPDX-License-Identifier: Apache-2.0
"""Independent loopback clients exercise the native TCP probe's host oracle."""
from pathlib import Path
import importlib.util
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

PEER = Path(__file__).resolve().parent / 'tests/tcp_runtime_peer.py'
spec = importlib.util.spec_from_file_location('tcp_peer', PEER)
peer_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(peer_module)


class PeerTests(unittest.TestCase):
    def test_capture_write_failure_fails_even_at_complete_record_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'qemu.log'
            log.write_text('qemu: network dump write error - stopping dump\n')
            with self.assertRaisesRegex(ValueError, 'capture stopped'):
                peer_module.capture_log_digest(log)
            log.write_bytes(b'')
            self.assertEqual(len(peer_module.capture_log_digest(log)), 64)

    def test_owner_exit_marker_must_be_fresh_and_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            serial = Path(directory) / 'serial'
            old = b'TCP_CLOSED_OWNER_EXITED bytes=65535 port=41000 child=17\n'
            serial.write_bytes(old + b'TCP_CLOSED_OWNER_EXITED bytes=31 port=41001 child=18')
            with self.assertRaises(TimeoutError):
                peer_module.wait_owner_exit(serial, len(old), timeout=0)
            with serial.open('ab') as stream:
                stream.write(b'\n')
            self.assertEqual(peer_module.wait_owner_exit(serial, len(old), timeout=0),
                             {'bytes': 31, 'port': 41001, 'child': 18})

    def test_capture_counts_unique_payload_and_rejects_truncation(self):
        def frame(sequence, flags, payload=b''):
            ethernet = b'\0' * 12 + b'\x08\x00'
            ip = bytearray(20)
            ip[0], ip[9] = 0x45, 6
            struct.pack_into('!H', ip, 2, 40 + len(payload))
            ip[12:16], ip[16:20] = socket.inet_aton('10.0.2.15'), socket.inet_aton('10.0.2.2')
            tcp = struct.pack('!HHIIBBHHH', 41001, 19876, sequence, 0, 0x50, flags, 65535, 0, 0)
            return ethernet + ip + tcp + payload
        packets = [frame(0xfffffff0, 2), frame(0xfffffff1, 0x18, b'1234'),
                   frame(0xfffffff1, 0x18, b'1234'), frame(0xfffffff5, 0x18, b'5678')]
        data = struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1)
        for packet in packets:
            data += struct.pack('<IIII', 1, 0, len(packet), len(packet)) + packet
        result = peer_module.emitted_payload(data, 41001, 19876, 31)
        self.assertEqual(result, {'unique_bytes': 8, 'payload_packets': 3})
        with self.assertRaisesRegex(ValueError, 'truncated'):
            peer_module.emitted_payload(data[:-1], 41001, 19876, 31)
        with self.assertRaises(ValueError):
            peer_module.emitted_payload(data, 41002, 19876, 31)

    def start_peer(self):
        child = subprocess.Popen([sys.executable, str(PEER), '0'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True)
        self.addCleanup(self.stop_peer, child)
        ready = child.stdout.readline().strip()
        self.assertTrue(ready.startswith('TCP_PEER_READY '), ready)
        return child, int(ready.split()[1])

    @staticmethod
    def stop_peer(child):
        if child.poll() is None:
            child.terminate()
        child.communicate(timeout=5)

    def test_complete_data_half_close_and_closed_owner(self):
        child, port = self.start_peer()
        tile = bytes(((number * 29) + 7) % 256 for number in range(256))
        full = tile * 8192
        with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
            for offset in range(0, len(full), 12347):
                client.sendall(full[offset:offset + 12347])
            response = bytearray()
            while len(response) < 8:
                part = client.recv(8 - len(response))
                self.assertTrue(part, 'peer closed before the complete checksum')
                response.extend(part)
            expected = 14695981039346656037
            for byte in full:
                expected = ((expected ^ byte) * 1099511628211) % (1 << 64)
            self.assertEqual(int.from_bytes(response, 'big'), expected)
        with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
            message = bytearray()
            while True:
                part = client.recv(8)
                if not part:
                    break
                message.extend(part)
            self.assertEqual(message, b'peer-fin')
            client.sendall(tile * 32)
            client.shutdown(socket.SHUT_WR)
        with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
            self.assertEqual(client.recv(1), b'G')
            client.sendall((tile * 256)[:-1])
        stdout, stderr = child.communicate(timeout=5)
        self.assertEqual(child.returncode, 0, stderr)
        for case, size in [(1, 2097152), (2, 8192), (3, 65535)]:
            self.assertIn(f'TCP_PEER_VERIFIED case={case} bytes={size} ', stdout)

    def test_corrupted_bytes_fail(self):
        child, port = self.start_peer()
        with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
            client.sendall(b'\x08' + b'\x00' * 255)
            client.shutdown(socket.SHUT_WR)
        stdout, stderr = child.communicate(timeout=5)
        self.assertNotEqual(child.returncode, 0)
        self.assertNotIn('TCP_PEER_VERIFIED', stdout)
        self.assertIn('payload mismatch', stderr)

    def test_short_read_fails(self):
        child, port = self.start_peer()
        with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
            client.sendall(bytes(((number * 29) + 7) % 256 for number in range(31)))
            client.shutdown(socket.SHUT_WR)
        stdout, stderr = child.communicate(timeout=5)
        self.assertNotEqual(child.returncode, 0)
        self.assertNotIn('TCP_PEER_VERIFIED', stdout)
        self.assertIn('short payload', stderr)


if __name__ == '__main__':
    unittest.main()
