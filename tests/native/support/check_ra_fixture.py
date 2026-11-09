#!/usr/bin/env python3
"""Compare raw HTTP replies and request logs; delete with Python and ra_fixture_probe.cpp.

Compile the probe without the client or its HTTP wrappers:
  c++ -std=c++17 -pthread -Itests/native/support tests/native/support/ra_fixture_probe.cpp -o build/ra_fixture_probe
  python tests/native/support/check_ra_fixture.py build/ra_fixture_probe
Only wall-clock time, the Python server-version header and the advertised ephemeral
origin are fixed, identically on both sides. No response bytes are removed or rewritten.
"""
import argparse
from http.server import BaseHTTPRequestHandler
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT), str(ROOT / 'tests')]
from ra_mock import mock_service
from test_host_player import disc


def request(port, method, path, body):
    wire = (f'{method} {path} HTTP/1.1\r\nHost: localhost\r\nUser-Agent: fixture-equivalence\r\n'
            f'Content-Length: {len(body)}\r\nConnection: close\r\n\r\n').encode() + body
    with socket.create_connection(('127.0.0.1', port), timeout=2) as peer:
        peer.sendall(wire)
        out = bytearray()
        while chunk := peer.recv(65536):
            out.extend(chunk)
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe', type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve()
    for text in ['Display:\n' + '🐱' * 400 + '\n', 'Display:\nx' + '€' * 400 + '\n',
                 'Display:\na' + 'é' * 600 + '\n', '\b\f\n\r\t"\\\x7f']:
        actual = subprocess.check_output([str(probe), '--quote', text], text=True).rstrip('\n')
        assert actual == json.dumps(text), (actual, json.dumps(text))
    print('rich-presence patches: ASCII escapes and surrogate pairs match json.dumps byte for byte')
    server = f'{BaseHTTPRequestHandler.server_version} {BaseHTTPRequestHandler.sys_version}'
    operations = [b'r=login2&u=TestUser&p=test-password', b'r=login2&u=TestUser&t=test-token',
                  b'r=login2&u=TestUser&p=wrong', b'r=achievementsets&g=1234', b'r=startsession&g=1234',
                  b'r=awardachievement&a=101&h=0', b'r=gameid&m=synthetic', b'r=ping', b'r=unexpected',
                  b'r=login2&u=Test%55ser&p=test-password&empty=&r=ignored&plus=a+b&encoded=%26%3D']
    count = 0
    with patch('time.time', return_value=1700000000):
        for scenario in ['ok', 'offline', 'blocked', 'blocked-json', 'expired', 'unknown', 'set-failure', 'award-offline', 'delay', 'renewed']:
            with mock_service() as service, subprocess.Popen([str(probe), scenario, server], stdin=subprocess.PIPE,
                                                          stdout=subprocess.PIPE, text=True) as native:
                try:
                    port = int(native.stdout.readline())
                    python_port = int(service.url.rsplit(':', 1)[1])
                    service.url = 'http://127.0.0.1:12345'
                    if scenario == 'unknown': service.unknown = True
                    elif scenario == 'set-failure': service.set_failure = True
                    elif scenario == 'award-offline': service.award_mode = 'offline'
                    elif scenario == 'delay': service.delay = .4
                    elif scenario == 'renewed': service.token = 'renewed-test-token'
                    else: service.mode = scenario
                    for body in operations + ([b'r=login2&u=TestUser&t=renewed-test-token'] if scenario == 'renewed' else []):
                        a = request(port, 'POST', '/dorequest.php', body)
                        b = request(python_port, 'POST', '/dorequest.php', body)
                        assert a == b, (scenario, body, a, b)
                        count += 1
                    for path in ['/Badge/101.png', '/Images/1.png', '/arbitrary']:
                        assert request(port, 'GET', path, b'') == request(python_port, 'GET', path, b''), (scenario, path)
                        count += 1
                    native.stdin.write('log\n'); native.stdin.flush()
                    log = json.loads(native.stdout.readline())
                    expected = {'requests': [[p, q, a] for p, q, a in service.requests],
                                'awards': service.awards, 'badge_requests': service.badge_requests}
                    assert log == expected, (scenario, log, expected)
                    print(f'{scenario}: raw response bytes and full request/award/badge logs match')
                finally:
                    native.stdin.write('quit\n'); native.stdin.flush()
                    assert native.wait(timeout=5) == 0
    with tempfile.TemporaryDirectory() as folder:
        tmp = Path(folder)
        for n in range(1, 5):
            expected, actual = disc(tmp / 'python.bin', n), tmp / 'native.bin'
            subprocess.run([str(probe), '--disc', str(actual), str(n)], check=True)
            assert actual.read_bytes() == expected.read_bytes(), n
        print('synthetic discs 1–4: every byte matches test_host_player.disc, including Random(14)')
    print(f'PASS: {count} raw HTTP responses, 10 scripted configurations, 4 synthetic discs')


if __name__ == '__main__':
    main()
